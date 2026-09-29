#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDate>
#include <QFile>
#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStyleFactory>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <memory>

#include <spdlog/spdlog.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/daily_file_sink.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <fstream>
#include <iomanip>

#include "Config/ConfigPaths.h"
#include "UI/MainWindow.h"
#include "HAL/core/HardwareManager.h"

// 控制台信号处理器：直接关闭 cmd 窗口（CTRL_CLOSE_EVENT）或 Ctrl+C/Ctrl+Break
// 属于 Windows 强杀进程，不经过 Qt 事件循环，QCoreApplication::aboutToQuit
// 不会触发。必须在此时对舵机/卡断使能，否则软件关闭后硬件仍带电。
// 注意：本回调运行在独立的控制台信号线程，只能做线程安全的硬件层操作
// （HardwareManager::ShutdownHalt 不触碰 Qt 对象）。
static BOOL WINAPI ConsoleSignalHandler(DWORD ctrlType)
{
    SPDLOG_WARN("[Main] Console signal received: type={}, halting hardware", (int)ctrlType);
    try {
        HardwareManager::instance().ShutdownHalt();
    } catch (...) {}
    // 返回 FALSE：继续执行系统默认处理（进程终止）
    return FALSE;
}

extern "C" USHORT __stdcall RtlCaptureStackBackTrace(ULONG, ULONG, PVOID*, PULONG);

// 崩溃转储路径与符号搜索路径：由 main() 在日志目录判定完成后以**窄字符**预填。
// 为何不在这里调 Qt：CrashHandler 运行在异常上下文（可能已发生堆损坏），
// QString/QDir 的分配与文件系统调用都有二次崩溃风险，故此处只读静态缓冲。
static char g_crashLogPath[1024] = {0};
static char g_symSearchPath[1024] = {0};

// 临时崩溃诊断：记录未处理异常地址与调用栈（带符号解析）
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep)
{
    try {
        // 未预填（main() 完成前就崩溃）时退回当前工作目录的相对路径，不再使用硬编码绝对路径。
        const char* crashPath = (g_crashLogPath[0] != '\0') ? g_crashLogPath : "crash.txt";
        std::ofstream of(crashPath, std::ios::app);
        void* frames[48] = {};
        USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        // 符号搜索路径 = exe 所在目录（Debug 版 pdb 与 exe 同级；Release 无 pdb，退化为仅模块名）。
        const char* symPath = (g_symSearchPath[0] != '\0') ? g_symSearchPath : ".";
        if (!SymInitialize(proc, symPath, TRUE))
            of << "(SymInitialize failed " << GetLastError() << ")\n";

        auto dumpAddr = [&](const char* tag, void* p) {
            DWORD64 base = SymGetModuleBase64(proc, (DWORD64)p);
            if (!base) {
                of << "  " << tag << " " << p << " (unknown module)\n";
                return;
            }
            char modname[MAX_PATH] = {};
            GetModuleFileNameA((HMODULE)base, modname, MAX_PATH);
            SYMBOL_INFO* s = (SYMBOL_INFO*)_alloca(sizeof(SYMBOL_INFO) + 1024);
            memset(s, 0, sizeof(SYMBOL_INFO) + 1024);
            s->SizeOfStruct = sizeof(SYMBOL_INFO);
            s->MaxNameLen = 1024;
            DWORD64 disp = 0;
            of << "  " << tag << " " << p << " [" << modname << "]";
            if (SymFromAddr(proc, (DWORD64)p, &disp, s))
                of << "  " << s->Name << "+0x" << std::hex << disp << std::dec;
            else
                of << "  +0x" << std::hex << (reinterpret_cast<DWORD64>(p) - base) << std::dec;
            IMAGEHLP_LINE64 ln = {};
            ln.SizeOfStruct = sizeof(ln);
            DWORD ldisp = 0;
            if (SymGetLineFromAddr64(proc, (DWORD64)p, &ldisp, &ln))
                of << "  (" << ln.FileName << ":" << ln.LineNumber << ")";
            of << "\n";
        };

        of << "== crash addr=" << ep->ExceptionRecord->ExceptionAddress
           << " code=0x" << std::hex << ep->ExceptionRecord->ExceptionCode << std::dec
           << " flt=" << ep->ExceptionRecord->ExceptionFlags
           << " nparams=" << ep->ExceptionRecord->NumberParameters
           << " info0=0x" << std::hex
           << (ep->ExceptionRecord->NumberParameters > 0 ? ep->ExceptionRecord->ExceptionInformation[0] : 0)
           << std::dec << "\n";
        dumpAddr("fault", ep->ExceptionRecord->ExceptionAddress);
        for (USHORT i = 0; i < n; ++i) {
            std::string tag = "[" + std::to_string(i) + "]";
            dumpAddr(tag.c_str(), frames[i]);
        }
        of.flush();
        SymCleanup(proc);
    } catch (...) {}
    return EXCEPTION_EXECUTE_HANDLER;
}

