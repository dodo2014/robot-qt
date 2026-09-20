#pragma once

#include <QString>

/// 配置文件路径解析与加载 —— **全项目唯一实现**
///
/// 历史背景：本逻辑原先在三处各写一份，候选路径与优先级互不相同
/// （`HardwareManager::EnsureConfigLoaded`、`ConfigPage::ensureConfigLoaded`、
/// `ProcessManager::processJsonPath`），见 `doc/config_ownership_plan.md` 的 C5。
/// 分文件存储（该文档 §六）的前置条件正是「路径解析必须先收敛为一个函数」，
/// 否则分文件后会立刻出现第四套。
namespace ConfigPaths {

/// 编译期注入的工程源码根目录（`PROJECT_SOURCE_DIR`）；未定义时返回空串。
QString sourceDir();

/// 按统一候选顺序解析 `<配置目录>/<fileName>`，返回第一个存在的路径。
///
/// 候选顺序（先命中先用）：
///   1. `<PROJECT_SOURCE_DIR>/config/<fileName>`   开发机源码树优先
///   2. `<applicationDirPath>/<fileName>`          部署包根目录
///   3. `<applicationDirPath>/../config/<fileName>` 部署包上级
///
/// 三者都不存在时返回候选 1 —— 与旧 `processJsonPath()` 行为一致，
/// 调用方须自行按「文件不存在」处理（不要假定返回值一定存在）。
QString resolve(const QString& fileName);

/// `config/config.json` 的路径
QString configJsonPath();

/// `config/process.json` 的路径
QString processJsonPath();

/// 日志目录（**全项目唯一实现**，P4 从 `main.cpp` 迁入）。
///
/// 规则：① `CREAMPUFF_LOG_DIR` 环境变量覆盖；② exe 位于编译期源码树之下 → 工程根
/// `log/`（开发机，`sim_smoke.ps1` 与 `doc/test/*.md` 以此为准）；③ 否则 exe 旁
/// `log/`（便携部署）；④ 都不可写 → `%APPDATA%/CreamPuffRobot/log`。
///
/// `reasonOut` 回传命中的规则，便于现场按日志定位。
QString logDir(QString* reasonOut = nullptr);

/// 幂等加载 `config.json` 到 `ConfigManager`。
///
/// 已加载且文件仍存在时直接返回 `true`，不重复读盘 —— 避免覆盖内存中的未保存修改。
/// 返回是否最终处于「已加载」状态。
bool ensureConfigLoaded();

} // namespace ConfigPaths
