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
- **用户实际运行 Debug 版**（真机验证用 `out\build\x64-Debug\CreamPuffRobot.exe`）。改动代码后**务必同时编译 Debug + Release**，否则用户拿到的 exe 不含修复。编译前若 `LNK1168 无法写入 exe`，先结束正在运行的 `CreamPuffRobot.exe` 进程。CLI 编译（无需开 VS）：先 `cmd /c "call vcvars64.bat && ninja CreamPuffRobot"`（vcvars 在 `D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\`，Debug 用 VS 自带 ninja，Release 用 `D:\Qt\Tools\Ninja\ninja.exe`）。

## Architecture

```
CMakeLists.txt  — root: find_package(Qt6/Eigen3/OpenCV/spdlog) + 5 subdirs
├─ src/Config/  — Configuration management (ConfigManager, ProcessManager)
├─ src/HAL/     — Hardware Abstraction Layer（interfaces/core/motioncard/servo/camera/algorithm 六子目录）
├─ src/Core/    — Kinematics, CoordTransform, Trajectory
├─ src/Logic/   — PickCycleController (state machine)
└─ src/UI/      — MainWindow + 5 pages + ToggleSwitch
```

Layering (link direction): `UI → Logic → Core → HAL`; `HAL → Config` (HardwareManager 读配置喂给底层卡)

### HAL 多品牌体系

- **目录结构（已重组，2026-08）**：`src/HAL/` 下分六子目录——
  `interfaces/`（纯接口 I*.h）、`core/`（HardwareManager、AxisConverter、AxisMap、HALFactory、AxisConfigService）、`motioncard/`（BoPaiCard、SimCard）、`servo/`（XRServo、SimServo）、`camera/`（SimCamera、CameraCaptureWorker、FrameSaver、FrameConverter、SimVision、CameraManager）、`algorithm/`（SimAlgo）。
  **include 约定**：HAL 内部互 include 用平铺文件名（`#include "SimCard.h"`），靠 CMake include dir 追加全部子目录解析；外部引用用 `HAL/<子目录>/Xxx.h`（如 `HAL/core/HardwareManager.h`、`HAL/interfaces/IMotionCard.h`）。
