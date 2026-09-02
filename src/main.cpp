#include <QApplication>
#include <QDir>
#include <QStandardPaths>
#include <QStyleFactory>

#include <algorithm>
#include <cctype>
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

// 临时崩溃诊断：记录未处理异常地址与调用栈（带符号解析）
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep)
{
    try {
        std::ofstream of("D:/workspace/projects/CreamPuffRobot/log/crash.txt", std::ios::app);
        void* frames[48] = {};
        USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        HANDLE proc = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        if (!SymInitialize(proc, "D:/workspace/projects/CreamPuffRobot/out/build/x64-Debug", TRUE))
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

int main(int argc, char* argv[])
{
    SetUnhandledExceptionFilter(CrashHandler);

    // 注册控制台信号处理器（关闭 cmd 窗口 / Ctrl+C 等强杀路径也断使能）
    SetConsoleCtrlHandler(ConsoleSignalHandler, TRUE);

    QApplication app(argc, argv);
    app.setApplicationName("CreamPuffRobot");
    app.setApplicationVersion("1.0.0");
    app.setStyle(QStyleFactory::create("Fusion"));

    // spdlog — daily file logger
    // 日志目录优先级：工程源码 log 目录(开发机) > exe 旁 log(便携部署) > %APPDATA%
    QString logDir;
    const QStringList candidates = {
        QStringLiteral(PROJECT_SOURCE_DIR "/log"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/log"),
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/log"),
    };
    for (const auto& c : candidates)
    {
        if (QDir().mkpath(c))
        {
            logDir = c;
            break;
        }
    }
    if (logDir.isEmpty())
        logDir = candidates.front();

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

        SPDLOG_INFO("[Main] Log initialized: {}", logPath.toStdString());
    }
    catch (...)
    {
        // 日志目录不可用时禁用文件日志，避免启动崩溃
    }

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
