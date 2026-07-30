# CreamPuffRobot — AGENTS.md

## Project Overview

SCARA 泡芙抓取机器人控制系统。Qt6 深色主题 HMI + 仿真/真机双模式。

## Build

- **VS**: Open folder in VS, `Ctrl+Shift+B` (x64-Debug)
- **CLI**: Open "Developer PowerShell for VS 2026", then:
  ```
  cmake -S . -B out\build\x64-Debug -G Ninja ^
    -DCMAKE_TOOLCHAIN_FILE=vcpkg\scripts\buildsystems\vcpkg.cmake ^
    -DQt6_DIR=D:/Qt/6.11.1/msvc2022_64/lib/cmake/Qt6
  cmake --build out\build\x64-Debug
  ```
- **Run**: `out\build\x64-Debug\CreamPuffRobot.exe`

## Architecture

```
CMakeLists.txt  — root: find_package(Qt6/Eigen3/OpenCV/spdlog) + 5 subdirs
├─ src/Config/  — Configuration management (ConfigManager, ProcessManager)
├─ src/HAL/     — Hardware Abstraction Layer (interfaces + SimCard)
├─ src/Core/    — Kinematics, CoordTransform, Trajectory
├─ src/Logic/   — PickCycleController (state machine)
└─ src/UI/      — MainWindow + 4 pages + ToggleSwitch
```

Layering (link direction): `UI → Logic → Core → HAL`

## Code Conventions

- **C++17**, `UTF-8` / `W4`, 64-bit only
- **No comments** in code unless essential
- **QSS**: per-widget stylesheet with full `QPushButton { ... }` selector, never bare properties. `QSizePolicy::Ignored` for stretch participation
- **Signals**: connected to lambdas that call `qDebug()` stubs until state machine is wired
- **Naming**: PascalCase classes/methods, camelCase locals, `m_` for members, `QStringLiteral` for all UI strings
- **spdlog**: `daily_file_sink_mt` at `PROJECT_SOURCE_DIR/log/creampuff.log`, 30-day retention, pattern `[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v`

## Config

- `CMakeSettings.json` — VS CMake config (Ninja + Qt6_DIR)
- `vcpkg.json` — dependencies: eigen3, nlohmann-json, opencv4, spdlog
- `config/config.json` — runtime config (copied to output dir at build). Full schema documented in `doc/config.md`.
  - `axes` is a JSON **object** (not array). Key = `motor_{portId}` (hardwareType=0) or `servo_{portId}` (hardwareType=1). Each entry has a `sortOrder` field for UI display ordering.

### ConfigManager (src/Config/ConfigManager.h/.cpp)

- Singleton via `ConfigManager::instance()`
- Loads from `config.json` at startup (search order: `PROJECT_SOURCE_DIR/config/config.json` → app dir)
- Path resolution supports dot-notation with array index or object key: `"axes.motor_0.maxSpeed"` or `"axes[0].maxSpeed"`
- Values saved on every change with 300ms debounce timer
- Template `getValue<T>(path, default)` for reads; `set(path, value)` for writes; `markDirty()` to trigger deferred save after direct root() manipulation
- `SIMULATION_MODE` compile option (ON by default)

### ProcessManager (src/Config/ProcessManager.h/.cpp)

- Singleton via `ProcessManager::instance()`
- Manages process/program data: `QVector<SchemeData>` (schemes → actions → points)
- Model structs: `ActionType` enum (Move/Vision/Extrude/Delay/Gripper), `PointData`, `ActionData`, `SchemeData`
- `load()` — reads from `PROJECT_SOURCE_DIR/config/process.json`; auto-generates default test scheme if file missing
- `save()` — serializes `m_schemes` to `process.json` with 4-space indent
- `actionTypeName()` — returns display name ("移动"/"识别"/"挤压"/"延时"/"夹爪")
- `generateUniqueSchemeName()` — generates `方案_XXX` with random suffix, dedup against existing schemes

## UI Pages

1. **AutoRunPage** — 自动运行: camera views, LCD display, coord panel, log, 5 control buttons
2. **ManualControlPage** — 手动控制: enable/disable, 6-axis JOG table
3. **ProcessPage** — 工艺与流程: scheme management, action list (QListWidget), detail stack (QStackedWidget × 5 action types). Data stored in `process.json` via `ProcessManager`. 动作与方案的新增/编辑/删除/保存完整闭环，实时同步 JSON。
   - 移动动作: 点位 QTableWidget (点名称/X/Y/Z/R/姿态) + 添加/删除/上移/下移/示教按钮
   - 识别动作: 识别类型/曝光时间/匹配模板/置信度阈值
   - 挤压动作: 挤出量/挤出速度/回抽量/回抽速度
   - 延时动作: 延时时间 ms
   - 夹爪动作: 闭合/张开 选择
4. **ConfigPage** — 设备与配置: 5 tabs (通信与连接/运动学参数/视觉与工艺参数/TCP与标定/电控与映射)
   - **电控与映射** tab: 左侧轴列表按 `sortOrder` 升序排列。每个轴存储在 `axes` 对象中，key 为 `{motor|servo}_{portId}`。修改 `hardwareType` 或 `portId` 时自动校验同类型端口不重复（重复则弹窗回退），并自动重命名 JSON key。

## UI Conventions

- **FormLayout + GroupBox**: 禁止同时使用 `setSizeConstraint(QLayout::SetMinimumSize)` 和显式 `setMinimumHeight`，两者冲突会导致 QFormLayout 行高被压缩。用 `WrapLongRows` 替代 `DontWrapRows`。
- **Label/Field 对齐**: 所有 form row 内控件统一 `setFixedHeight(32)`，label 设 `QSizePolicy::Fixed` 垂直策略，确保 QFormLayout 中 label 与右侧控件垂直居中对齐。
- **触控屏滚动条**: QScrollBar 宽度 ≥ 28px，handle 纯白 `#ffffff`，track 深色 `#1a2430`，min-height ≥ 50px，圆角 6-8px。
- **配置自动保存**: 所有配置页面使用 `ConfigManager`。QLineEdit 用 `editingFinished` 触发保存（回车或失焦），QDoubleSpinBox/QSpinBox 用 `valueChanged` 实时保存，QComboBox 用 `currentIndexChanged` 或 `currentTextChanged`。触发后 300ms 去抖写回 `config.json`。
- **ConfigManager 防御性编程**:
  - `root_` 默认初始化为 `json::object()` 而非 null，避免后续 `operator[]` 抛出异常
  - `load()` 解析失败时重置为 `json::object()`
  - `get()` 路径导航时对每个 token 做 `is_object()` 类型检查后再 `contains()`/`operator[]`
  - `set()` 在 `path.empty()` 时直接 return 并 `qWarning`，防止意外覆盖整个 JSON 根节点
  - `getValue()` 模板函数首行检查 `path.empty()` 后 return default，且全程 try-catch
- **Lambda 捕获安全**: `CreateElecMapTab()` 中 `loadAxis`/`pathFor` 等 lambda 按值捕获控件指针和 `this`，禁止使用 `[&]` 捕获栈上局部变量引用，避免信号触发时访问已销毁的栈帧导致 use-after-free 崩溃。

## Current Phase

ProcessPage 已完成完整的数据建模、UI 双向绑定和 JSON 文件持久化。其余页面 (AutoRunPage, ManualControlPage, ConfigPage) 为纯 UI shell，按钮为 `qDebug()` 桩。状态机 + 硬件接线为下一阶段。