- **接口层**（纯虚，`src/HAL/interfaces/`）: `IMotionCard`(脉冲单位)、`IAxisServo`、`IEndEffector`、`ICamera`、`IPuffAlgorithm`
- **工厂**: `HALFactory.h` 运行时字符串注册工厂，`REGISTER_MOTION_CARD/AXIS_SERVO/END_EFFECTOR/CAMERA/PUFF_ALGORITHM` 宏自动注册
- **仿真实现**: `SimCard`("SimCard")、`SimServo`("SimServo")、`SimCamera`("SimCamera")、`SimAlgo`("SimAlgo")。`SimCamera` 生成含移动目标（粉/蓝泡芙斑块 + 深度图 mm）的测试图案帧；`SimAlgo` 按 `SimVision.h` 的 `TargetSpec` 颜色匹配检测目标→`PuffResult`（像素框 + 相机内参换算的物理坐标，`z` 取深度并受 `vision.depthZMin/ZMax` 过滤，`confidence`≈覆盖率）。`SimVision.h` 是两者共用的目标规格（改图案颜色/半径必须同步，保证"生成→识别"闭环）
- **相机采集线程**: `CameraCaptureWorker`（QObject + QTimer，被移入 `QThread`）每帧调 `ICamera::CaptureFrame()` 并经 `frameReady(const CameraFrame&)` 信号广播（值类型，`qRegisterMetaType<CameraFrame>` 已注册，跨线程自动深拷贝）。`HardwareManager` 负责生命周期：`CameraOpen/Close`、`StartCameraStream(fps)/StopCameraStream`、`IsCameraStreaming`，并把 worker 的 `frameReady` 转发为自身信号供 UI 订阅
- **帧存储线程**: `FrameSaver`（QObject + QTimer，独立 QThread）异步写 PNG，`SaveImage(QImage, subdir)` 进队后由线程内 30ms 定时器落盘到 `appDir/saves/<subdir>/yyyyMMdd_HHmmss_zzz.png`，经 `imageSaved(path)/saveError(msg)` 回调。**三线程模型**：采集线程 emit 帧 → UI 线程渲染/叠加/参数下发 → 存储线程消费保存队列
- **帧渲染工具**: `FrameConverter`（HAL，静态函数）：`ColorToQImage`(RGB888)、`DepthToQImage`(深度 mm→伪彩，0 值/负值画深色)、`DrawOverlays`(绘制 PuffResult 绿框 + 物理坐标/置信度文本)。**HAL 已链接 `Qt6::Gui`**（新增）供 QImage/QPainter
- **品牌实现**: `BoPaiCard`("Bopai"，博派运动卡，`USE_BOPAI`)、`XRServo`("XRServo"，FashionStar 总线伺服舵机，`USE_XRSERVO`)。舵机 ID 由 HardwareManager 从 `config.communication.servos[]` 读取喂入
- **XRServo 协议为 FashionStar（曾用错协议）**: 真机舵机是 Fashionrobo 总线舵机，协议帧头**请求 `0x4C 0x12` / 响应 `0x1C 0x05`**，校验和 = (header+cmd+size+Σcontent) & 0xFF（求和，非取反），小端，角度 0.1°、速度 0.1°/s。命令：PING=1、SET_ANGLE=8（含周期/功率）、DAMPING=9、QUERY_ANGLE=10、SET_ANGLE_BY_VELOCITY=12、MONITOR=22（电压/电流/功率/温度/状态/角度一次取全）。参考 `D:\workspace\projects\ServoTest\FashionStar_UartServoProtocol.*`（真机实测可用）。**曾移植 bopai\puff 的 `0xF9 0xFF` 协议**，真机不认 → 点动/移动无动作、角度只回缓存（默认 90°）。`ReadAngle` 必须真实查询（cmd 10），不能回内存缓存
- **XRServo 舵机 ID 来源与点动（实测踩坑）**: 真机实测映射 **轴2(J2)→舵机 id 0、轴4(R)→id 1**（不是 1/2）。舵机总线 ID 由 HardwareManager 从 `axes.Axis_J2.portId`/`axes.Axis_R.portId` 读取（即「电控与映射」页的物理端口 ID，**改 portId 需重启程序重连才生效**；曾误从 `communication.servos[].id` 读导致改 portId 无效果、只有 id=1 的舵机动作）。点动 `MoveAtSpeed` **禁用 cmd 12 (SET_ANGLE_BY_VELOCITY)**（真机点动无响应），改为与 Go 同走 cmd 8 并把速度换算成到达周期（`interval = |Δangle|/speed×1000`，钳制 50–30000ms）
- **BoPaiCard 网口连接（MC_Open 需要 PC 与卡两端 IP）**: 本机 IP 由 `HardwareManager` 从 `communication.motionCard.pcIp` 读取，经 `IMotionCard::SetHost(pcIp, port)` 在 `Connect` 前注入（**底层卡代码禁止读 ConfigManager**，遵循 `SetAxisConfig` 同款注入模式）。卡 IP `192.168.0.1`。连不上先 `ping` 确认同网段
- **XRServo 共享串口句柄**: 同一串口总线的多个舵机实例（J2/R 都连 COM3）**禁止各自 `CreateFileA` 独占打开**——第二个必失败。`XRServo.cpp` 按端口名维护共享句柄注册表（`g_serials` + 引用计数，首个创建、末个关闭）。共享句柄下**帧事务必须持 `SharedSerial::mtx` 串口级互斥**（发送+接收整体持锁），否则两实例并发写会帧交叉。新增总线设备时沿用此模式
- **XRServo 通信阻塞与 UI 卡顿**: FashionStar 事务为同步串口读写，**不加固定 Sleep**（写后短超时轮询 `ReadFile`，单帧几 ms）。但事务仍在 UI 线程：`HardwareManager::PollTick` 舵机遥测**降频到每 5 tick（250ms）**（`servoPollCounter_`）避免每 50ms 阻塞。彻底消除需把串口 IO 移出 UI 线程（通信线程 + 异步接口），当前阶段暂缓
- **速度单位陷阱（BoPaiCard MoveAbs/MoveRel，曾差 6 个数量级）**: IMotionCard 契约速度单位为脉冲/s。默认速度必须 `cfg->maxSpeed × PulsePerUnit(axisId)`（物理速度→脉冲速度），**禁止直接把 `cfg->maxSpeed` 当脉冲速度**。`MoveJog` 处逻辑正确，可作参照
- **`HardwareManager`**（单例 QObject）: 读 `config.simulation.*Type` + `communication.*` 经工厂创建硬件；调用面为**物理单位(mm/度)**，内部经 `AxisConverter` 换算为脉冲再调底层卡；50ms QTimer 轮询状态经信号 `stateUpdated/servoStateUpdated/axisAlarm/limitTriggered` 广播
- **软限位（已在 HardwareManager 层强制执行）**: `axes.<key>.limitMin/limitMax` 由 `HardwareManager` **实时读 config**（`GetLimitMin/Max`、`IsWithinSoftLimits`，改动即生效）。`MoveAbs` 目标越界拒绝下发；`MoveJog` 启动方向已在边界拒绝；点动中越界自动停止（卡轴在 `PollTick` 停止，舵机在 `JogTick` 夹紧到边界）。越界通过 `softLimitTriggered` 信号通知 UI（手动页状态点显示"限位"橙色 + 底部提示）。`limitMin >= limitMax` 视为配置错误，不限制并 `SPDLOG_WARN`。真机 BoPai 卡点动停止有≤一个轮询周期的越界量，可后续用卡自带软限位寄存器精确化
  - **触发语义（曾踩坑）**: `PollTick` 只在轴**正在点动撞入边界**（`st.running == true`）时 `emit softLimitTriggered`。静止停在边界（如 Z/夹爪/挤出的初始最小位置 0，或 MoveAbs 恰好落在边界）**不触发**，否则启动即误报"到达软限位"。`MoveJog` 启动方向已在边界时由拒绝路径补发一次信号
  - **UI 联动（ManualControlPage）**: 用 `QVector<int> softLimitDir_`（1=撞最大/-1=撞最小/0=正常）跟踪每轴方向（曾用 bool 无法区分方向）。`RefreshSoftLimitHint()` 聚合所有 `dir != 0` 的轴，用 " / " 拼接为 `轴N 到达软限位（最大位置 X）` 逐条提示；点动离开或 Go 成功时清 0 并**重算提示**（全部清除后恢复默认 `提示：按住 +/- 按钮持续运动，松开停止`）。状态点橙色 `#ffb347`；默认提示浅蓝 `#8fd4ff`
