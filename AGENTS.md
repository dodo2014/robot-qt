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
- **Release**: root `build_release.bat` (relocatable via `%~dp0`, ASCII-only; builds Release + one-click packaging)

## Architecture

```
CMakeLists.txt  — root: find_package(Qt6/Eigen3/OpenCV/spdlog) + 5 subdirs
├─ src/Config/  — Configuration management (ConfigManager, ProcessManager)
├─ src/HAL/     — Hardware Abstraction Layer (interfaces + SimCard/SimServo + HardwareManager)
├─ src/Core/    — Kinematics, CoordTransform, Trajectory
├─ src/Logic/   — PickCycleController (state machine)
└─ src/UI/      — MainWindow + 4 pages + ToggleSwitch
```

Layering (link direction): `UI → Logic → Core → HAL`; `HAL → Config` (HardwareManager 读配置喂给底层卡)

### HAL 多品牌体系

- **接口层**（纯虚，`src/HAL/`）: `IMotionCard`(脉冲单位)、`IAxisServo`、`IEndEffector`、`ICamera`、`IPuffAlgorithm`
- **工厂**: `HALFactory.h` 运行时字符串注册工厂，`REGISTER_MOTION_CARD/AXIS_SERVO/END_EFFECTOR/CAMERA/PUFF_ALGORITHM` 宏自动注册
- **仿真实现**: `SimCard`("SimCard")、`SimServo`("SimServo")
- **品牌实现**: `BoPaiCard`("Bopai"，博派运动卡，`USE_BOPAI`)、`XRServo`("XRServo"，XR 串口舵机，`USE_XRSERVO`)。舵机 ID 由 HardwareManager 从 `config.communication.servos[]` 读取喂入
- **`HardwareManager`**（单例 QObject）: 读 `config.simulation.*Type` + `communication.*` 经工厂创建硬件；调用面为**物理单位(mm/度)**，内部经 `AxisConverter` 换算为脉冲再调底层卡；50ms QTimer 轮询状态经信号 `stateUpdated/servoStateUpdated/axisAlarm/limitTriggered` 广播
- **`AxisConverter`**（单例）: 物理↔脉冲双向换算；参数由 HardwareManager 从 `config.axes.<key>.transmission` 读取后 `ConfigureAxis()` 喂入，**底层卡代码禁止 `#include "ConfigManager.h"`**
- **`AxisMap.h`**: 逻辑轴枚举 `LogicalAxis{J1,J2,Z,R,Gripper,Extruder}` ↔ 硬件绑定(卡轴/舵机) 映射
- **IMotionCard 契约**: `MoveAbs/MoveRel/GetPosition` 均以**脉冲**为单位；`SetAxisConfig(axisId, cfg)` 下发每轴换算参数
- **品牌扩展**: 新增品牌实现文件放 `src/HAL/`，SDK 放根目录 `3rdparty/<brand>/`（`include/lib/bin` 三目录）。顶层 CMake POST_BUILD 自动复制 `3rdparty/bopai/bin/*.dll`。`ZMotion/Leisai` 目录已预留
- **新实现链接陷阱**: 只靠 `REGISTER_*` 宏的静态注册对象可能被链接器 dead-strip 丢弃 .obj，导致工厂找不到类型（曾导致 SimServo 无法创建）。`HardwareManager.cpp` 的 `ForceLinkHALImpls()` 通过取成员函数地址强制保留，并在其中**显式 `Factory::Register(...)` 兜底**（覆盖注册无副作用）。**新增运动卡/舵机实现必须同步加入该函数**

## Code Conventions

- **C++17**, `UTF-8` / `W4`, 64-bit only
- **No comments** in code unless essential
- **QSS**: per-widget stylesheet with full `QPushButton { ... }` selector, never bare properties. `QSizePolicy::Ignored` for stretch participation
- **Signals**: connected to lambdas that call `qDebug()` stubs until state machine is wired
- **Naming**: PascalCase classes/methods, camelCase locals, `m_` for members, `QStringLiteral` for all UI strings
- **spdlog**: `daily_file_sink_mt` at `PROJECT_SOURCE_DIR/log/creampuff.log`, 30-day retention, pattern `[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%P] %v`. `%P` is a custom flag (`RootStripFlag` in `src/main.cpp`) that strips the project-root prefix from `__FILE__`, e.g. `[src\HAL\HardwareManager.cpp:158]`; paths outside the project root stay absolute.

## Config

- `CMakeSettings.json` — VS CMake config (Ninja + Qt6_DIR)
- `vcpkg.json` — dependencies: eigen3, nlohmann-json, opencv4, spdlog
- `config/config.json` — runtime config (copied to output dir at build). Full schema documented in `doc/config.md`.
  - `axes` is a JSON **object** (not array). Key = immutable logical identity name (e.g. `Axis_J1`, `Axis_J2`, `Axis_Z`, `Axis_R`, `Axis_Gripper`, `Axis_Extruder`) — never coupled to physical port. `hardwareType`/`portId` are mutable attributes inside each entry. Each entry has a `sortOrder` field for UI display ordering.
  - `communication.motionCard.port` **存为字符串**（如 `"60000"`）：ConfigPage 通过 `bindLineEdit` 将其绑定为 QLineEdit 文本。`HardwareManager` 读取时必须 `getValue<std::string>` 再 `std::stoi` 转换，**不要改成 JSON 数字类型**——两边读取类型不一致会触发 ConfigManager `getValue error: type must be ...`（曾踩坑）。

### ConfigManager (src/Config/ConfigManager.h/.cpp)

