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
├─ src/HAL/     — Hardware Abstraction Layer (interfaces + SimCard/SimServo/SimCamera/SimAlgo + HardwareManager)
├─ src/Core/    — Kinematics, CoordTransform, Trajectory
├─ src/Logic/   — PickCycleController (state machine)
└─ src/UI/      — MainWindow + 5 pages + ToggleSwitch
```

Layering (link direction): `UI → Logic → Core → HAL`; `HAL → Config` (HardwareManager 读配置喂给底层卡)

### HAL 多品牌体系

- **接口层**（纯虚，`src/HAL/`）: `IMotionCard`(脉冲单位)、`IAxisServo`、`IEndEffector`、`ICamera`、`IPuffAlgorithm`
- **工厂**: `HALFactory.h` 运行时字符串注册工厂，`REGISTER_MOTION_CARD/AXIS_SERVO/END_EFFECTOR/CAMERA/PUFF_ALGORITHM` 宏自动注册
- **仿真实现**: `SimCard`("SimCard")、`SimServo`("SimServo")、`SimCamera`("SimCamera")、`SimAlgo`("SimAlgo")。`SimCamera` 生成含移动目标（粉/蓝泡芙斑块 + 深度图 mm）的测试图案帧；`SimAlgo` 按 `SimVision.h` 的 `TargetSpec` 颜色匹配检测目标→`PuffResult`（像素框 + 相机内参换算的物理坐标，`z` 取深度并受 `vision.depthZMin/ZMax` 过滤，`confidence`≈覆盖率）。`SimVision.h` 是两者共用的目标规格（改图案颜色/半径必须同步，保证"生成→识别"闭环）
- **相机采集线程**: `CameraCaptureWorker`（QObject + QTimer，被移入 `QThread`）每帧调 `ICamera::CaptureFrame()` 并经 `frameReady(const CameraFrame&)` 信号广播（值类型，`qRegisterMetaType<CameraFrame>` 已注册，跨线程自动深拷贝）。`HardwareManager` 负责生命周期：`CameraOpen/Close`、`StartCameraStream(fps)/StopCameraStream`、`IsCameraStreaming`，并把 worker 的 `frameReady` 转发为自身信号供 UI 订阅
- **帧存储线程**: `FrameSaver`（QObject + QTimer，独立 QThread）异步写 PNG，`SaveImage(QImage, subdir)` 进队后由线程内 30ms 定时器落盘到 `appDir/saves/<subdir>/yyyyMMdd_HHmmss_zzz.png`，经 `imageSaved(path)/saveError(msg)` 回调。**三线程模型**：采集线程 emit 帧 → UI 线程渲染/叠加/参数下发 → 存储线程消费保存队列
- **帧渲染工具**: `FrameConverter`（HAL，静态函数）：`ColorToQImage`(RGB888)、`DepthToQImage`(深度 mm→伪彩，0 值/负值画深色)、`DrawOverlays`(绘制 PuffResult 绿框 + 物理坐标/置信度文本)。**HAL 已链接 `Qt6::Gui`**（新增）供 QImage/QPainter
- **品牌实现**: `BoPaiCard`("Bopai"，博派运动卡，`USE_BOPAI`)、`XRServo`("XRServo"，XR 串口舵机，`USE_XRSERVO`)。舵机 ID 由 HardwareManager 从 `config.communication.servos[]` 读取喂入
- **`HardwareManager`**（单例 QObject）: 读 `config.simulation.*Type` + `communication.*` 经工厂创建硬件；调用面为**物理单位(mm/度)**，内部经 `AxisConverter` 换算为脉冲再调底层卡；50ms QTimer 轮询状态经信号 `stateUpdated/servoStateUpdated/axisAlarm/limitTriggered` 广播
- **软限位（已在 HardwareManager 层强制执行）**: `axes.<key>.limitMin/limitMax` 由 `HardwareManager` **实时读 config**（`GetLimitMin/Max`、`IsWithinSoftLimits`，改动即生效）。`MoveAbs` 目标越界拒绝下发；`MoveJog` 启动方向已在边界拒绝；点动中越界自动停止（卡轴在 `PollTick` 停止，舵机在 `JogTick` 夹紧到边界）。越界通过 `softLimitTriggered` 信号通知 UI（手动页状态点显示"限位"橙色 + 底部提示）。`limitMin >= limitMax` 视为配置错误，不限制并 `SPDLOG_WARN`。真机 BoPai 卡点动停止有≤一个轮询周期的越界量，可后续用卡自带软限位寄存器精确化
  - **触发语义（曾踩坑）**: `PollTick` 只在轴**正在点动撞入边界**（`st.running == true`）时 `emit softLimitTriggered`。静止停在边界（如 Z/夹爪/挤出的初始最小位置 0，或 MoveAbs 恰好落在边界）**不触发**，否则启动即误报"到达软限位"。`MoveJog` 启动方向已在边界时由拒绝路径补发一次信号
  - **UI 联动（ManualControlPage）**: 用 `QVector<int> softLimitDir_`（1=撞最大/-1=撞最小/0=正常）跟踪每轴方向（曾用 bool 无法区分方向）。`RefreshSoftLimitHint()` 聚合所有 `dir != 0` 的轴，用 " / " 拼接为 `轴N 到达软限位（最大位置 X）` 逐条提示；点动离开或 Go 成功时清 0 并**重算提示**（全部清除后恢复默认 `提示：按住 +/- 按钮持续运动，松开停止`）。状态点橙色 `#ffb347`；默认提示浅蓝 `#8fd4ff`