// 自定义日志标志 %P：输出源码位置时去掉工程根目录前缀
// 例: [src\HAL\HardwareManager.cpp:158]
class RootStripFlag final : public spdlog::custom_flag_formatter
{
public:
    explicit RootStripFlag(const QString& root)
        : rootNorm_(QDir::toNativeSeparators(QDir::cleanPath(root)))
    {
        for (auto& ch : rootNorm_)
            ch = (ch == QLatin1Char('\\')) ? QLatin1Char('/') : ch;
        rootNorm_ = rootNorm_.toLower();
        rootLen_ = static_cast<size_t>(rootNorm_.size());
    }

    void format(const spdlog::details::log_msg& msg, const std::tm&,
                spdlog::memory_buf_t& dest) override
    {
        std::string loc = msg.source.filename ? msg.source.filename : "unknown";
        if (rootLen_ > 0 && loc.size() >= rootLen_)
        {
            std::string norm = loc;
            std::replace(norm.begin(), norm.end(), '\\', '/');
            for (auto& ch : norm)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (norm.compare(0, rootLen_, rootNorm_.toStdString()) == 0)
            {
                loc.erase(0, rootLen_);
                if (!loc.empty() && (loc.front() == '\\' || loc.front() == '/'))
                    loc.erase(0, 1);
            }
        }
        spdlog::fmt_lib::format_to(std::back_inserter(dest), "{}:{}", loc, msg.source.line);
    }

    std::unique_ptr<spdlog::custom_flag_formatter> clone() const override
    {
        return std::make_unique<RootStripFlag>(*this);
    }

private:
    QString rootNorm_;
    size_t rootLen_ = 0;
};