- Singleton via `ConfigManager::instance()`
- Loads from `config.json` at startup (search order: `PROJECT_SOURCE_DIR/config/config.json` → app dir)
- Path resolution supports dot-notation with array index or object key: `"axes.Axis_J1.maxSpeed"` or `"axes[0].maxSpeed"`
- Values saved on every change with 300ms debounce timer
- Template `getValue<T>(path, default)` for reads; `set(path, value)` for writes; `markDirty()` to trigger deferred save after direct root() manipulation
- `SIMULATION_MODE` compile option (ON by default)

### ProcessManager (src/Config/ProcessManager.h/.cpp)

- Singleton via `ProcessManager::instance()`
- Manages process/program data: `QVector<SchemeData>` (schemes → actions → points)
- Model structs: `ActionType` enum (Move/Vision/Extrude/Delay/Gripper), `PointData`, `ActionData`, `SchemeData`
- `load()` — reads from `PROJECT_SOURCE_DIR/config/process.json`; auto-generates default test scheme if file missing. Points are read from the nested `coord` object (`coord.x/y/z/r`); the old flat `x/y/z/r` layout is **not** supported
- `save()` — serializes `m_schemes` to `process.json` with 4-space indent. Point serialization: `name`/`posture` at point level, values under `coord{x,y,z,r}`, placeholder `joints{j1..j4:0}` (joints values are stubbed to 0 for now)
- `actionTypeName()` — returns display name ("移动"/"识别"/"挤压"/"延时"/"夹爪")
- `generateUniqueSchemeName()` — generates `方案_XXX` with random suffix, dedup against existing schemes

## UI Pages

1. **AutoRunPage** — 自动运行: camera views, LCD display, coord panel, log, 5 control buttons
2. **ManualControlPage** — 手动控制: enable/disable, 6-axis JOG table
3. **ProcessPage** — 工艺与流程: scheme management, action list (QListWidget), detail stack (QStackedWidget × 5 action types). Data stored in `process.json` via `ProcessManager`. 动作与方案的新增/编辑/删除/保存完整闭环，实时同步 JSON。
   - 移动动作: 点位 QTableWidget (点名称/X/Y/Z/R/姿态) + 添加/删除/上移/下移/示教按钮。内存仍为扁平 PointData，仅 JSON 序列化为 `coord`/`joints` 嵌套结构
   - 识别动作: 识别类型/曝光时间/匹配模板/置信度阈值
   - 挤压动作: 挤出量/挤出速度/回抽量/回抽速度
   - 延时动作: 延时时间 ms
   - 夹爪动作: 闭合/张开 选择
4. **ConfigPage** — 设备与配置: 5 tabs (通信与连接/运动学参数/视觉与工艺参数/TCP与标定/电控与映射)
   - **电控与映射** tab: 左侧轴列表按 `sortOrder` 升序排列。每个轴存储在 `axes` 对象中，key 为**不可变的逻辑身份名**（如 `Axis_J1`），与物理端口解耦。修改 `hardwareType` 或 `portId` 时仅更新对应属性字段（不重命名 key），并自动校验同类型端口不重复（重复则弹窗回退）。

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
- **QSS 陷阱（Qt6 崩溃）**: **禁止在 `QDoubleSpinBox`/`QSpinBox`/`QLineEdit` 的 per-widget 样式表里写 `font-size`**。`ManualControlPage` 的转速/目标 spinbox 曾因此触发 Qt 样式引擎在页面 reparent 到 `QStackedWidget`（`MainWindow::SetupUI` 的 `addWidget`）polish 阶段崩溃（0xc0000005，读写访问违例，地址随机）。若要改字号，用 `setFont()`（`setPointSizeF(9.75)` ≈ 13px），样式表只保留颜色/边框/内边距。

## Current Phase

HAL 多品牌硬件接入全部完成：
- **Phase 1+2+5**：`HardwareManager` 组装层、`AxisConverter` 单位换算、`AxisMap` 逻辑轴映射、`SimServo`、`IMotionCard` 补齐 MoveJog/StopJog/SetAxisConfig、`ManualControlPage` 接线。
- **Phase 3**：`BoPaiCard`（博派运动卡，`REGISTER_MOTION_CARD("Bopai")`，SDK 在 `3rdparty/bopai/`）、`XRServo`（XR 串口舵机，`REGISTER_AXIS_SERVO("XRServo")`，舵机 ID 从 `communication.servos[]` 读取）。`USE_BOPAI`/`USE_XRSERVO` 编译开关。
- **仿真验收（Sim 模式）**：手动控制 P1–P5 已通过 GUI 自动化全量验证通过——P1 点动积分（MoveJog/StopJog 位置累计）、P2 每轴状态点（已使能绿/运行中蓝/告警红）与一键回零提示、P3 急停（SimCard 全轴 halt + 舵机 Torque OFF）、P4 舵机点动/停止（Stop 保持力矩、无 TorqueOff）、P5 未连接硬件提示（无效类型不动作）。期间修复：spinbox QSS `font-size` 触发 Qt6 polish 崩溃、SimServo 死链接注册、config 端口类型不一致。

**下一步（后续）**：ZMotion/Leisai 品牌接入（SDK 目录已预留）；`PickCycleController::SetHardware` 注入与自动运行硬件接线；状态机实现。

其余未接线部分（`qDebug()`/`SPDLOG` 桩）：AutoRunPage 的 启动/复位/停止/初始化/急停；ProcessPage 的"示教读取"；ConfigPage 的"九点标定"。