- **`AxisConverter`**（单例）: 物理↔脉冲双向换算；参数由 HardwareManager 从 `config.axes.<key>.transmission` 读取后 `ConfigureAxis()` 喂入，**底层卡代码禁止 `#include "ConfigManager.h"`**
- **`AxisMap.h`**: 逻辑轴枚举 `LogicalAxis{J1,J2,Z,R,Gripper,Extruder}` ↔ 硬件绑定(卡轴/舵机) 映射
- **IMotionCard 契约**: `MoveAbs/MoveRel/GetPosition` 均以**脉冲**为单位；`SetAxisConfig(axisId, cfg)` 下发每轴换算参数。`SimCard` 内部位置即脉冲。**单位陷阱（曾引发回归）**: 仿真限位夹紧必须用脉冲域限位（`SimAxis::limitMinPulse/limitMaxPulse`，`SetAxisConfig` 按 `ppu = ppr*microSteps/(360 或导程)` 换算，缺省 ±1e30），**禁止拿 `cfg.limitMin/limitMax`（度/mm）直接夹紧脉冲位置**——J1 的 180° 会被当成 180 脉冲（≈0.00097°），导致每次轮询位置被归零、点动/Go 不动
- **品牌扩展**: 新增品牌实现文件放 `src/HAL/`，SDK 放根目录 `3rdparty/<brand>/`（`include/lib/bin` 三目录）。顶层 CMake POST_BUILD 自动复制 `3rdparty/bopai/bin/*.dll`。`ZMotion/Leisai` 目录已预留
- **新实现链接陷阱**: 只靠 `REGISTER_*` 宏的静态注册对象可能被链接器 dead-strip 丢弃 .obj，导致工厂找不到类型（曾导致 SimServo 无法创建）。`HardwareManager.cpp` 的 `ForceLinkHALImpls()` 通过取成员函数地址强制保留，并在其中**显式 `Factory::Register(...)` 兜底**（覆盖注册无副作用）。**新增相机/算法/运动卡/舵机实现必须同步加入该函数**（`SimCamera`/`SimAlgo` 已在其中显式注册）

## Code Conventions

- **C++17**, `UTF-8` / `W4`, 64-bit only
- **No comments** in code unless essential
- **QSS**: per-widget stylesheet with full `QPushButton { ... }` selector, never bare properties. `QSizePolicy::Ignored` for stretch participation
- **Signals**: connected to lambdas that call `qDebug()` stubs until state machine is wired
- **Naming**: PascalCase classes/methods, camelCase locals, `m_` for members, `QStringLiteral` for all UI strings
- **spdlog**: `daily_file_sink_mt` at `PROJECT_SOURCE_DIR/log/creampuff.log`, 30-day retention, pattern `[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%P] %v`. `%P` is a custom flag (`RootStripFlag` in `src/main.cpp`) that strips the project-root prefix from `__FILE__`, e.g. `[src\HAL\HardwareManager.cpp:158]`; paths outside the project root stay absolute.
  - **统一用 `SPDLOG_INFO/WARN/ERROR/CRITICAL` 宏，禁止 `spdlog::info(...)` 等函数式调用**：函数式调用不带 source location，`%P` 会兜底成 `[unknown:0]`（曾全库 67 处误用）。宏默认带上 `__FILE__:__LINE__`