// 日志保留策略（D16：2026-09-14 定稿「7 天」→ 2026-09-29 修订为「保留最近 N 个文件」）。
// 为何按个数而非按天数：日志已按日期分文件（daily_file_sink 的 %Y-%m-%d 命名），若以
// 「今天 − N 天」为 cutoff 删除，长假期间不开机则现存文件的日期**全部早于 cutoff**
// ⇒ 复工第一次启动会把历史日志一次性清空，旧记录一个不剩。改为**保留日期最新的
// keepCount 个文件**，与开机频次解耦：休假再久也只丢最老的，最近 N 次开机的记录始终在。
// 另：spdlog daily_file_sink 的 max_files 实测未生效（曾积到 35 个日期文件，最早
// 2026-07-27），故必须自己清。
// 安全边界（窄到只可能命中本程序自己的日志）：
//   · 只在程序选定的 log 目录内；
//   · 只匹配 ^creampuff_YYYY-MM-DD\.log$（creampuff.log / crash.txt / 其它文件一律不碰）；
//   · 文件名日期非法的一律保留（不参与排序、不占配额）；总数不足 keepCount 时一个不删；
//   · 删除数量写日志，便于事后核对。
static void PurgeOldLogs(const QString& logDir, int keepCount)
{
    QDir dir(logDir);
    if (!dir.exists()) return;

    static const QRegularExpression re(QStringLiteral("^creampuff_(\\d{4})-(\\d{2})-(\\d{2})\\.log$"));

    const auto entries = dir.entryList({ QStringLiteral("creampuff_*.log") }, QDir::Files);

    QList<QPair<QDate, QString>> dated;   // (文件日期, 文件名)
    dated.reserve(entries.size());
    for (const auto& name : entries) {
        const auto m = re.match(name);
        if (!m.hasMatch()) continue;
        const QDate d(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
        if (!d.isValid()) continue;
        dated.append({ d, name });
    }

    // 日期降序（最新在前）。同日不可能出现两个日期文件（daily sink 只按日期命名），
    // 故无需再以文件名做次级比较。
    std::sort(dated.begin(), dated.end(),
              [](const QPair<QDate, QString>& a, const QPair<QDate, QString>& b) {
                  return a.first > b.first;
              });

    const int keep = (keepCount > 0) ? keepCount : 1;
    int removed = 0;
    for (int i = keep; i < dated.size(); ++i) {
        if (QFile::remove(dir.filePath(dated.at(i).second))) ++removed;
    }
    if (removed > 0) {
        SPDLOG_INFO("[Main] Log retention: removed {} file(s), kept newest {} of {} day-file(s)",
                    removed, keep, dated.size());
    }
}


int main(int argc, char* argv[])
{
    SetUnhandledExceptionFilter(CrashHandler);

    // 注册控制台信号处理器（关闭 cmd 窗口 / Ctrl+C 等强杀路径也断使能）
    SetConsoleCtrlHandler(ConsoleSignalHandler, TRUE);

    QApplication app(argc, argv);
    app.setApplicationName("CreamPuffRobot");
    app.setApplicationVersion("1.0.0");
    app.setStyle(QStyleFactory::create("Fusion"));

    // spdlog — daily file logger（判定规则见 ConfigPaths::logDir）
    QString logReason;
    const QString logDir = ConfigPaths::logDir(&logReason);

    // 预填崩溃转储与符号路径（窄字符，供 CrashHandler 在异常上下文安全使用）
    {
        const QByteArray crashPath =
            QDir::toNativeSeparators(logDir + QStringLiteral("/crash.txt")).toLocal8Bit();
        const QByteArray exeDir =
            QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).toLocal8Bit();
        std::snprintf(g_crashLogPath, sizeof(g_crashLogPath), "%s", crashPath.constData());
        std::snprintf(g_symSearchPath, sizeof(g_symSearchPath), "%s", exeDir.constData());
    }

    const auto logPath = logDir + QStringLiteral("/creampuff.log");
    try
    {
        constexpr int rotationHour = 0;
        constexpr int rotationMinute = 0;

        auto dailySink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
            logPath.toStdString(), rotationHour, rotationMinute, false, 30);

        auto formatter = std::make_unique<spdlog::pattern_formatter>();
        formatter->add_flag<RootStripFlag>('P', QStringLiteral(PROJECT_SOURCE_DIR))
                 .set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%P] %v");
        dailySink->set_formatter(std::move(formatter));

        auto logger = std::make_shared<spdlog::logger>("main", dailySink);
        logger->set_level(spdlog::level::debug);
        spdlog::set_default_logger(logger);
        spdlog::flush_on(spdlog::level::debug);
        spdlog::flush_every(std::chrono::seconds(1));

        SPDLOG_INFO("[Main] Log initialized: {} (rule: {})",
                    logPath.toStdString(), logReason.toStdString());
    }
    catch (...)
    {
        // 日志目录不可用时禁用文件日志，避免启动崩溃
    }

    // 日志保留最近 7 个日期文件（D16 修订，2026-09-29）：按**个数**保留，不按天数——
    // 否则长假后首次开机会把假期前的历史日志全部清掉（详见 PurgeOldLogs 注释）
    PurgeOldLogs(logDir, 7);

    MainWindow mainWindow;
    mainWindow.show();

    // 退出前断使能：舵机发 Damping 松力（避免软件关闭后舵机仍带电锁定）。
    // 必须在 HardwareManager 单例析构前执行（此时串口仍打开，能发帧）。
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        SPDLOG_INFO("[Main] App quitting, disabling all axes");
        HardwareManager::instance().DisableAll();
    });

    return app.exec();
}