- **`AxisConverter`**（单例）: 物理↔脉冲双向换算；参数由 HardwareManager 从 `config.axes.<key>.transmission` 读取后 `ConfigureAxis()` 喂入，**底层卡代码禁止 `#include "ConfigManager.h"`**
- **`AxisMap.h`**: 逻辑轴枚举 `LogicalAxis{J1,J2,Z,R,Gripper,Extruder}` ↔ 硬件绑定(卡轴/舵机) 映射。**已改为 config 驱动**：`HardwareManager::LoadAxisConfigsFromConfig` 读 `axes.<key>.portId` 注入 `AxisMap::SetBinding`（卡轴 index = BoPai 卡 axis 号、舵机 index = 总线 ID）；默认表仅作 config 缺失兜底。真机接线（电机-卡 axis / home）见 `doc/card_axis_test.md`：J1→axis1/home1、Z→axis2/home2、夹爪→axis4/home4、挤出→axis3（未接）。**home 输入按轴号固定（第 N 轴接 home N），软件不可改映射**。
- **IMotionCard 契约**: `MoveAbs/MoveRel/GetPosition` 均以**脉冲**为单位；`SetAxisConfig(axisId, cfg)` 下发每轴换算参数。`SimCard` 内部位置即脉冲。**BoPaiCard 曾违反契约（真机首测必现，已修复）**: `MoveAbs/MoveRel` 内部二次 `×ppu`（传入已是脉冲）、`RefreshStatus` 回读再 `/ppu`（再配 `HardwareManager::GetPosition` 的 `ToPhysical` ≈ 平方误差，J1 错约 89 倍、Z/夹爪 144/71 倍）。**修复**：MoveAbs/MoveRel 直接收脉冲、回读直接给 `lAxisPrfPos`、`PulsePerUnit` 改按 `axisType`+`lead×gearRatio` 计算（与 `AxisConverter` 同一公式）。`SimCard::SetAxisConfig` 软限位换算同步改用同一公式。**单位陷阱（曾引发回归）**: 仿真限位夹紧必须用脉冲域限位（`SimAxis::limitMinPulse/limitMaxPulse`，`SetAxisConfig` 按 `ppu = ppr*microSteps/(360 或导程)` 换算，缺省 ±1e30），**禁止拿 `cfg.limitMin/limitMax`（度/mm）直接夹紧脉冲位置**——J1 的 180° 会被当成 180 脉冲（≈0.00097°），导致每次轮询位置被归零、点动/Go 不动
- **品牌扩展**: 新增品牌实现文件放 `src/HAL/<子目录>/`（卡→`motioncard/`，舵机→`servo/`，相机→`camera/`），SDK 放根目录 `3rdparty/<brand>/`（`include/lib/bin` 三目录）。顶层 CMake POST_BUILD 自动复制 `3rdparty/bopai/bin/*.dll`。`ZMotion/Leisai` 目录已预留
- **实现注册与链接（新方案）**: 品牌/仿真实现以 `REGISTER_*` 宏在各自 cpp 内注册；**主 exe 通过 CMake `$<LINK_LIBRARY:WHOLE_ARCHIVE,HAL>` 整库链接 HAL**（MSVC → `/WHOLEARCHIVE:HAL`），强制包含全部 .obj，静态注册对象必然执行。**新增品牌只需**：写实现文件（放对应子目录）+ `REGISTER_*` 宏 + 在 `src/HAL/CMakeLists.txt` 的 `HAL_SOURCES` 加源文件（带子目录前缀，沿用 `USE_XXX` option）——**无需改任何中心代码**。曾用 `ForceLinkHALImpls()` 手写注册表（取成员地址保活 + 显式 Register 兜底），已废弃移除。若未来出现运行时换品牌/闭源分发需求，再演进为 DLL 插件 + 工厂 `Register` 注入。
- **BoPai 运动卡连接环境（真机测试注意事项）**: 官方测试软件连 wifi 可连卡；而本程序（BoPai SDK `MC_Open`）在 **wifi 开启时会 `MC_Open failed`（如 code=-6）**，必须**关闭 wifi** 才能连上卡（与旧同事工程 `PuffPickerPlugin` 行为一致）。config `communication.motionCard.pcIp` 需为真实网卡 IP、卡 IP `192.168.0.1` 同网段。
- **旋转轴换算必须含 gearRatio（曾差 100 倍）**: `AxisConverter::PhysicalPerPulse` rotation 分支**必须 `steps /= gearRatio`**（gearRatio=输出端转数/电机转数，1:100 谐波→0.01），直线轴用 `lead×gearRatio` 同理。曾漏算导致 J1 显示偏大 100 倍、软限位 ±180° 秒超。用户看到的软限位 -180~180 单位是**角度（度）**。
- **inverted（direction=反向）语义（统一逻辑坐标）**: 配置 `direction=1` 后，`MoveAbs/MoveJog` 下发目标取反（物理坐标），同时 `GetPosition()`/`PollTick` 回读位置**同样取反**返回逻辑坐标——界面显示、软限位判断（`limitMin/Max` 为逻辑范围）、按钮方向全部一致。只反转下发不回读会造成"Go +90 显示 -90"。
- **BoPai 回零（已调通，关键陷阱）**:
  - **HomeAxis 持锁后禁止调 `GetAxisStatus`**（内部对同一 `std::mutex` 二次加锁 → 死锁抛 `0xe06d7363` 崩溃；仅 `MC_HomeStart` 成功路径触发）。持锁内直接读 `lastStatus_` 即可。
  - `MC_HomeStart` 偶发返回 **1**（轴忙/未就绪竞态，`HomeStop` 刚发出即 `Start`）：需 `MC_HomeStop` + 延时 150ms 重试（≤3 次）；启动前先 `MC_Stop` + 确认使能（未使能先 `MC_AxisOn`）。
  - **`MC_HomeSns(1<<axisId)` 设该轴 HOME 高有效，极性决定搜索方向**（真机 J1：`homeSns=-1` 顺时针、`0/1` 逆时针）；配置 `homeSns>=0` 才调用。回零搜索方向 `homeDir` 独立配置。
  - `StopAxis` 必须**先 `MC_HomeStop` 再 `MC_Stop`**：仅 `MC_Stop` 可能无法退出卡端 HOME 状态机，导致后续 Jog/Trap 被拒（回零中停止后再点动无反应）。
  - 回零速度单位 **Pulse/ms**（`dHomeRapidVel/dHomeLocatVel`），经 `axes.<key>.homeRapidVel/homeLocatVel` 配置，换算公式见 `doc/config.md`（J1 3°/s→21.3、1°/s→7.1）。
  - **回零门禁 `homingActive_`**（HardwareManager）：`HomeAxis` 置位，`MoveAbs/MoveJog` 入口拒绝回零中的运动请求；`PollTick` 检测 `running` 复位或 `StopAxis/StopJog/DisableAll/EmergencyStop` 时清除。**不能用 `IsAxisBusy` 替代**（点动按钮 `autoRepeat` 每 100ms 触发，busy 会挡掉连续点动）。

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
  - **轴换算参数**（`axes.<key>.axisType` + `transmission`）: `axisType` = `"rotation"`(角度)/`"linear"`(直线 mm)，**废除曾用 `hardwareType` 兼任旋转/直线的推断**。换算公式（参考 `D:\workspace\projects\bopai\puff\config.json` 及其换算，注意参考工程公式漏了 gearRatio）:
    - rotation: `pulsesPerUnit = pulsesPerRev×microSteps / 360`（脉冲/度）
    - linear: `pulsesPerUnit = pulsesPerRev×microSteps / (lead×gearRatio)`（脉冲/mm）；`gearRatio`=电机每转的输出端转数（从动/主动，皮带 20/40 → 0.5）
  - **真实机械参数**（信捷 MP3-57H023 步进，`3rdparty` 外参考）：**J1=驱动器拨码 25600 Pulse/rev + 谐波减速比 1:100** → rotation `encoderResolution=25600`、`gearRatio=0.01`（7111.11 脉冲/度；曾误填 32000/gear1"无减速"，显示偏大 100 倍）；Z=32000/1/lead5/gear0.5（皮带20:40 + 丝杆导程5mm → 电机每圈 2.5mm）；夹爪=32000/1/lead2/gear1（电机轴丝杆状，金属环行程~20mm）；J2/R=串口舵机（minPulse500/maxPulse2500/minAngle0/maxAngle180）。**Z/夹爪每圈脉冲 32000 为初值、待真机标定**（`calibrationPending:true`），用低速点动实测反推；夹爪软限位 0–20mm 为目测值待实测修正
  - `communication.motionCard` 含 `pcIp`（本机网卡 IP，需与卡同网段）；`communication.servo.baudRate` 与 `port` 一样**存为字符串**，读取须 `getValue<std::string>` 再 `stoi`

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
- **Phase 3**：`BoPaiCard`（博派运动卡，`REGISTER_MOTION_CARD("Bopai")`，SDK 在 `3rdparty/bopai/`）、`XRServo`（FashionStar 总线舵机，`REGISTER_AXIS_SERVO("XRServo")`，舵机 ID 从 `communication.servos[]` 读取）。`USE_BOPAI`/`USE_XRSERVO` 编译开关。
- **仿真验收（Sim 模式）**：手动控制 P1–P5 已通过 GUI 自动化全量验证通过——P1 点动积分（MoveJog/StopJog 位置累计）、P2 每轴状态点（已使能绿/运行中蓝/告警红）与一键回零提示、P3 急停（SimCard 全轴 halt + 舵机 Torque OFF）、P4 舵机点动/停止（Stop 保持力矩、无 TorqueOff）、P5 未连接硬件提示（无效类型不动作）。期间修复：spinbox QSS `font-size` 触发 Qt6 polish 崩溃、SimServo 死链接注册、config 端口类型不一致。
- **软限位强制执行 + 手动页提示（已完成并验证）**：`MoveAbs`/`MoveJog` 边界拒绝、点动撞限自动停、`softLimitTriggered` 信号、状态点橙色 + 底部聚合提示（多轴用 " / " 拼接、点动离开即清除恢复默认、默认提示浅蓝醒目色）。修复：SimCard 脉冲/物理单位夹紧回归、启动时静止停在 min=0 的轴误报软限位、舵机轴二次撞限不重触发。
- **ProcessPage 动作运行速度滑条（已完成并验证）**：`QSpinBox` + `QSlider` 双向同步（1-100%），仅移动/挤压/夹爪动作显示；滑块无边框、固定长度 240 逻辑px。已通过 GUI 拖动验证同步（拖动滑条 → 输入框数值跟随）。
- **视觉检测页 + 相机子系统（已完成并验证）**：SimCamera/SimAlgo 桩 + `CameraCaptureWorker` 采集线程 + `FrameSaver` 存储线程 + `FrameConverter` 渲染工具 + VisionTestPage。GUI 已验证：导航第 4 项进入页面、开流后 FPS~20 动图（双目标移动）、单次/连续检测（发现 2 目标 + 叠加框 + 物理坐标）、RGB/深度切换（深度伪彩近蓝远红）、保存截图落盘 `saves/snapshots`、离线图片加载后静态显示并检出 2 目标、打开/关闭相机状态灯。
- **真机接入确定性 bug 修复（代码完成、编译+仿真冒烟通过，待硬件联测）**：PC IP 硬编码（`192.168.0.200`）→ `SetHost` 注入；XRServo 两实例独占打开 COM3 → 共享句柄 + 引用计数；MoveAbs/MoveRel 默认速度误用物理速度 → `maxSpeed×PulsePerUnit`；换算公式补 gearRatio、`axisType` 独立字段；ConfigPage 增 pcIp 配置项/轴类型下拉/gearRatio double；位置标签按轴类型加单位（Z/夹爪 mm）；baudRate 字符串读取。构建 EXITCODE=0、仿真冒烟存活无崩溃。
- **XRServo 真机协议修正（已完成，待真机复测）**：真机实测发现点动/移动无动作、角度错误、UI 卡顿 → 根因**协议用错**（bopai\puff 的 0xF9/0xFF 协议 vs FashionStar 0x4C12/0x1C05）+ 阻塞轮询。已按 FashionStar 协议重写帧层（Ping/SetAngle/QueryAngle/Monitor/Damping，短超时无 Sleep、事务级串口互斥），`ReadAngle` 真实查询；`PollTick` 舵机遥测降频 250ms 缓解卡顿。
- **XRServo 舵机 ID 映射与点动修复（已完成，待真机复测）**：实测映射为 J2→id0、R→id1，已改 config；舵机 ID 来源改为 `axes.Axis_J2/Axis_R.portId`（电控映射页可配，重启生效）；点动改用 cmd 8 + 周期换算（弃用 cmd 12）。编译+仿真冒烟通过。
- **XRServo 点动平滑 + 热重连（已完成）**：
  - **点动一顿一顿根因（重要）**：旧逻辑每 50ms 用「实时查询 current+3°」发新目标，目标推进 60°/s 远超设定速度（15-20°/s），舵机追不上、每 tick 被打断；且每 tick 阻塞查询导致定时器周期漂移。**第一轮修复**改为时间累积模型（目标=起点±速度×用时）+ 发送节流 ≥2°，但日志仍一顿一顿。**第二轮根因**：`MoveAtSpeed` 的 interval 按 `servo` 内部缓存 `impl_->angle` 计算，而该缓存被遥测 PollTick（250ms）刷新成**真实滞后位置**（真实位置比目标落后 5-8°），导致 interval 在 **185ms/500ms 交替**，每次新 SET_ANGLE 打断舵机并重启梯形加减速 → 一顿一顿。**最终修复**：① JogTick 的 interval 改为按「相对上次已发送目标 lastJogTarget_」的增量计算（≈185ms 均匀步进），与遥测缓存解耦；② Connect 时写 `FSUS_PARAM_ACCEL_SWITCH(44)=0x00` 关闭梯形加减速（匀速），避免高频打断时加减速曲线反复重启。
  - **热重连**：`PollTick` 遥测（250ms）检测任一舵机持续离线约 1s → `ReconnectServos()`：两舵机**必须一起 Disconnect**（共享句柄引用计数归零才真正关闭）再 Connect + 重设 ID/速度，自动恢复。
