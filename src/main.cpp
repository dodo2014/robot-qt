#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDate>
#include <QFile>
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

// 日志保留策略（D16，2026-09-14 定稿：保留 7 天）。
// 为何要自己清：日志已按日期分文件（daily_file_sink 的 %Y-%m-%d 命名），但 sink 的
// max_files 实测未生效——现存 35 个日期文件（最早 2026-07-27）。故显式清理。
// 安全边界（窄到只可能命中本程序自己的日志）：
//   · 只在程序选定的 log 目录内；
//   · 只匹配 ^creampuff_YYYY-MM-DD\.log$（creampuff.log / crash.txt / 其它文件一律不碰）；
//   · 只删**日期早于 cutoff（今天 - keepDays）**的；日期非法或 ≥ cutoff 一律保留；
//   · 删除数量写日志，便于事后核对。
static void PurgeOldLogs(const QString& logDir, int keepDays)
{
    QDir dir(logDir);
    if (!dir.exists()) return;

    const QDate today = QDate::currentDate();
    const QDate cutoff = today.addDays(-keepDays);
    static const QRegularExpression re(QStringLiteral("^creampuff_(\\d{4})-(\\d{2})-(\\d{2})\\.log$"));

    const auto entries = dir.entryList({ QStringLiteral("creampuff_*.log") }, QDir::Files);
    int removed = 0;
    for (const auto& name : entries) {
        const auto m = re.match(name);
        if (!m.hasMatch()) continue;
        const QDate d(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
        if (!d.isValid() || d >= cutoff) continue;
        if (QFile::remove(dir.filePath(name))) ++removed;
    }
    if (removed > 0) {
        SPDLOG_INFO("[Main] Log retention: removed {} file(s) older than {} days (cutoff {})",
                    removed, keepDays, cutoff.toString(QStringLiteral("yyyy-MM-dd")).toStdString());
    }
}

// 日志目录判定（2026-09-16 改为便携优先，取代原「工程源码 log 目录一律优先」）：
//   0) 环境变量 CREAMPUFF_LOG_DIR 非空且可创建 → 直接用（现场排障的逃生门，优先级最高）
//   1) exe 所在目录位于编译期源码树 PROJECT_SOURCE_DIR 之下 → 工程根 log/
//      —— 开发机行为，与改动前完全一致。**不可去掉这一支**：sim_smoke.ps1 与
//      doc/test/*.md、doc/compile_guide.md 均以工程根 log/ 为判定依据。
//   2) 否则 → exe 旁 log/（便携部署：整个输出目录拷到任意机器即可，不受盘符影响）
//   3) 都不可写 → %APPDATA%/CreamPuffRobot/log
// 关键：规则 1 比较的是 **exe 的实际位置** 与烧进 exe 的源码路径字符串，而不是
// 「D: 盘存不存在」——所以目标机有无 D: 盘都会正确落到 exe 旁。
// reasonOut 回传命中的规则，便于现场按日志定位。
static QString ResolveLogDir(QString* reasonOut = nullptr)
{
    auto setReason = [reasonOut](const char* r) {
        if (reasonOut) *reasonOut = QString::fromLatin1(r);
    };

    // 0) 环境变量覆盖
    const QString overrideDir = qEnvironmentVariable("CREAMPUFF_LOG_DIR").trimmed();
    if (!overrideDir.isEmpty())
    {
        if (QDir().mkpath(overrideDir))
        {
            setReason("env CREAMPUFF_LOG_DIR");
            return QDir::cleanPath(overrideDir);
        }
        SPDLOG_WARN("[Main] CREAMPUFF_LOG_DIR not creatable, ignored: {}",
                    overrideDir.toStdString());
    }

    // 1) exe 在源码树内 → 工程根 log（开发机）
    const QString exeDir = QDir::cleanPath(QCoreApplication::applicationDirPath());
    const QString srcDir = QDir::cleanPath(QString::fromUtf8(PROJECT_SOURCE_DIR));
    const QString srcLog = srcDir + QStringLiteral("/log");

    const QString exeNorm = QDir::fromNativeSeparators(exeDir).toLower();
    const QString srcNorm = QDir::fromNativeSeparators(srcDir).toLower();
    if (exeNorm == srcNorm || exeNorm.startsWith(srcNorm + QLatin1Char('/')))
    {
        if (QDir().mkpath(srcLog))
        {
            setReason("source tree");
            return QDir::cleanPath(srcLog);
        }
    }

    // 2) exe 旁 log（便携部署）
    const QString exeLog = exeDir + QStringLiteral("/log");
    if (QDir().mkpath(exeLog))
    {
        setReason("next to exe");
        return QDir::cleanPath(exeLog);
    }

    // 3) 用户数据目录
    const QString appDataLog =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/log");
    if (QDir().mkpath(appDataLog))
    {
        setReason("appdata");
        return QDir::cleanPath(appDataLog);
    }

    setReason("all candidates failed, using exe dir");
    return exeLog;
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

    // spdlog — daily file logger（判定规则见 ResolveLogDir）
    QString logReason;
    const QString logDir = ResolveLogDir(&logReason);

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

    // 日志保留 7 天（D16）：启动时清理过期日期文件（不删当天/7 天内的）
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