### C++ & Qt Rules (Strict Constraints)
- **内存与性能至上**: 
  - 优先使用智能指针 (`std::unique_ptr`, `std::shared_ptr`) 管理资源（需受控于 Qt 对象树 QObject Tree 的除外）。
  - 避免无意义的深度继承，优先使用接口（纯虚类）。
  - 对硬件控制、内存读写和指针调用，默认具备防御性编程思维。
- **敬畏 Qt 线程边界**:
  - **严禁阻塞主线程**：任何等待电机到位、读写文件、网络请求等耗时动作，绝对不可在 UI 线程执行。
  - **安全的跨线程通信**：线程间的数据传递和 UI 刷新，必须且只能使用 Qt 的信号与槽（Signals and Slots）机制。禁止子线程直接修改 UI 控件指针（防止 `0xc0000005` 崩溃）。

## Config

- `CMakeSettings.json` — VS CMake config (Ninja + Qt6_DIR)
- `vcpkg.json` — dependencies: eigen3, nlohmann-json, opencv4, spdlog
- `config/config.json` — runtime config (copied to output dir at build). Full schema documented in `doc/config.md`.
  - `axes` is a JSON **object** (not array). Key = immutable logical identity name (e.g. `Axis_J1`, `Axis_J2`, `Axis_Z`, `Axis_R`, `Axis_Gripper`, `Axis_Extruder`) — never coupled to physical port. `hardwareType`/`portId` are mutable attributes inside each entry. Each entry has a `sortOrder` field for UI display ordering.
  - `communication.motionCard.port` **存为字符串**（如 `"60000"`）：ConfigPage 通过 `bindLineEdit` 将其绑定为 QLineEdit 文本。`HardwareManager` 读取时必须 `getValue<std::string>` 再 `std::stoi` 转换，**不要改成 JSON 数字类型**——两边读取类型不一致会触发 ConfigManager `getValue error: type must be ...`（曾踩坑）。
  - `simulation.cameraType`("SimCamera") / `simulation.algorithmType`("SimAlgo") / `simulation.cameraDeviceId`；`vision.*` 键：`confidenceThreshold`(0.85)、`depthZMin`/`depthZMax`、`exposure`、`frameWidth`/`frameHeight`/`frameFps`(640/480/30)。`SimAlgo::Detect` **实时读 `vision.depthZMin/ZMax`** 过滤 z（`LoadConfig(json)` 仅存储占位，行为以 ConfigManager 为准）。

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
   - **动作运行速度**（`m_speedPercentRow`，仅移动/挤压/夹爪动作显示，其余隐藏）: `QSpinBox`(1-100, 后缀 " %") + `QSlider`(横向) 双向同步，各自 `blockSignals` 防回环；保存时读 `m_speedPercentSpin->value()`。滑块 `setFixedWidth(240)` 固定长度、无拉伸（`addWidget` 不带 stretch），长度调整改 `ProcessPage.cpp:469`
4. **VisionTestPage** — 视觉检测（导航第 4 项，位于「设备与配置」前）: 相机控制（型号下拉/序列号/分辨率/FPS/打开关闭/开始停止采集 + 状态点）＋ 预览（RGB/深度切换、识别框叠加开关、FPS/时间戳/分辨率、保存截图）＋ 算法测试（型号下拉/单次检测/连续检测/结果表格，表格列 = 序号/置信度/X/Y/Z/偏航/U/V/宽/高）＋ 离线图片加载（`getOpenFileName`→`CameraFrame`→`Detect`）＋ 参数（置信度阈值/Zmin/Zmax/曝光，`QDoubleSpinBox::valueChanged` 实时写 `ConfigManager`）。
   - 采集来源统一走 `HardwareManager::frameReady`（采集线程）→ `OnFrameReady` 存 `latestFrame_` 渲染；检测结果存 `lastResults_`，RGB/深度/截图共用 `BuildDisplayImage()`（含叠加）。截图经 `FrameSaver` 异步写盘。
   - 型号下拉在构造函数从工厂 `AvailableTypes()` 填充（`Initialize()` 前的 `ForceLinkHALImpls` 未执行时可能为空，回退 "SimCamera"/"SimAlgo" 字符串，与工厂注册名一致）。