- **舵机停止"往回转一点"根因（已修复）**：`XRServo::Stop()` 曾实现为 `MoveToAngle(impl_->angle, 1000)`——缓存角度来自 250ms 前遥测，滞后于实时位置。Go 0→100 途中按停止时舵机实际已在 65° 而缓存仍为 60°，发 SET_ANGLE 目标 60° → 先反向转回 60° 再停。**改为协议标准 `CMD 24 (CONTROL_MODE_STOP, mode=1 停止后保持锁力)`**，舵机在当前位置立即停止、无反向运动、保持力矩不垂落。不能再发 Damping（会失去力矩下垂）。
- **多次点击 Go 舵机突然加速（已修复）**：Go 走 `XRServo::MoveToAngle(angle,0)`，interval 原按缓存 `impl_->angle` 计算——缓存会被遥测刷新成真实滞后位置，也可能仍是上次目标（dAngle≈0 → interval 被 clamp 到 100ms → 舵机从真实位置全速猛冲）。**修复**：① `MoveToAngle` 先真实 `QueryAngle` 再算 interval，下限 50ms（杜绝 0 周期猛冲）；② UI 层 Go 按钮点击后置灰，到位后恢复（`HardwareManager::MarkAxisBusy/CheckAxisBusy` + `axisMoveFinished` 信号 + `axisBusyUntilMs_` 时间戳，PollTick 检测到位；点动/停止/软限位夹紧也复位）。新增 `IAxisServo::GetLastMoveTimeMs()` 供到位判断（SimServo 返回 0 即时）。
- **软件退出断使能（已修复，两路径覆盖）**：`MainWindow` 析构是 `= default`，退出时无断使能调用；`HardwareManager` 单例析构时 `XRServo` 析构只 `Disconnect()`（关串口句柄），舵机保持带电锁定。**修复**：① `main.cpp` 用 `QCoreApplication::aboutToQuit` 调 `DisableAll()`（覆盖**关闭主窗口**正常退出，此时串口仍打开）；② **直接关闭 cmd 窗口 / Ctrl+C 是 Windows 强杀路径，不经过 Qt 事件循环，aboutToQuit 不触发** → 新增 `SetConsoleCtrlHandler(ConsoleSignalHandler)` 捕获 `CTRL_CLOSE_EVENT/Ctrl+C/Ctrl+Break`，调线程安全的 `HardwareManager::ShutdownHalt()`（仅硬件层断使能，不触碰 Qt 对象，可被控制台信号线程调用）。均早于单例析构执行。
- **舵机使能/扭矩语义（已确认，非 bug）**：`EnableAll→TorqueOn` = 查询当前角并发「到当前角(1000ms)」锁位（有扭矩，手转不动）；`DisableAll→TorqueOff` = `SendDamping(500)` 阻尼松力（可手转）。断电重启后舵机默认释放锁力（`FSUS_PARAM_POWER_ON_LOCK_SWITCH` 默认 0x00），可手转属正常。**点动/Go 走 SET_ANGLE 指令直接驱动电机，与使能/扭矩状态无关**——未使能也能动；如需「未使能禁止运动」需在 HardwareManager 层加门禁（当前未做）。
- **使能门禁（先使能再运动，已完成）**：
  - **规则**：手动界面点动/移动(Go)/回零/一键回零 必须先手动使能，未使能拒绝执行并提示；停止/急停不设门禁（兜底安全）；急停后必须重新手动使能才能继续操作；轴报警状态禁止运动（`lastAlarm_` 门禁，停止/急停除外）；一键回零额外要求 `IsGlobalEnabled()`。
  - **唯一事实源**：`HardwareManager::axisEnabled_`（按逻辑轴索引）。`EnableAll` 手动置 true（逐轴记录结果，失败轴保持未使能）；`DisableAll`/`EmergencyStop`/`ReconnectServos` 置 false；**`Initialize` 不再自动使能**（曾自动 EnableAxis+TorqueOn，与「不允许程序自动使能」冲突已移除）。状态变化发 `enableStateChanged` 信号。
  - **断使能/急停先停止运动中**：`DisableAll`/`EmergencyStop` 先 `jogTimer_->stop()` + `StopAll`/`EmergencyStop` 再断使能，避免「点动中断使能后仍运动」。
  - **门禁位置**：`MoveAbs`/`MoveJog`/`HomeAxis`/`HomeAll` 入口（单点拦截，UI 和未来自动流程统一走此门面）。`PickCycleController::StartCycle`/`ExecuteOneShot` 入口加 `IsGlobalEnabled` 检查。UI 状态灯改为读 `IsAxisEnabled`（不再用 servo online 冒充使能）。
  - **热重连后**：舵机扭矩归零，对应轴使能标志复位，需重新手动使能。

