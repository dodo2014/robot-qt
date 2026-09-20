#include "ConfigPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

#include "ConfigManager.h"

#include <spdlog/spdlog.h>

namespace {

/// 候选 1：源码树。`PROJECT_SOURCE_DIR` 由各 target 的 CMakeLists 注入
/// （Config / HAL / UI / tests 均有定义），未定义时返回空串表示该候选不可用。
QString sourceTreeCandidate(const QString& fileName)
{
#ifdef PROJECT_SOURCE_DIR
    return QString::fromUtf8(PROJECT_SOURCE_DIR) + QStringLiteral("/config/") + fileName;
#else
    Q_UNUSED(fileName);
    return QString();
#endif
}

} // namespace

namespace ConfigPaths {

QString sourceDir()
{
#ifdef PROJECT_SOURCE_DIR
    return QString::fromUtf8(PROJECT_SOURCE_DIR);
#else
    return QString();
#endif
}

QString resolve(const QString& fileName)
{
    const QString appDir = QCoreApplication::applicationDirPath();

    QStringList candidates;
    const QString src = sourceTreeCandidate(fileName);
    if (!src.isEmpty())
        candidates << src;
    candidates << appDir + QLatin1Char('/') + fileName
               << appDir + QStringLiteral("/../config/") + fileName;

    for (const auto& path : candidates) {
        if (QFileInfo::exists(path))
            return path;
    }
    // 全部候选都不存在：返回首选候选，由调用方按「文件不存在」处理
    return candidates.first();
}

QString configJsonPath()
{
    return resolve(QStringLiteral("config.json"));
}

QString processJsonPath()
{
    return resolve(QStringLiteral("process.json"));
}

bool ensureConfigLoaded()
{
    auto& cfg = ConfigManager::instance();
    if (!cfg.filePath().isEmpty() && QFileInfo::exists(cfg.filePath()))
        return true; // 已加载

    const QString path = configJsonPath();
    if (!QFileInfo::exists(path)) {
        SPDLOG_INFO("[ConfigPaths] No config.json found in any candidate path");
        return false;
    }
    return cfg.load(path);
}


// 日志目录判定（2026-09-16 改为便携优先，取代原「工程源码 log 目录一律优先」）。
//
// **P4 从 `main.cpp` 迁到这里**：原为 `main.cpp` 的 `static` 函数，只有
// `main()` 能用；视觉页的「打开日志目录」按钮需要同一个答案，若在页面里
// 重写一遍规则就会立刻出现第二套实现（正是 C5 的教训）。现为**全项目唯一实现**。
//
// 规则：
//   0) 环境变量 CREAMPUFF_LOG_DIR 非空且可创建 → 直接用（现场排障的逃生门，优先级最高）
//   1) exe 所在目录位于编译期源码树 PROJECT_SOURCE_DIR 之下 → 工程根 log/
//      —— 开发机行为，与改动前完全一致。**不可去掉这一支**：sim_smoke.ps1 与
//      doc/test/*.md、doc/compile_guide.md 均以工程根 log/ 为判定依据。
//   2) 否则 → exe 旁 log/（便携部署：整个输出目录拷到任意机器即可，不受盘符影响）
//   3) 都不可写 → %APPDATA%/CreamPuffRobot/log
// 关键：规则 1 比较的是 **exe 的实际位置** 与烧进 exe 的源码路径字符串，而不是
// 「D: 盘存不存在」——所以目标机有无 D: 盘都会正确落到 exe 旁。
// reasonOut 回传命中的规则，便于现场按日志定位。
QString logDir(QString* reasonOut)
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

} // namespace ConfigPaths
