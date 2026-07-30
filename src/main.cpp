#include <QApplication>
#include <QDir>
#include <QStyleFactory>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/daily_file_sink.h>

#include "UI/MainWindow.h"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("CreamPuffRobot");
    app.setApplicationVersion("1.0.0");
    app.setStyle(QStyleFactory::create("Fusion"));

    // spdlog 鈥?daily file logger
    {
        const auto logDir = QString::fromUtf8(PROJECT_SOURCE_DIR) + QStringLiteral("/log");
        QDir().mkpath(logDir);

        const auto logPath = logDir + QStringLiteral("/creampuff.log");
        constexpr int rotationHour = 0;
        constexpr int rotationMinute = 0;

        auto dailySink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
            logPath.toStdString(), rotationHour, rotationMinute, false, 30);
        dailySink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%@] %v");

        auto logger = std::make_shared<spdlog::logger>("main", dailySink);
        logger->set_level(spdlog::level::debug);
        spdlog::set_default_logger(logger);
        spdlog::flush_on(spdlog::level::debug);
        spdlog::flush_every(std::chrono::seconds(1));

        SPDLOG_INFO("[Main] Log initialized: {}", logPath.toStdString());
    }

    MainWindow mainWindow;
    mainWindow.show();

    return app.exec();
}