5. **ConfigPage** — 设备与配置: 5 tabs (通信与连接/运动学参数/视觉与工艺参数/TCP与标定/电控与映射)
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
- **QSlider QSS 线框陷阱**: 只写 `::groove/::handle/::sub-page` 子控件样式时，控件**基础框/聚焦虚框**仍会按默认样式绘制一圈"线框"。必须加基础规则 `QSlider { background: transparent; border: none; }` 和 `QSlider:focus { border: none; outline: none; }`。定宽用 `setFixedWidth()` + `addWidget`（不带 stretch），QSlider 默认水平 sizePolicy 为 Expanding，`addWidget(slider, 1)` 会被拉满整行。
- **DPI 缩放注意**: 当前部署屏为 **150% DPI**，物理像素 = 逻辑像素 × 1.5。`setFixedWidth(240)` 实际约 360 物理px；做 GUI 自动化坐标换算/像素测量时先确认缩放，别拿物理坐标直接当逻辑值。

## Current Phase

HAL 多品牌硬件接入全部完成：
- **Phase 1+2+5**：`HardwareManager` 组装层、`AxisConverter` 单位换算、`AxisMap` 逻辑轴映射、`SimServo`、`IMotionCard` 补齐 MoveJog/StopJog/SetAxisConfig、`ManualControlPage` 接线。
- **Phase 3**：`BoPaiCard`（博派运动卡，`REGISTER_MOTION_CARD("Bopai")`，SDK 在 `3rdparty/bopai/`）、`XRServo`（XR 串口舵机，`REGISTER_AXIS_SERVO("XRServo")`，舵机 ID 从 `communication.servos[]` 读取）。`USE_BOPAI`/`USE_XRSERVO` 编译开关。
- **仿真验收（Sim 模式）**：手动控制 P1–P5 已通过 GUI 自动化全量验证通过——P1 点动积分（MoveJog/StopJog 位置累计）、P2 每轴状态点（已使能绿/运行中蓝/告警红）与一键回零提示、P3 急停（SimCard 全轴 halt + 舵机 Torque OFF）、P4 舵机点动/停止（Stop 保持力矩、无 TorqueOff）、P5 未连接硬件提示（无效类型不动作）。期间修复：spinbox QSS `font-size` 触发 Qt6 polish 崩溃、SimServo 死链接注册、config 端口类型不一致。
- **软限位强制执行 + 手动页提示（已完成并验证）**：`MoveAbs`/`MoveJog` 边界拒绝、点动撞限自动停、`softLimitTriggered` 信号、状态点橙色 + 底部聚合提示（多轴用 " / " 拼接、点动离开即清除恢复默认、默认提示浅蓝醒目色）。修复：SimCard 脉冲/物理单位夹紧回归、启动时静止停在 min=0 的轴误报软限位、舵机轴二次撞限不重触发。
- **ProcessPage 动作运行速度滑条（已完成并验证）**：`QSpinBox` + `QSlider` 双向同步（1-100%），仅移动/挤压/夹爪动作显示；滑块无边框、固定长度 240 逻辑px。已通过 GUI 拖动验证同步（拖动滑条 → 输入框数值跟随）。
- **视觉检测页 + 相机子系统（已完成并验证）**：SimCamera/SimAlgo 桩 + `CameraCaptureWorker` 采集线程 + `FrameSaver` 存储线程 + `FrameConverter` 渲染工具 + VisionTestPage。GUI 已验证：导航第 4 项进入页面、开流后 FPS~20 动图（双目标移动）、单次/连续检测（发现 2 目标 + 叠加框 + 物理坐标）、RGB/深度切换（深度伪彩近蓝远红）、保存截图落盘 `saves/snapshots`、离线图片加载后静态显示并检出 2 目标、打开/关闭相机状态灯。

**下一步（后续）**：ZMotion/Leisai 品牌接入（SDK 目录已预留）；`PickCycleController::SetHardware` 注入与自动运行硬件接线；AutoRunPage 两个相机占位框接入 `frameReady` 实时画面；奥比中光（Orbbec）真实相机 SDK 实现 `ICamera`；状态机实现。

其余未接线部分（`qDebug()`/`SPDLOG` 桩）：AutoRunPage 的 启动/复位/停止/初始化/急停；ProcessPage 的"示教读取"；ConfigPage 的"九点标定"。