- **真机 J1 换算/方向/回零/按钮门禁（已完成并真机验证）**：J1 驱动器 25600 Pulse/rev + 谐波 1:100 → rotation `gearRatio=0.01`（`AxisConverter` 补除 gearRatio）；`inverted` 统一逻辑坐标（下发+回读同步取反）；回零修复（HomeAxis 内 `GetAxisStatus` 二次加锁死锁、`MC_HomeStart` 返回 1 竞态加延时重试、`MC_HomeSns` 极性定方向、`StopAxis` 先 `MC_HomeStop`）、回零速度配置化（`homeRapidVel/homeLocatVel`，3°/s→21.3、1°/s→7.1 Pulse/ms）、回零门禁 `homingActive_` + UI 统一提示。回零动作到位、数值归零。

- **HAL 目录重组 + HardwareManager 拆分（已完成，编译+冒烟通过）**：`src/HAL/` 拆六子目录（interfaces/core/motioncard/servo/camera/algorithm）；相机生命周期拆出 `CameraManager`（HardwareManager 保留 `CameraOpen/Close/Start/Stop/IsStreaming` 转发与 `frameReady` 信号）；每轴速度/单位/软限位查询拆出 `AxisConfigService`（HardwareManager 保留转发方法，UI 层零改动）。**对外 API 不变**，Debug/Release 编译通过、仿真启动存活。经验：拆分共享 `HardwareManager.h/.cpp` 与 `CMakeLists.txt` 的模块**不能并行**；外部 include 用 `HAL/<子目录>/Xxx.h`，HAL 内部平铺靠 include dir 追加子目录解析。

**下一步**：**真机电机轴（轴1 J1 / 轴3 Z / 轴5 夹爪）手动功能测试**（config 已切 `Bopai`，测试计划见 `doc/card_axis_test.md`）——J1 已测通换算/方向/回零/停止；待测：Z/夹爪 `calibrationPending` 每圈脉冲标定、Go 定位精度、遥测、拔网线异常。**自动流程遗留**：`PickCycleController` 仍硬编码卡轴号（0/2/3），与新 `portId` 映射不一致，手动测试完成后需改为走 `HardwareManager`/`AxisMap` 接口再接自动流程。随后 ZMotion/Leisai 品牌接入（SDK 目录已预留）；AutoRunPage 两个相机占位框接入 `frameReady` 实时画面；奥比中光（Orbbec）真实相机 SDK 实现 `ICamera`；状态机实现。

其余未接线部分（`qDebug()`/`SPDLOG` 桩）：AutoRunPage 的 启动/复位/停止/初始化/急停；ProcessPage 的"示教读取"；ConfigPage 的"九点标定"。
