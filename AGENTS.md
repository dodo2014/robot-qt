# CreamPuffRobot — AGENTS.md

## Project Overview

SCARA 泡芙抓取机器人控制系统。Qt6 深色主题 HMI + 仿真/真机双模式。

## 速查（Quick Start，新会话先读）

### 本文档地图

| 章节 | 内容（详细权威版见各节内指针） |
| --- | --- |
| Build | 编译/运行/Release 全流程；PowerShell 必需、LNK1168、沙箱编译（`doc/compile_guide.md`） |
| 项目记忆与日志（三套，勿混用） | AGENTS.md / doc/worklog / .history 三套记忆的职责边界 |
| Architecture | 分层依赖（UI→Logic→Core→HAL）、HAL 多品牌体系、Home Offset；详细版见 `doc/architecture.md` |
| Continuous QA & Testing | TEST_RECORD.md 强制记账法则、Sim 冒烟规范、运动学验证程序 |
| Code Conventions | C++17/W4、命名、QSS、spdlog（`SPDLOG_*` 宏）约定 |
| Config | config.json schema 要点 + ConfigManager / ProcessManager（schema 详版 `doc/config.md`、process.json 见 `doc/process.md`） |
| UI Pages / UI Conventions | 五页职责 + QSS/布局/DPI/线程陷阱 |
| Current Phase | 已完成里程碑、SequenceWorker 引擎文档、真机踩坑硬知识、下一步 |

### 常用命令速查（细节与坑见下方对应章节）

- **Debug 编译**：PowerShell 下 `& out\smoke\build_debug.bat`（默认 target `CreamPuffRobot`，已内置 LNK1168 自动重试）
- **Release 编译**：根目录 `build_release.bat`；两脚本用环境变量 `$env:TARGET='<target>'` 切换 target（如 `test_kinematics_check`）。**必须在 PowerShell 执行**（Git Bash 调 `cmd /c` 会坏 vcvars 路径）
- **运行**：`out\build\x64-Debug\CreamPuffRobot.exe`；程序**硬编码读工程根 `config/config.json`**（勿拷 exe 副本冒烟）；改动代码须**同时编 Debug + Release**
- **运动学验证（本仓库唯一 CMake 测试 target）**：`$env:TARGET='test_kinematics_check'` 后编译 → exe 在 `out\build\<config>\tests\test_kinematics_check.exe`（无参=内置回归，退出码 0/1）
- **Sim 冒烟**：PowerShell `& out\smoke\sim_smoke.ps1`；**通过判定 = 当日日志含 `Initialize complete`**（存活≠通过），日志 `log/creampuff_YYYY-MM-DD.log`
- **Tooltip/弹窗样式**：三层保险集中在 `MainWindow.cpp`（勿删减，见 UI Conventions 节）；**tipcheck 已废弃勿跑**（2026-09-03 裁定，无实际效果）；改动后**用户目视确认**
- **Git**：禁止未经用户明确要求执行 commit/push
- **测试台账**：每修完 bug / 完成功能后强制在 `TEST_RECORD.md` 追加一行；大改核心逻辑前先读它防回归

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
- **用户实际运行 Debug 版**（真机验证用 `out\build\x64-Debug\CreamPuffRobot.exe`）。改动代码后**务必同时编译 Debug + Release**，否则用户拿到的 exe 不含修复。编译前若 `LNK1168 无法写入 exe`，先结束正在运行的 `CreamPuffRobot.exe` 进程——**`out\smoke\build_debug.bat` 与根 `build_release.bat` 已内置 LNK1168 自动处理**（检测到 LNK1168 → `taskkill /IM CreamPuffRobot.exe /F` → 等 1s → 自动重编一次，无需手动干预）。CLI 编译（无需开 VS）：先 `cmd /c "call vcvars64.bat && ninja CreamPuffRobot"`（vcvars 在 `D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\`，Debug 用 VS 自带 ninja，Release 用 `D:\Qt\Tools\Ninja\ninja.exe`）。
- **Shell 注意（2026-08-31 踩坑）**：CLI 编译须在 **PowerShell** 执行（`cmd /c "call ""...vcvars64.bat"" >nul && ninja CreamPuffRobot"`，在 out\build\x64-Debug 下）；**Git Bash 直接调 `cmd //c` 会因引号嵌套/MSYS2 路径转换坏掉 vcvars 路径**（报 `'ommunity' 不是内部或外部命令`），勿用。现成编译脚本：`out\smoke\build_debug.bat`（可经 PowerShell `&` 调用）。**`.bat` 文件必须纯 ASCII**（含中文注释会被 cmd 按 GBK 解析成乱码命令，报 `'橀噺' 不是内部或外部命令`）；`.ps1` 若含中文须带 UTF-8 BOM（PowerShell 5.1 无 BOM 按 ANSI 解析）。
- **沙箱编译方案（2026-09-01 落盘）**：完整权威参考见 **`doc/compile_guide.md`**——含工具链真实路径（MSVC `D:/Program Files/Microsoft Visual Studio/.../MSVC/14.51.36231`，**无 `(x86)`**；WinSDK `D:/Windows Kits/10` 10.0.26100.0；Qt `D:/Qt/6.11.1/msvc2022_64`）、vcvars64/build_release.bat 被 reg.exe 黑名单拦截时的手工 INCLUDE/LIB/PATH 构造 + ninja 内联编译命令（Debug/Release 通用）、LNK1168 处理、首次 cmake 配置、TARGET 切换、Release 冒烟变体。**勿再以"沙箱工具链缺失"误判**——工具链齐全，只是需要手工设环境变量。

## 项目记忆与日志（三套，勿混用）

- **`AGENTS.md`（本文档）**：人工整理的精华记忆，带 `file:line`，即改即用。新会话必读。
- **`doc/worklog/YYYY-MM-DD.md`**：每日工作日志，人工精读记录（真机数据、问题/结论、明日计划）。手动创建。
- **`.history/`（OpenCode 插件自动生成，勿手改）**：自动采集每次会话的用户提问 → `.history/pending.ndjson`，按条件（最老提问超 3h 或满 100 条）自动压缩成 `.history/history.txt` 长期记忆。插件 `project-history-compress`（全局配置 `D:\dev\opencode\config\opencode\plugins\project-history.js`）手动触发压缩。**用途**：跨会话/换人/换模型快速恢复项目上下文；含噪声，作为背景参考，**不作为权威事实**（权威以 AGENTS.md + 源码为准）。`.history/` 内的 `pending.ndjson/state.json/lock` 已被插件写入 `.gitignore` 排除。新会话如需项目记忆，可读 `.history/history.txt`。
- **`.history/` WorkBuddy 版压缩（2026-08-31 起，与 OpenCode 插件同格式互通）**：automation `project-history-compress`（每天 08:30）按同一组文件/同一 marker 约定压缩，两工具交替维护互不破坏。WorkBuddy 采集靠会话约定：用户提出对项目长期有意义的提问/需求/决策时，助手 prepend 到 `pending.ndjson`（`{"ts":"ISO","text":"..."}`）；手动触发：对 WorkBuddy 说「压缩项目历史」。

## Architecture

> 完整架构梳理（目录职责/分层依赖/三条数据流/门面设计原则/拆分路线/关键不变量）见 **`doc/architecture.md`**（2026-08-28）。以下为速览。

```
CMakeLists.txt  — root: find_package(Qt6/Eigen3/OpenCV/spdlog) + 5 subdirs
├─ src/Config/  — Configuration management (ConfigManager, ProcessManager)
├─ src/HAL/     — Hardware Abstraction Layer（interfaces/core/motioncard/servo/camera/algorithm 六子目录）
├─ src/Core/    — Kinematics, CoordTransform, Trajectory（2026-08 重构：`Pose{x,y,z,r}`/`Joints{j1,j2,z,r}`，2D SCARA L1=138.83/L2=166.86，R 独立透传；`Forward/Inverse/InverseSmart/SetTCP`；**TCP 已内化为等效小臂**（`l2_eff = L2 + tcpForward_`，Z 扣 `tcpDown_`，旧 `ApplyTCPOffset/AddTCPOffset` 已删除）；**甜甜圈工作空间校验**（`rInner=|L1−L2_eff|`，原点不可达；**内外边界容差 0.1mm**——曾 0.001mm 太紧，完全伸直位示教点极径因显示舍入微超理论最大半径被误拒，2026-08-31 修复）；`CoordTransform` 为 Eigen 4×4 手眼矩阵；旧 `Pose3D`/`JointAngles` 已删除。**L1 大臂水平投影 2026-08 重测：174.35 → 138.83**）
├─ src/Logic/   — PickCycleController (视觉抓取单周期模板) + SequenceWorker（大脑执行引擎，2026-08-20 新增）
└─ src/UI/      — MainWindow + 5 pages + ToggleSwitch + KinematicsHelper.h（header-only FK 工具，供 AutoRun/Manual 页坐标刷新）
```

Layering (link direction): `UI → Logic → Core → HAL`; `HAL → Config` (HardwareManager 读配置喂给底层卡)

### HAL 多品牌体系

- **目录结构（已重组，2026-08）**：`src/HAL/` 下分六子目录——
  `interfaces/`（纯接口 I*.h）、`core/`（HardwareManager、AxisConverter、AxisMap、HALFactory、AxisConfigService）、`motioncard/`（BoPaiCard、SimCard）、`servo/`（XRServo、SimServo）、`camera/`（SimCamera、CameraCaptureWorker、FrameSaver、FrameConverter、SimVision、CameraManager）、`algorithm/`（SimAlgo）。
  **include 约定**：HAL 内部互 include 用平铺文件名（`#include "SimCard.h"`），靠 CMake include dir 追加全部子目录解析；外部引用用 `HAL/<子目录>/Xxx.h`（如 `HAL/core/HardwareManager.h`、`HAL/interfaces/IMotionCard.h`）。
- **接口层**（纯虚，`src/HAL/interfaces/`）: `IMotionCard`(脉冲单位)、`IAxisServo`、`IEndEffector`、`ICamera`、`IPuffAlgorithm`。注：`IEndEffector` + `REGISTER_END_EFFECTOR` 目前为**预留空槽**（无实现注册、从未实例化，HardwareManager 仅一处防御性 `Disconnect`）；夹爪实际是逻辑轴 `Axis_Gripper`（BoPai 卡通用运动轴 + 软限位），不走末端执行器分支
- **工厂**: `HALFactory.h` 运行时字符串注册工厂，`REGISTER_MOTION_CARD/AXIS_SERVO/END_EFFECTOR/CAMERA/PUFF_ALGORITHM` 宏自动注册
- **仿真实现**: `SimCard`("SimCard")、`SimServo`("SimServo")、`SimCamera`("SimCamera")、`SimAlgo`("SimAlgo")。`SimCamera` 生成含移动目标（粉/蓝泡芙斑块 + 深度图 mm）的测试图案帧；`SimAlgo` 按 `SimVision.h` 的 `TargetSpec` 颜色匹配检测目标→`PuffResult`（像素框 + 相机内参换算的物理坐标，`z` 取深度并受 `vision.depthZMin/ZMax` 过滤，`confidence`≈覆盖率）。`SimVision.h` 是两者共用的目标规格（改图案颜色/半径必须同步，保证"生成→识别"闭环）
- **相机采集线程**: `CameraCaptureWorker`（QObject + QTimer，被移入 `QThread`）每帧调 `ICamera::CaptureFrame()` 并经 `frameReady(const CameraFrame&)` 信号广播（值类型，`qRegisterMetaType<CameraFrame>` 已注册，跨线程自动深拷贝）。`HardwareManager` 负责生命周期：`CameraOpen/Close`、`StartCameraStream(fps)/StopCameraStream`、`IsCameraStreaming`，并把 worker 的 `frameReady` 转发为自身信号供 UI 订阅
- **帧存储线程**: `FrameSaver`（QObject + QTimer，独立 QThread）异步写 PNG，`SaveImage(QImage, subdir)` 进队后由线程内 30ms 定时器落盘到 `appDir/saves/<subdir>/yyyyMMdd_HHmmss_zzz.png`，经 `imageSaved(path)/saveError(msg)` 回调。**三线程模型**：采集线程 emit 帧 → UI 线程渲染/叠加/参数下发 → 存储线程消费保存队列
- **帧渲染工具**: `FrameConverter`（HAL，静态函数）：`ColorToQImage`(RGB888)、`DepthToQImage`(深度 mm→伪彩，0 值/负值画深色)、`DrawOverlays`(绘制 PuffResult 绿框 + 物理坐标/置信度文本)。**HAL 已链接 `Qt6::Gui`**（新增）供 QImage/QPainter
- **品牌实现**: `BoPaiCard`("Bopai"，博派运动卡，`USE_BOPAI`)、`XRServo`("XRServo"，FashionStar 总线伺服舵机，`USE_XRSERVO`)。舵机 ID 由 HardwareManager 从 `config.communication.servos[]` 读取喂入
- **XRServo 协议为 FashionStar（曾用错协议）**: 真机舵机是 Fashionrobo 总线舵机，协议帧头**请求 `0x4C 0x12` / 响应 `0x1C 0x05`**，校验和 = (header+cmd+size+Σcontent) & 0xFF（求和，非取反），小端，角度 0.1°、速度 0.1°/s。命令：PING=1、SET_ANGLE=8（含周期/功率）、DAMPING=9、QUERY_ANGLE=10、SET_ANGLE_BY_VELOCITY=12、MONITOR=22（电压/电流/功率/温度/状态/角度一次取全）。参考 `D:\workspace\projects\ServoTest\FashionStar_UartServoProtocol.*`（真机实测可用）。**曾移植 bopai\puff 的 `0xF9 0xFF` 协议**，真机不认 → 点动/移动无动作、角度只回缓存（默认 90°）。`ReadAngle` 必须真实查询（cmd 10），不能回内存缓存
- **XRServo 舵机 ID 来源与点动（实测踩坑）**: 真机实测映射 **轴2(J2)→舵机 id 0、轴4(R)→id 1**（不是 1/2）。舵机总线 ID 由 HardwareManager 从 `axes.Axis_J2.portId`/`axes.Axis_R.portId` 读取（即「电控与映射」页的物理端口 ID，**改 portId 需重启程序重连才生效**；曾误从 `communication.servos[].id` 读导致改 portId 无效果、只有 id=1 的舵机动作）。点动 `MoveAtSpeed` **禁用 cmd 12 (SET_ANGLE_BY_VELOCITY)**（真机点动无响应），改为与 Go 同走 cmd 8 并把速度换算成到达周期（`interval = |Δangle|/speed×1000`，钳制 50–30000ms）
- **XRServo 角度表示统一（2026-08-31 修复，180° 边界坑）**: **cmd 22 (MONITOR) 与 cmd 10 (QUERY_ANGLE) 角度表示不同**——MONITOR 原始角度为 0~360 宽范围（int32 解析，180.1° 原样返回），QUERY_ANGLE 为 ±180 有符号回绕（int16 解析，180.1° 被固件表示为 −179.9°）。手动页坐标面板用遥测（cmd 22）、示教读取经 `GetPosition→ReadAngle`（cmd 10）→ 同一位置两处显示相差 360°。**修复：`XRServo::QueryMonitor` 角度统一 wrap 到 [−180,180]**（`>180→−=360`、`<−180→+=360`），与 cmd 10 及 R 软限位 [−180,180] 表示一致。新角度读取路径务必沿用此约定。**三个 180 边界坑（2026-08-31 全部踩过）**：① **R 限位域须与角度表示一致**——config `Axis_R.limitMin/Max` 曾为 [0,180]，示教点 r=−179.9（cmd 10 回绕表示）被 ValidateJoints 拒 → IK 无解（用户重新示教后正常）；② **`TorqueOn` 禁止用 SET_ANGLE(查询角) 锁位**——180° 边界查询返回 −179.8，舵机从 +180 走 359.8° 翻转一整圈；改用 `CMD 24 (SendControlModeStop mode=1 停止后保持锁力)`（与 Stop 同款）只锁定不位移；③ **`MoveToAngle/MoveAtSpeed` 行程计算前必须 ±360 对齐**（新增 `Impl::AlignNear(ref, target)` 归一到目标附近）——目标 179.9 与查询 −179.9 表示差 360 → dAngle 错算 359.8° → interval 7196ms → `MarkAxisBusy(7.2s)` → WaitForAxes 干等（点间停顿 7 秒不连贯）。
- **BoPaiCard 网口连接（MC_Open 需要 PC 与卡两端 IP）**: 本机 IP 由 `HardwareManager` 从 `communication.motionCard.pcIp` 读取，经 `IMotionCard::SetHost(pcIp, port)` 在 `Connect` 前注入（**底层卡代码禁止读 ConfigManager**，遵循 `SetAxisConfig` 同款注入模式）。卡 IP `192.168.0.1`。连不上先 `ping` 确认同网段
- **XRServo 共享串口句柄**: 同一串口总线的多个舵机实例（J2/R 都连 COM3）**禁止各自 `CreateFileA` 独占打开**——第二个必失败。`XRServo.cpp` 按端口名维护共享句柄注册表（`g_serials` + 引用计数，首个创建、末个关闭）。共享句柄下**帧事务必须持 `SharedSerial::mtx` 串口级互斥**（发送+接收整体持锁），否则两实例并发写会帧交叉。新增总线设备时沿用此模式
- **XRServo 通信阻塞与 UI 卡顿**: FashionStar 事务为同步串口读写，**不加固定 Sleep**（写后短超时轮询 `ReadFile`，单帧几 ms）。但事务仍在 UI 线程：`HardwareManager::PollTick` 舵机遥测**降频到每 5 tick（250ms）**（`servoPollCounter_`）避免每 50ms 阻塞。彻底消除需把串口 IO 移出 UI 线程（通信线程 + 异步接口），当前阶段暂缓
- **速度单位陷阱（BoPaiCard MoveAbs/MoveRel，曾差 6 个数量级）**: IMotionCard 契约速度单位为脉冲/s。默认速度必须 `cfg->maxSpeed × PulsePerUnit(axisId)`（物理速度→脉冲速度），**禁止直接把 `cfg->maxSpeed` 当脉冲速度**。`MoveJog` 处逻辑正确，可作参照
- **加速度单位一致陷阱（BoPaiCard，2026-08 修复）**: 卡端 `TTrapPrm.acc / TJogPrm.dAcc` 单位是 **Pulse/ms²**，而 config `maxAccel/maxDecel` 是物理单位（°/s² 或 mm/s²）。曾直接把 `cfg->maxAccel` 塞入（参考工程 bopai\puff 也这么写，同为 bug），实际加速度与配置不符。修复：`BoPaiCard::AccelToPulse(axisId, phys) = phys × PulsePerUnit(axisId) / 1e6`，MoveAbs/MoveRel/MoveJog 三处统一换算。**真机复测注意**：同一加速度数值下电机加减速行为将变化
  - **acc 单位为何是 Pulse/ms²**（交叉验证，勿再改错）: 速度单位已确认是 Pulse/ms（回零 `dHomeRapidVel=21.3` 对应 J1 3°/s，`MC_SetVel(vel/1000)`）；`dHomeAcc=1.0` 配合 21.3 Pulse/ms 快速段，若 acc 单位是 Pulse/s² 则加速段需 6 小时，但回零实测数秒完成 → 加速度单位必为 Pulse/ms²，`AccelToPulse` 的 `÷1e6` 正确。
  - **回归（2026-08，J1 点动/Go 无反应）**: `AccelToPulse` 依赖的 `BoPaiCard::PulsePerUnit` rotation 分支**漏了 gearRatio**（linear 分支有、rotation 漏），导致 J1（gearRatio=0.01）PulsePerUnit 少 100 倍 → 加速度 0.0007111 Pulse/ms²（应为 0.07111）→ 加速到 27°/s 需 ~270s → 表现为"无反应"。仅 J1 受影响（挤出 gearRatio=1、Z/夹爪走 linear 分支）。修复：rotation 分支 `steps /= gearRatio`（与 `AxisConverter::PhysicalPerPulse` 对齐）。
  - **maxAccel 改完立即生效（2026-08）**: `AxisConfigService::GetMaxAccel` 实时读 config（同 `GetMaxSpeed` 模式）；`HardwareManager::MoveAbs/MoveJog` 卡轴分支运动前 `motionCard_->SetAccel(index, GetMaxAccel(axis))` 刷新卡内快照再下发。故 maxAccel 与 maxSpeed/软限位一样**改完无需重启**（旧版 maxAccel 只在 `LoadAxisConfigsFromConfig` 启动时快照进卡，改完需重启程序才生效）。`maxDecel` 无独立 config 字段，恒跟随 `maxAccel`。
- **`HardwareManager`**（单例 QObject）: 读 `config.simulation.*Type` + `communication.*` 经工厂创建硬件；调用面为**物理单位(mm/度)**，内部经 `AxisConverter` 换算为脉冲再调底层卡；50ms QTimer 轮询状态经信号 `stateUpdated/servoStateUpdated/axisAlarm/limitTriggered` 广播
- **软限位（已在 HardwareManager 层强制执行）**: `axes.<key>.limitMin/limitMax` 由 `HardwareManager` **实时读 config**（`GetLimitMin/Max`、`IsWithinSoftLimits`，改动即生效）。`MoveAbs` 目标越界拒绝下发；`MoveJog` 启动方向已在边界拒绝；点动中越界自动停止（卡轴在 `PollTick` 停止，舵机在 `JogTick` 夹紧到边界）。越界通过 `softLimitTriggered` 信号通知 UI（手动页状态点显示"限位"橙色 + 底部提示）。`limitMin >= limitMax` 视为配置错误，不限制并 `SPDLOG_WARN`。真机 BoPai 卡点动停止有≤一个轮询周期的越界量，可后续用卡自带软限位寄存器精确化
  - **触发语义（曾踩坑）**: `PollTick` 只在轴**正在点动撞入边界**（`st.running == true`）时 `emit softLimitTriggered`。静止停在边界（如 Z/夹爪/挤出的初始最小位置 0，或 MoveAbs 恰好落在边界）**不触发**，否则启动即误报"到达软限位"。`MoveJog` 启动方向已在边界时由拒绝路径补发一次信号
  - **拦截必须区分运动方向（2026-08 修复，与 J1 同源）**: `PollTick` 软限位 `StopJog` 曾只看 `position 越界 + running`，**不看运动方向** → 夹爪惯性冲到 `0.2`（>max=0）后按住「松开」每 50ms 被 StopJog 打断、只能一点点按；`Go` 回界内（如到 -3）也被立即停。修复：按逻辑位置增量 `delta = st.position - lastPollPos_[i]` 判断，**仅拦截"仍朝越界方向运动"**（越上界且 delta≥0 / 越下界且 delta≤0），**朝边界内运动（离开越界区）放行**。成员 `lastPollPos_` 在 `Initialize` 用 `GetPosition` 初始化基线，每轮 `PollTick` 更新。`MoveAbs/MoveJog` 入口校验本就按目标/方向判越界，无需改
  - **UI 联动（ManualControlPage）**: 用 `QVector<int> softLimitDir_`（1=撞最大/-1=撞最小/0=正常）跟踪每轴方向（曾用 bool 无法区分方向）。`RefreshSoftLimitHint()` 聚合所有 `dir != 0` 的轴，用 " / " 拼接为 `轴N 到达软限位（最大位置 X）` 逐条提示；点动离开或 Go 成功时清 0 并**重算提示**（全部清除后恢复默认 `提示：按住 +/- 按钮持续运动，松开停止`）。状态点橙色 `#ffb347`；默认提示浅蓝 `#8fd4ff`
- **`AxisConverter`**（单例）: 物理↔脉冲双向换算；参数由 HardwareManager 从 `config.axes.<key>.transmission` 读取后 `ConfigureAxis()` 喂入，**底层卡代码禁止 `#include "ConfigManager.h"`**
- **`AxisMap.h`**: 逻辑轴枚举 `LogicalAxis{J1,J2,Z,R,Gripper,Extruder}` ↔ 硬件绑定(卡轴/舵机) 映射。**已改为 config 驱动**：`HardwareManager::LoadAxisConfigsFromConfig` 读 `axes.<key>.portId` 注入 `AxisMap::SetBinding`（卡轴 index = BoPai 卡 axis 号、舵机 index = 总线 ID）；默认表仅作 config 缺失兜底。真机接线（电机-卡 axis / home）见 `doc/test/card_axis_test.md`：J1→axis1/home1、Z→axis2/home2、夹爪→axis4/home4、挤出→axis3（未接）。**home 输入按轴号固定（第 N 轴接 home N），软件不可改映射**。
- **IMotionCard 契约**: `MoveAbs/MoveRel/GetPosition` 均以**脉冲**为单位；`SetAxisConfig(axisId, cfg)` 下发每轴换算参数。`SimCard` 内部位置即脉冲。**BoPaiCard 曾违反契约（真机首测必现，已修复）**: `MoveAbs/MoveRel` 内部二次 `×ppu`（传入已是脉冲）、`RefreshStatus` 回读再 `/ppu`（再配 `HardwareManager::GetPosition` 的 `ToPhysical` ≈ 平方误差，J1 错约 89 倍、Z/夹爪 144/71 倍）。**修复**：MoveAbs/MoveRel 直接收脉冲、回读直接给 `lAxisPrfPos`、`PulsePerUnit` 改按 `axisType`+`lead×gearRatio` 计算（与 `AxisConverter` 同一公式）。`SimCard::SetAxisConfig` 软限位换算同步改用同一公式。**单位陷阱（曾引发回归）**: 仿真限位夹紧必须用脉冲域限位（`SimAxis::limitMinPulse/limitMaxPulse`，`SetAxisConfig` 按 `ppu = ppr*microSteps/(360 或导程)` 换算，缺省 ±1e30），**禁止拿 `cfg.limitMin/limitMax`（度/mm）直接夹紧脉冲位置**——J1 的 180° 会被当成 180 脉冲（≈0.00097°），导致每次轮询位置被归零、点动/Go 不动
- **品牌扩展**: 新增品牌实现文件放 `src/HAL/<子目录>/`（卡→`motioncard/`，舵机→`servo/`，相机→`camera/`），SDK 放根目录 `3rdparty/<brand>/`（`include/lib/bin` 三目录）。顶层 CMake POST_BUILD 自动复制 `3rdparty/bopai/bin/*.dll`。`ZMotion/Leisai` 目录已预留
- **实现注册与链接（新方案）**: 品牌/仿真实现以 `REGISTER_*` 宏在各自 cpp 内注册；**主 exe 通过 CMake `$<LINK_LIBRARY:WHOLE_ARCHIVE,HAL>` 整库链接 HAL**（MSVC → `/WHOLEARCHIVE:HAL`），强制包含全部 .obj，静态注册对象必然执行。**新增品牌只需**：写实现文件（放对应子目录）+ `REGISTER_*` 宏 + 在 `src/HAL/CMakeLists.txt` 的 `HAL_SOURCES` 加源文件（带子目录前缀，沿用 `USE_XXX` option）——**无需改任何中心代码**。曾用 `ForceLinkHALImpls()` 手写注册表（取成员地址保活 + 显式 Register 兜底），已废弃移除。若未来出现运行时换品牌/闭源分发需求，再演进为 DLL 插件 + 工厂 `Register` 注入。
- **BoPai 运动卡连接环境（真机测试注意事项）**: 官方测试软件连 wifi 可连卡；而本程序（BoPai SDK `MC_Open`）在 **wifi 开启时会 `MC_Open failed`（如 code=-6）**，必须**关闭 wifi** 才能连上卡（与旧同事工程 `PuffPickerPlugin` 行为一致）。config `communication.motionCard.pcIp` 需为真实网卡 IP、卡 IP `192.168.0.1` 同网段。
- **旋转轴换算必须含 gearRatio（曾差 100 倍）**: `AxisConverter::PhysicalPerPulse` rotation 分支**必须 `steps /= gearRatio`**（gearRatio=输出端转数/电机转数，1:100 谐波→0.01），直线轴用 `lead×gearRatio` 同理。曾漏算导致 J1 显示偏大 100 倍、软限位 ±180° 秒超。用户看到的软限位 -180~180 单位是**角度（度）**。**`BoPaiCard::PulsePerUnit` 也必须同样处理**（2026-08 曾漏，导致加速度换算错 100 倍、J1 点动/Go 无反应）——两处换算必须与 `AxisConverter` 保持同一公式，改动任一处时务必同步检查另一处。
- **inverted（direction=反向）语义（统一逻辑坐标）**: 配置 `direction=1` 后，`MoveAbs/MoveJog` 下发目标取反（物理坐标），同时 `GetPosition()`/`PollTick` 回读位置**同样取反**返回逻辑坐标——界面显示、软限位判断（`limitMin/Max` 为逻辑范围）、按钮方向全部一致。只反转下发不回读会造成"Go +90 显示 -90"。
- **BoPai 卡告警/状态字诊断（2026-08，轴失步排查）**:
  - `lAxisStatus[]` 状态字含 20 位：`SV_ALARM`(0x2 驱动器报警)、`FOLLOW_ERR`(0x10 跟随误差)、`ESTOP`(0x1)、软限位(0x4/0x8)、硬限位(0x20/0x40)、`ARRIVE`(0x800)、`HOME_FAIL`(0x400000) 等。`MotorStatus` 已扩展 `alarm/followError/estop/softLimitPositive/softLimitNegative/arrive/statusWord`，`BoPaiCard::RefreshStatus` 全量解析。
  - **日志落盘（边沿触发）**: `HardwareManager::PollTick` 用"异常签名"（报警/跟随误差/急停/硬软限位组合）边沿检测，变化时 `SPDLOG_WARN` 打印完整状态字（`axis N abnormal -> 跟随误差(失步) | statusWord=0x...`）。仅边沿打印，避免 50ms 轮询刷爆日志。辅助函数 `DescribeAxisStatus`（`HardwareManager.cpp` 匿名 namespace）拼可读描述。
  - **UI**: 手动页状态点红色时 tooltip 显示具体告警原因（驱动器报警/跟随误差(失步)/急停/硬软限位）；`followError/estop` 计入红色告警态、卡端软限位计入橙色限位态（`ManualControlPage::alarmDetail_`）。
  - **关键认知（J1 开环步进无编码器）**: `lAxisPrfPos` 是**规划位置**（发了多少脉冲），非实际位置；**失步/堵转（起步卡住、运动中咔咔响、数值到目标实际没到）卡检测不到、不产生告警**——`FOLLOW_ERR` 需编码器闭环才有意义，`SV_ALARM` 是驱动器报警输入（失步不触发）。开环失步只能靠机械实测位移发现。加速应**先逐步提 maxAccel 到起步失步临界、再提 maxSpeed 到高速失步临界**，退回一格；仍不满足则查驱动器电流拨码是否额定。
- **BoPai 回零（已调通，关键陷阱）**:
  - **HomeAxis 持锁后禁止调 `GetAxisStatus`**（内部对同一 `std::mutex` 二次加锁 → 死锁抛 `0xe06d7363` 崩溃；仅 `MC_HomeStart` 成功路径触发）。持锁内直接读 `lastStatus_` 即可。
  - `MC_HomeStart` 偶发返回 **1**（轴忙/未就绪竞态，`HomeStop` 刚发出即 `Start`）：需 `MC_HomeStop` + 延时 150ms 重试（≤3 次）；启动前先 `MC_Stop` + 确认使能（未使能先 `MC_AxisOn`）。
  - **`MC_HomeSns` 极性设置**：参数是**全局位掩码**（每 bit 对应一轴），`homeSns=1`→`|=1<<axisId`（高有效）、`0`→`&=~(1<<axisId)`（低有效）。**曾永远传 `0x1` 导致 homeSns=0/1 行为一致**（已修复）。`homeSns=-1` 不调用（沿用卡默认）。极性**影响搜索方向**及 `AXIS_STATUS_HOME_SWITCH` 位的解读。J1 真机 `homeSns=0`（低有效）配合 `homeDir=1` 电机逆时针搜索。
  - **`ulHomeMaxDis` 必须非零**：部分 BoPai 卡将 0 解释为"搜索 0 距离"→ 立即完成，设配置字段 `homeMaxDis`（Pulse，J1 设 1,500,000）。曾硬编码 4000000 测试通过，已改为 config 驱动。
  - **PollTick 软限位会误杀回零**（根因：软限位 min=0 且当前位置=0 时，`position<=lo` → `st.running=true` → `StopJog(MC_Stop)` 终止回零搜索。修复：`if(st.running && !homingActive_[i])` 回零中的轴不拦截。这也是"改软限位到 -180~90 就能回零"的原因——lo=-180，位置不在边界）。
  - **回零完成检测必须加最短保护期（2026-08-26，真机误杀复现）**: 完成判定 `homingActive_[i] && !st.running` 曾为单拍判定——MC_HomeStart 到卡端 running 置位存在启动间隙（含 Start=1 时 HomeStop+150ms 重试窗口），间隙内一拍即误判"完成"清掉 homingActive_ 保护；随后压界起步的回零搜索（J1 未回零逻辑位 -102==limitMin 且 inverted 搜索方向朝越界侧 delta<0）被软限位 StopJog 误杀 → 报"轴1到达软限位"且未碰原点开关。修复：`homeStartedMs_[ai]` 记录发起时刻（HardwareManager），发起后 1s 内 running=false 不判完成。挤出轴（恒 0 压 limitMin=0）同源。
  - **`MotorStatus` 新增 `homeSwitch`/`homeFail`**：解析 `AXIS_STATUS_HOME_SWITCH`（HOME 信号电平）和 `AXIS_STATUS_HOME_FAIL`，HomeAxis 前后打印供真机诊断。
  - `StopAxis` 必须**先 `MC_HomeStop` 再 `MC_Stop`**：仅 `MC_Stop` 可能无法退出卡端 HOME 状态机，导致后续 Jog/Trap 被拒（回零中停止后再点动无反应）。
  - 回零速度单位 **Pulse/ms**（`dHomeRapidVel/dHomeLocatVel`），经 `axes.<key>.homeRapidVel/homeLocatVel` 配置，换算公式见 `doc/config.md`（J1 3°/s→21.3、1°/s→7.1）。
  - **回零门禁 `homingActive_`**（HardwareManager）：`HomeAxis` 置位，`MoveAbs/MoveJog` 入口拒绝回零中的运动请求；`PollTick` 检测 `running` 复位或 `StopAxis/StopJog/DisableAll/EmergencyStop` 时清除。**不能用 `IsAxisBusy` 替代**（点动按钮 `autoRepeat` 每 100ms 触发，busy 会挡掉连续点动）。
  - **回零配置字段汇总**：`homeDir`（搜索方向 0/1）、`homeSns`（-1/0/1 极性）、`homeRapidVel`/`homeLocatVel`（Pulse/ms）、`homeBackDis`（碰信号后精定位反向退出脉冲数，0=不退出）、`homeMaxDis`（最大搜索距离 Pulse，0=不限制，**实际须设非零**）。
  - **完整回零分析报告**：现象/三个根因/关联问题/修复对照表/排查顺序/新轴 Checklist 见 `doc/test/homing_debug_report.md`，后续轴调试优先参考。

### Home Offset（逻辑零点机制，2026-08 阶段 1 落地）

- **语义**：`逻辑角度 = 机械角度 - homeOffset`（机械零点=回零位置，逻辑零点=工艺示教零点）。J1 offset=102°、J2 offset=28°、其余 0。**所有界面/软限位/运动学统一使用逻辑坐标**。
- **实现位置（方案 Y，只改门面）**：全部落在 `HardwareManager` 层，`AxisConverter` 保持纯"机械方向坐标↔脉冲"不感知偏移。落地点：`MoveAbs` 下发 `目标机械 = inv ? -(逻辑+off) : (逻辑+off)`（`HardwareManager.cpp:375`）；`MoveJog` Servo 分支起点 `jogStartPos_=start-off`；`JogTick` 下发/夹紧 `MoveToAngle(逻辑+off)`；`GetPosition` Card/Servo 分支 `(inv?-phys:phys)-off`；`PollTick` Card 回读、Servo 遥测（`angleDeg -= off`）同样减 offset。**只有门面一个转换点，不得在别处再加减偏移**。
- **限位按逻辑坐标校验**：J1 物理 0~110° → 逻辑 `limitMin:-102 / limitMax:8`；J2 物理 0~180 → `limitMin:-28 / limitMax:152`。回零后逻辑位置为 **-offset**（机械归 0），H08 用例已核对区间。`MoveAbs/MoveJog` 入口校验、PollTick 撞界夹紧、UI 提示全部用逻辑坐标。
- **舵机（J2/R）HomeAxis**：归机械 0° 后逻辑自动 = -offset；Servo 分支限位夹紧目标 `MoveToAngle(逻辑+off)` 即机械角度。
- **已自测（TEST_RECORD.md）**：驱动测试 24/24 通过（2026-08-19），含回零→-102/-28、Go 逻辑 5→机械 -107（inverted）、点动撞界自动停、越界拒绝/反向放行。
- **曾修两个隐藏 bug**：① `SimCard::SetAxisConfig` rotation 分支漏 `gearRatio`（ppu 错 100 倍 → 点动撞界夹紧失效），`denom=360*cfg.gearRatio`；② `MoveJog` 启动边界检查容差 1e-6 太紧（撞界停止后位置 7.99996875 浮点舍入），改 0.01。

## Continuous QA & Testing（防回归硬规约）

1. **测试台账制度**：项目根目录存在 **`TEST_RECORD.md`**，记录所有已调通的功能与边界条件测试。
2. **强制记账法则**：每次彻底解决一个 Bug 或完成一个新功能后，**必须**主动用文件编辑工具在 `TEST_RECORD.md` 追加一行测试记录：编号、**模块**、测试场景/动作、期望结果、状态（**🟢 已通过**）、踩坑记录/备注。

   **编号规则（`TR-###`，2026-09-03 起）**：首列为唯一编号 `TR-001`…`TR-0NN`，按追加顺序递增。**追加时用「当前最大号 +1」，禁止重排、禁止复用已删除记录的编号**；台账内及跨文件引用**一律用 `TR-###`，不得用行号**（行号随增删漂移，历史曾引用"TEST_RECORD.md 第 45 行"之类，2026-09-03 已全部改为编号）。编号一旦分配即与该条记录终身绑定，记录内容可调整但编号不变。
3. **修改前验算**：计划大范围重构或修改核心逻辑（`HardwareManager`、`ProcessManager`、`PollTick/JogTick`、换算/软限位/回零等）前，**必须先读取 `TEST_RECORD.md`**，脑内推演改动是否会破坏已标为 **🟢 已通过** 的用例；有风险则调整方案或补回归验证。
4. **Git 提交须用户明确要求**：**禁止主动执行 `git commit/push`**——即使完成一批改动并整理好提交信息，也必须等用户明确说"提交/commit"才执行；push 同理。工作完成时最多提示"可提交"，由用户决定时机。
5. **Sim 冒烟规范（2026-08-31 定稿，2026-09-02 修订）**：用标准脚本 `out\smoke\sim_smoke.ps1`——就地临时改**工程根** `config/config.json` 为 SimCard/SimServo → `-WindowStyle Hidden` 独立窗口启动 **15s**（Debug 版从进程启动到 `HardwareManager::Initialize` 实测约 7s，原 8s 会在初始化完成前 kill 进程导致日志截断、被误读成"初始化卡死"）验证存活 → try/finally 保证恢复配置 → **新增 `SMOKE_INIT_COMPLETE` 日志判定**（存活≠通过：断言框挂起时进程也存活，必须查当日日志含 `Initialize complete`）。**禁止拷贝 exe 输出目录做副本冒烟**——开发机版程序路径硬编码 `PROJECT_SOURCE_DIR`（main.cpp 读 `PROJECT_SOURCE_DIR/config/config.json` 与 `log/`），拷贝副本无效（曾误生成 2.1GB C 盘副本）。产物仅工程根 `log/creampuff_YYYY-MM-DD.log`。
6. **运动学验证程序（2026-08-31 固化）**：`tests/test_kinematics_check.cpp`（root CMake 已挂 tests 子目录，独立 target 不进主程序依赖链）。无参运行 = 内置真机摆位回归（2026-08-31 两组实测）+ 限位内 20 组 FK↔IK 往返自检，退出码 0/1 可脚本调用；`fk <j1> <j2> <z> <r>` / `ik <x> <y> <z> <r> [curJ2]` 手动查询。参数自动读 config（改 config 无需改程序）。编译：`$env:TARGET='test_kinematics_check'` 后跑 `out\smoke\build_debug.bat` 或根 `build_release.bat`（两脚本已支持 TARGET 环境变量选 target，默认 CreamPuffRobot 不变；PowerShell→bat 传参用环境变量，`%~1` 在该链路曾失真）。产物在 `out\build\<config>\tests\test_kinematics_check.exe`（**在 tests/ 子目录**，CMake 定义于 tests 作用域；Debug 目录可能仅剩 .ilk/.pdb，缺失时以 Release 产物为准）。

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
  - **轴换算参数**（`axes.<key>.axisType` + `transmission`）: `axisType` = `"rotation"`(角度)/`"linear"`(直线 mm)，**废除曾用 `hardwareType` 兼任旋转/直线的推断**。换算公式（`AxisConverter::PhysicalPerPulse` 与 `BoPaiCard::PulsePerUnit` 必须一致，**参考工程 `bopai\puff` 漏了 gearRatio 勿照抄**）:
    - rotation: `pulsesPerUnit = pulsesPerRev×microSteps / (gearRatio×360)`（脉冲/度）
    - linear: `pulsesPerUnit = pulsesPerRev×microSteps / (lead×gearRatio)`（脉冲/mm）；`gearRatio`=电机每转的输出端转数（从动/主动，皮带 20/40 → 0.5）
  - **真实机械参数**（信捷 MP3-57H023 步进，`3rdparty` 外参考）：**J1=驱动器拨码 25600 Pulse/rev + 谐波减速比 1:100** → rotation `encoderResolution=25600`、`gearRatio=0.01`（7111.11 脉冲/度；曾误填 32000/gear1"无减速"，显示偏大 100 倍）；Z=25600/1/lead5/gear0.5（皮带20:40 + 丝杆导程5mm → 电机每圈 2.5mm，10240 脉冲/mm；**2026-08-26 阶段 3 已真机标定与理论一致**，config `calibrationPending` 仍 true 未置 false）；夹爪=40000/1/lead2/gear1（电机轴丝杆状，金属环行程~20mm；驱动器为信捷 XINJE DP3L1-224，拨码 SW5-SW8 全 OFF=40000 Pulse/rev，**已据此标定**，`calibrationPending:false`）；J2/R=串口舵机（minPulse500/maxPulse2500/minAngle0/maxAngle180）。夹爪软限位 [-5, 0]mm 为目测值待实测修正
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
   - **单步执行 UI 现状（2026-09-03 真机验证通过，阶段 5）**：顶栏两行——第一行「当前方案: [名称框] [方案下拉] 新增方案 **切换方案**(原「确认切换」仅改文案) 删除方案」，第二行「**方案调试** | 单步执行 | 停止 | [状态标签]」（`ProcessPage.cpp:125-214`）。`m_statusLabel`（:192）接 `SequenceWorker::stateChanged` + `schemeFinished`/`interrupted`/`errorOccurred` 显示 ● 空闲/● 运行中/● 单步暂停/● 执行选中动作/✅ 完成/■ 已停止/⏹ 已急停/✖ 出错——**此前 stateChanged 全项目 6 处 emit、0 处 connect，界面无任何执行状态反馈，2026-09-03 补齐接线**。实现要点：① 终态保留（StartExecution 在 schemeFinished/interrupted 后补发 `stateChanged("空闲")`，会用 `m_finalStatus/m_finalColor` 保留终态，新会话启动清空）；② 急停与业务停止都发 interrupted，靠 `IsGlobalEnabled()` 区分文案（急停断使能）。单步按钮**执行期同步禁用**（`OnStepExecute` 点击栈内即 `setEnabled(false)`，ProcessPage.cpp:1033/1042，非等信号）——D2：stepGo 是 bool 非计数，执行期点击会预置放行使下一个暂停点被跳过连跑两个动作，同步禁用把点击收敛到暂停期。切换/删除方案执行期门禁 `IsExecutionActive()`（:946 = m_stepActive || worker->IsRunning()，:774 删除、:814 切换，弹窗「单步执行尚未结束…」）。
   - **执行选中动作（2026-08-31）**: 动作列表按钮行「下移」后绿色运行图标按钮（仅图标 + tooltip）→ `OnRunSelectedAction` → `SequenceWorker::RunSingleAction` 单动作独立执行（详见 SequenceWorker 节）；未选动作弹窗、与单步会话互斥（启动前 `ResetStepSession`）。
   - 移动动作: 点位 QTableWidget (点名称/X/Y/Z/R/姿态) + 添加/删除/上移/下移/示教按钮。内存仍为扁平 PointData，仅 JSON 序列化为 `coord`/`joints` 嵌套结构
   - 识别动作: 识别类型/曝光时间/匹配模板/置信度阈值
   - 挤压动作: 挤出量/挤出速度/回抽量/回抽速度
   - 延时动作: 延时时间 ms
   - 夹爪动作: 闭合/张开 选择 + 行程输入框（mm，2026-09-02 新增：值为轴5 绝对目标坐标，0=夹紧、负=松开，默认闭合 0.00/张开 -3.00，实时写回 `action.gripperTarget`，与手动页轴5 Go 同语义）
   - **动作运行速度**（`m_speedPercentRow`，仅移动/挤压/夹爪动作显示，其余隐藏）: `QSpinBox`(1-100, 后缀 " %") + `QSlider`(横向) 双向同步，各自 `blockSignals` 防回环；保存时读 `m_speedPercentSpin->value()`。**拖动即实时写回当前动作 `action.speedPercent`（`ApplySpeedPercentToCurrentAction`，2026-08-31 起改即生效，不必先点保存）**。滑块 `setFixedWidth(240)` 固定长度、无拉伸（`addWidget` 不带 stretch），长度调整改 `ProcessPage.cpp:469`
4. **VisionTestPage** — 视觉检测（导航第 4 项，位于「设备与配置」前）: 相机控制（型号下拉/序列号/分辨率/FPS/打开关闭/开始停止采集 + 状态点）＋ 预览（RGB/深度切换、识别框叠加开关、FPS/时间戳/分辨率、保存截图）＋ 算法测试（型号下拉/单次检测/连续检测/结果表格，表格列 = 序号/置信度/X/Y/Z/偏航/U/V/宽/高）＋ 离线图片加载（`getOpenFileName`→`CameraFrame`→`Detect`）＋ 参数（置信度阈值/Zmin/Zmax/曝光，`QDoubleSpinBox::valueChanged` 实时写 `ConfigManager`）。
   - 采集来源统一走 `HardwareManager::frameReady`（采集线程）→ `OnFrameReady` 存 `latestFrame_` 渲染；检测结果存 `lastResults_`，RGB/深度/截图共用 `BuildDisplayImage()`（含叠加）。截图经 `FrameSaver` 异步写盘。
   - 型号下拉在构造函数从工厂 `AvailableTypes()` 填充（主程序以 `$<LINK_LIBRARY:WHOLE_ARCHIVE,HAL>` 整库链接 HAL，工厂静态注册进程启动即生效，下拉通常非空；为空时回退 "SimCamera"/"SimAlgo" 字符串，与工厂注册名一致）。
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
- **Tooltip QSS 真机制（2026-09-01 真坑，沙箱 tipcheck 像素验证）**: 应用级 stylesheet 生效后，QSS 的 `QToolTip { ... }` 选择器**不匹配**实际弹出的 QTipLabel 实例（私有类名是 QTipLabel/QFrame/QWidget，无 QToolTip）；`QToolTip::setPalette()` 在 application stylesheet 环境下被 QStyleSheetStyle **完全绕过**——看似生效的修复实际上 tooltip 仍 Fusion 深色默认，与深色 UI 混淆看不清。**唯一真生效**是 QSS `QToolTip, QTipLabel { ... }` 规则（QSS 按类名字符串匹配，私有类也命中）+ `QToolTip::setPalette(白底深字)` 作辅（双保险最强）。**当前实现（三层保险，全部集中在 `MainWindow.cpp`，2026-09-01 五修定稿）**：① QSS `QToolTip, QTipLabel { background-color:#ffffff; color:#1a1a1a; border:1px solid #cfd8e0; border-radius:4px; padding:4px 8px; font-size:13px; }`（`ApplyGlobalStyle`）；② `SetupToolTipStyle()` 内 `QToolTip::setPalette`（Active/Inactive/Disabled 三组）+ `setFont`；③ **`TipStyleFilter`**（匿名 namespace，qApp 级事件过滤器）——tooltip 窗口首次 Polish/Show 时对其【实例】直接 setStyleSheet 白底深字，**实例级样式表优先级高于 application 级，架构上不存在能覆盖它的层叠路径**。调用点固定在 `MainWindow` 构造函数末（`SetupUI()` 之后）。**任何后续改动：禁止把其中一层"优化"掉或注释掉**（已发生 3 次回退：gemini 注释 setPalette、gpt 删空 QTipLabel 规则），三段要么全留要么整体替换。**任何后续 tooltip/弹窗样式改动闭合方式 = 用户目视确认**（tipcheck 已废弃，见后文，不再作为验证手段）。tipcheck 编译/运行命令在 `doc/compile_guide.md` 第 8 节；所有 `setToolTip` 调用（ProcessPage 6 个按钮、ManualControlPage 状态点告警）自动走全局白底。**⚠ tipcheck 已废弃（2026-09-03 用户裁定）**：`out/smoke/tipcheck/` 为独立复刻程序（内置 QSS 模拟主程序方案），**与实际按钮的真实 tooltip 环境不一致，实际无效果**——**禁止再跑 tipcheck 或引用其结果作为验证/判定依据**；compile_guide.md 第 9 节已删除。tooltip/弹窗样式改动闭合方式 = **用户目视确认**。**遗留（已搁置，2026-09-03）**：ProcessPage 动作列表下方 6 个按钮（新建/编辑/删除/上移/下移/执行选中）的 tooltip 显示问题**按用户要求暂时搁置不管**。**同段前置规则约束（2026-09-03）**：`ApplyGlobalStyle` 顶部新增 `QLabel { color:#b8cce3 }`（须**位于** `QToolTip, QTipLabel` 规则**之前**）——修复 tip 修复回归：48b8d74 把宽泛 `QWidget{color:#b8cce3}`（类型选择器匹配所有 QWidget 子类）收窄为 `QMainWindow,QDialog` 后，QMessageBox 内部消息 QLabel 无规则匹配 → 文字回退默认 palette WindowText 变黑。**勿把 QLabel 规则移到 QTipLabel 之后**（同 specificity 后出现者胜 → tip 应用级文字被浅字覆盖），勿删（弹窗会再次黑字）。
- **DPI 缩放注意**: 当前部署屏为 **150% DPI**，物理像素 = 逻辑像素 × 1.5。`setFixedWidth(240)` 实际约 360 物理px；做 GUI 自动化坐标换算/像素测量时先确认缩放，别拿物理坐标直接当逻辑值。
- **单位符号放输入框外（spin 右侧独立 QLabel）**: 规则：**禁止用 `QDoubleSpinBox::setSuffix()` 显示单位**（会把单位挤进框内、抢占数字宽度并挡滚动按钮）。单位用框外独立 `QLabel`，由 `loadAxis`/轴类型切换 `setText` 动态更新（rotation→°、°/s、°/s²；linear→mm、mm/s、mm/s²）。位置：`ManualControlPage` 速度列/目标列、`ConfigPage` 电控与映射的 6 个 spin（maxSpeed/maxAccel/jogSpeed/limitMin/limitMax/home）。
- **spin+单位同行对齐（ConfigPage，Gemini 微调）**: `QFormLayout` 的 field 位置只能放一个 widget，需把 `spin+单位QLabel+弹簧` 打包成 `QHBoxLayout` 容器。**关键：spin `addWidget(spin)` 不带拉伸因子 `,1`，末尾加 `addStretch()` 弹簧** —— 弹簧把 spin+单位整体向左顶，紧贴行 Label，单位不会漂到行最右侧远离标签；`setSpacing(6)` 控制 spin 与单位间距。
- **手动页列宽因子（colStretches，Gemini 微调「类型」列）**: 表头与数据行共用同一 `colStretches` 比例因子列表，`addWidget(w, factor)` 传入。当前值 `{85, 40, 110, 75, 90, 75, 100, 75, 60, 55, 34}`：**类型图标列 40**（图标"留白"，与轴名同列视觉对齐）；速度列 110、目标列 100（容纳数值 + 框外单位 mm/s、°/s）。调列宽只改这一处，表头/数据行自动同步。

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

- **回零互锁（Homing Interlock，2026-08-31 新增，第二道安全门禁）**：J1/Z/夹爪/挤出为开环步进，**断电丢失绝对坐标**——未回零前软件"以为"的坐标与机械实际不符，发绝对定位可能撞机（急停是事后补救，回零是事前保障）。实现：
  - **状态**：`HardwareManager::IsSystemHomed()`/`IsAxisHomed(axis)` + `axisHomed_`（无硬件绑定轴恒 true）+ 信号 `homeStateChanged(bool)`。程序实例内**全轴回零成功后置 true 并保持**（断使能/急停不丢坐标不清位）。
  - **未接硬件轴豁免（`AxisConfig.enabled=false`）**：config `axes.Axis_X.enabled=false`（未接电机，如轴6 只接卡）→ 初始化 `SetBinding(None)`（显式覆盖，防 AxisMap 默认表兜底回 Card）→ 绑定后 `axisHomed_[i]=true` 同步（构造时按默认表置 false，须纠正）→ 不参与回零互锁/使能判定（`IsGlobalEnabled` 本就跳过 None）。`HomeAll` 跳过 None 轴。接电机后 config 改回 `enabled=true` 即恢复参与。
  - **完成判定（2026-08-31 修正，勿回退为"下发即置位"）**：`HomeAxis` 只是**下发**回零指令（卡轴/舵机异步运动），**成功下发不置 homed**，只置 `homingActive_` + `homeStartedMs_`。置位点必须在**轴实际到位观测**：卡轴 = `PollCardAxis` 检测 `homingActive_ && !st.running && 过 1s 保护期`（回零真正结束）→ `MarkAxisHomed`；**舵机 = `PollServoTelemetry` 检测 `homingActive_ && |机械角|≤1°`**（`MarkAxisHomed`）。否则回零中途急停会被误判已回零 → 系统未回零却放行绝对定位（曾真机复现：一键回零中途急停 → Go 成功运动）。**⚠ 舵机判定必须用机械角，绝不能用逻辑角（2026-09-01 真机踩坑）**：`HomeAxis` 下发给舵机的是 `MoveToAngle(0.0)` 即**机械角 0**；`ReadTelemetry()` 原始 `angleDeg` 即为机械角，而 `toLogicalAngle` 会减 `homeOffset`（J2 offset=28）→ 逻辑角≈**−28°**，`|逻辑角|≤1°` 永不满足 → **J2（axis1）永不 MarkAxisHomed → `IsSystemHomed()` 恒 false → 一键回零完成后 Go 仍被拒「系统未回零」**（日志实证 axis1 全程无 `home done`，而 axis0/2/4/R 均有）。UI 推送仍用逻辑角副本（toLogicalAngle 包一层再 push）；R 轴 offset=0 两种判定等价不受影响。**打断复位**：`AbortHoming(ai)`（回零被打断 → 回零中的绑定轴 homed 复位 false，部分完成不算完成，须重新回零；已回零完成轴保持）接入 `EmergencyStop`/`DisableAll`/`StopAxis`/`StopJog`。
  - **拦截（`MoveAbs` 入口，使能门禁之后）**：未回零拒绝绝对定位（`MoveAbs rejected: system not homed`）。**JOG 点动（MoveJog）不设限**（供操作员脱困盲开）；回零本身（HomeAxis/HomeAll）不受限。
  - **引擎**：`RunSequence`/`RunSingleAction` 使能检查后加 `IsSystemHomed` → `errorOccurred("系统未回零，请先一键回零")`。
  - **UI 联动**：AutoRunPage 启动按钮初始禁用 + `homeStateChanged(true)` 激活；ProcessPage 单步（`m_stepBtn`）/执行选中按钮联动 + 两槽内双保险弹窗；ManualControlPage Go 失败提示「系统未回零，禁止绝对定位！请先执行一键回零（点动可脱困）」。

- **真机 J1 换算/方向/回零/按钮门禁（已完成并真机验证）**：J1 驱动器 25600 Pulse/rev + 谐波 1:100 → rotation `gearRatio=0.01`（`AxisConverter` 补除 gearRatio）；`inverted` 统一逻辑坐标（下发+回读同步取反）；软限位校验坐标系修正（`MoveJog` 改用逻辑方向 `direction` 而非物理方向 `effDir`，修复 inverted 轴上边界误拦/反向放行）。回零通过三轮根因定位后完整修复：(1) `MC_HomeSns` 位掩码 bug（曾永远传 0x1 导致 homeSns=0/1 行为一致，已改为读-改-写单 bit）；(2) `ulHomeMaxDis` 设非零值→卡实际搜索（部分固件 0="不搜"）；(3) **PollTick 软限位误杀回零**——软限位 0-90° 且位置=0 时 `position<=lo`→`st.running`→`StopJog` 终止回零，修复为 `!homingActive_[i]` 门禁。回零速度配置化（`homeRapidVel/homeLocatVel/homeBackDis/homeMaxDis`，Pulse/ms 或 Pulse，`homeMaxDis` 由 config 驱动替代曾硬编码的 4000000）。`MotorStatus` 增 `homeSwitch`/`homeFail` 诊断位。回零动作到位、数值归零。

- **舵机轴 ±180° 角度环绕归一化（2026-09-02，已完成并真机验证）**：舵机角度域 [-180,180]（`XRServo` QueryAngle 已统一 wrap），物理位置在软限位边界（R=180°）时遥测读数在 {179.9, 180, -179.9} 跳动（±0.1° 噪声跨 ±180 表示边界，**物理正常**）；示教存入 wrap 值 -179.9 后执行被 `Kinematics::ValidateJoints`（r∈[rMin,rMax]）判"目标点不可达"，`MoveAbs` 软限位同样会拒。修复：`HardwareManager::NormalizeRotationAngle(axis, angle)`——以 [limitMin,limitMax] 中心为基准 wrap 到中心 ±180 窗口（R 域 [0,185]：-179.9→180.1；J2 域 [-90,152] 中心 31 窗口内读数恒等）。**应用点 4 处**：`PollServoTelemetry::toLogicalAngle`、`GetPosition` Servo 分支、`MoveAbs` 入口（仅 Servo 绑定轴，先归一化再软限位校验）、`SequenceWorker::MoveToPoint` 的 `target.r`（**必须在 IK 前**，ValidateJoints 先于 MoveAbs 拦截）。**下发不额外 wrap**：`XRServo::MoveToAngle` 已有 ±180 硬 clamp（与官方软件一致，180.1→180 物理差 0.1°）。**硬约束**：① `checkServoHomeDone` 回零判定必须用原始机械角，归一化绝不能掺入；② 卡轴不做归一化（脉冲域连续无 wrap）；③ `Axis_R.limitMax=185` 仅软件校验域（舵机物理上限 180，官方实测发 185 走到 180 即停），限位余量用于吸收边界读数 180.1；④ `GetLimitMin/Max` 实时读 config，归一化窗口自动跟随限位编辑。

- **工艺流程夹爪动作行程输入框（2026-09-02，已完成并真机验证）**：夹爪动作编辑 = 下拉（张开/闭合）+ 行程 `QDoubleSpinBox`（mm，框外独立单位标签，QSS 禁 font-size 用 setFont），值为**轴5 绝对目标坐标**（用户拍板：0=夹紧、负=松开，"输入什么值执行什么值"，与手动页轴5 Go 同语义）；默认闭合 0.00 / 张开 -3.00（切换下拉按方向记忆值回填，不覆盖已填值）；hint 行实时显示轴5 软限位。数据：`ActionData.gripperTarget`（mm）落盘 process.json，旧文件缺失按方向补默认；改值实时写回动作（不必先点保存，防重蹈速度滑条坑）。执行层 `ExecuteGripper` 重写：取 `gripperTarget`、`IsWithinSoftLimits` 越界拒绝（不静默改写目标）、速度=`GetMaxSpeed(Gripper)×speedPercent`（旧版未传速度走卡内默认）、到位超时按 `GetAxisBusyMs` 动态兜底（低速固定 10s 会误判）。**行为修正**：旧实现 `isGripperOpen ? GetLimitMax(0) : GetLimitMin(-5)` 与物理事实（0=夹紧）相反——"打开夹爪"此前实际是夹紧；旧 process.json「关闭夹爪」的 isGripperOpen 本为 true，迁移后需人工把行程改 0.00（已改）。

- **HAL 目录重组 + HardwareManager 拆分（已完成，编译+冒烟通过）**：`src/HAL/` 拆六子目录（interfaces/core/motioncard/servo/camera/algorithm）；相机生命周期拆出 `CameraManager`（HardwareManager 保留 `CameraOpen/Close/Start/Stop/IsStreaming` 转发与 `frameReady` 信号）；每轴速度/单位/软限位查询拆出 `AxisConfigService`（HardwareManager 保留转发方法，UI 层零改动）。**对外 API 不变**，Debug/Release 编译通过、仿真启动存活。经验：拆分共享 `HardwareManager.h/.cpp` 与 `CMakeLists.txt` 的模块**不能并行**；外部 include 用 `HAL/<子目录>/Xxx.h`，HAL 内部平铺靠 include dir 追加子目录解析。

- **工艺流程单步执行真机验证 + UI 收口（2026-09-03 单步全通过 🟢；2026-09-04 自动运行 5.6-5.15 亦全通过 🟢）**：ProcessPage「单步执行」真机验证 5.1-5.5 + 边界补 1/3/4/5 全部 🟢（TR-061–TR-066）；AutoRunPage 启动整跑 5.6-5.15 共 10 用例全部 🟢（TR-069）：Move 整跑无大甩臂、Gripper/Delay/Vision(无相机降级)/schemeFinished/运行中停止/运行中急停(5.12)/门禁/运行中改参/运行中关窗。已知开口：5.9 相机降级路径（接相机后复测）、5.11 跨页互斥待完善（TR-066）。三项 UI 改动：① 顶栏两行布局 + 执行状态标签（`stateChanged` 接线补齐，见 UI Pages·ProcessPage 节）；② 单步按钮执行期**同步禁用**（D2 修复，改动仅在 UI 层，SequenceWorker 零改动）；③ 切换/删除方案执行期门禁 `IsExecutionActive()`。三个实测结论（防回归）：**点击次数 N+1**（WaitForStep 在循环体内，最后一个动作也挂起）；**Stop 非就近刹车**（只置 cancel 不走停轴，执行完当前动作才停，见急停节）；**急停后 IsSystemHomed 仍 true → 单步按钮不置灰，点击被使能门禁静默拦（只写日志不弹窗）**；未使能拒绝同样仅日志无弹窗。**遗留**：三页操作互斥（手动/工艺流程/自动运行）未实现（单步暂停中切自动运行页可被 running 门禁拒，但无跨页主动互斥与提示）；ProcessPage 按钮无 objectName（自动化需按文本定位）；单步执行/运行期间 UI 的 `m_currentActionLabel` 不随执行刷新（仅列表选中驱动）。


### SequenceWorker 大脑执行引擎（2026-08-20 阶段 2 轮 A 落地，引擎层自测通过）

- **定位**：按 `SchemeData` 逐动作执行的流程编排引擎（`src/Logic/SequenceWorker.h/.cpp`），与 `PickCycleController`（视觉抓取单周期模板）职责互补。UI 已接线（T7–T10，2026-08-20/21 完成）。
- **接口**：`RunSequence(const SchemeData&)`（使能门禁 `IsGlobalEnabled`，未使能拒绝+errorOccurred）/ **`RunSingleAction(const SchemeData&, int actionIndex)`（2026-08-31 新增，单动作独立执行，与 RunSequence 共用 running 门禁互斥；未使能发 errorOccurred，运行中/越界静默返回 false）** /`Stop()`（安全停止+interrupted，保持使能）/`EmergencyStop()`（+断使能）/`SetStepMode(bool)`/`NextStep()`；信号 `actionStarted/actionFinished/`**`singleActionFinished`**`/schemeFinished/interrupted/errorOccurred/logMessage/stateChanged`。
- **单步语义（N+1 次点击，勿按 N 次预期）**：单步 = `SetStepMode(true)` + `RunSequence`，与连续运行共用同一 `ExecuteActions()` 主循环，唯一分歧在 `SequenceWorker.cpp:306-314`——每动作 `ExecuteAction` 成功后 `emit stateChanged("单步暂停")` → `WaitForStep()`（20ms 轮询 `cancel||stepGo` 的 QEventLoop，退出时 `stepGo.store(false)`）→ `NextStep()` 置 `stepGo=true` 唤醒。**`WaitForStep` 在 for 循环体内：最后一个动作执行完也会暂停**，须再点一次才退出循环发 `schemeFinished` → **N 个动作需 1 次启动 + N 次释放 = N+1 次点击**（引擎层自测 S04「5 动作 5 次 NextStep」即含启动首点）。`NextStep()` 返回 false（非 running/非 stepMode）时 UI 不应置灰按钮。
- **线程模型**：`moveToThread` 到独立 QThread（worker 线程执行循环），**全部 HardwareManager 调用经 `InMainThread` 辅助用 `QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)` 回主线程执行**，与 PollTick 串行避免数据竞争；`RunSequence` 内 `invokeMethod("StartExecution", QueuedConnection)` 排队到 worker 线程。主线程只短暂执行硬件操作，UI 不卡（S08 已验）。
- **动作实现**：Move=`InverseSmart`（TCP 已内化）→ 逐轴 `MoveAbs`（先 J2/R 舵机后 J1/Z 卡轴）→ `WaitForAxes` 轮询 `IsAxisBusy` 到位（30s 兜底）；Vision=SimCamera 采帧+SimAlgo 检测+`CoordTransform::CameraToRobot` 手眼换算（无相机/算法时模拟延时）；Extrude=挤出量/回抽量（绝对目标=挤出量−回抽量，Extruder 限位 [0,100] 恒正）；Delay=`QEventLoop`+20ms 轮询可被 cancel 打断；Gripper=取 `action.gripperTarget`（轴5 绝对目标 mm，0=夹紧/负=松开），`IsWithinSoftLimits` 越界拒绝（不静默夹紧），速度=`GetMaxSpeed(Gripper)×speedPercent`，到位超时按 `GetAxisBusyMs` 动态兜底（2026-09-02 重写，旧版 `isGripperOpen?GetLimitMax:GetLimitMin` 与物理方向相反已废弃）。
- **中断语义**：`cancel_` 原子标志 + 等待循环（`WaitForCancelOrTime`/`WaitForAxes`/`WaitForStep`）20ms 轮询退出 → `ExecuteActions` 检测后发 `interrupted("用户停止")`。
- **参数来源**：`ReloadFromConfig()` 从 config 实时读 `kinematics.links.*`/`tcpCalibration.*`/`axes.*.limit*` 喂入 Kinematics/CoordTransform，与 ConfigPage 编辑一致。
- **自测**：临时驱动 17/17 通过（2026-08-20，仿真 Sim 全家桶）：S01 完整方案 5 动作、S03 Vision 闭环（基座 8.6,-0.8,55.0 conf=1.00）、S04 单步 5 次 NextStep、S05 Stop、S06 EmergencyStop、S07 未使能拒绝、S08 线程不卡。测试驱动已清理。
- **单动作独立执行（2026-08-31，编译+Sim 冒烟通过，GUI 交互待真机验证）**：`RunSingleAction` 镜像 `RunSequence` 模式（running 门禁 → 深拷贝 scheme → `invokeMethod("StartSingleExecution", QueuedConnection)`）；`StartSingleExecution` 镜像 `ExecuteActions`（cancel 区分 interrupted/errorOccurred；**失败双发 errorOccurred 为既有兜底语义**——ExecuteAction 内部多数失败路径已发，外层再发覆盖 MoveAbs 静默失败路径，勿"优化"掉）。UI：ProcessPage 动作列表按钮行「下移」后绿色运行图标按钮（40×32，`makeActionIcon(5)` 白色实心播放三角 + tooltip），`OnRunSelectedAction` 启动前 `ResetStepSession()` 清单步会话 + `ReloadFromConfig()`，拒绝路径（未使能/运行中/越界）立即恢复按钮；singleActionFinished/interrupted/errorOccurred 三信号恢复按钮。实施前对原计划逐条代码核验：8 处修正全部属实，另补 1 处硬伤（ProcessPage.h 缺 `class QPushButton;` 前置声明）。
- **已知改进项（2026-08-31 用户指出）**：`MoveToPoint` 目前一律用 `InverseSmart`（双解就近）求解，**无视示教点 `posture` 字段**；且示教读取 `OnTeachRead` **硬编码 `posture=elbow_up`**（未按实际 J2 符号记录）。应一并改进：① 示教按 `cur.j2` 符号记录 posture（≥0→elbow_up / <0→elbow_down）；② 执行按 posture 用 `Inverse(target, out, elbowUp)` 确定性解，`InverseSmart` 仅作 posture 缺失/未知兜底。触发背景：点_002 报 `IK(Smart)` 时用户质疑"elbow 已确认为何还走 ik smart"（当时直接根因是 R 限位域不一致，见 XRServo 节，但该改进仍必要）。
- **动作运行速度实时生效（2026-08-31 修复）**：ProcessPage 滑条/输入框 `valueChanged` 经 `ApplySpeedPercentToCurrentAction(v)` **实时写回当前动作 `speedPercent`**（改即生效，不必先点「保存动作」；落盘仍由保存按钮负责）。曾只有 `OnSaveAction` 才写 action.speedPercent，拖动滑条后直接执行读旧值（用户反馈"速度没变化"）。`ExecuteMove` 侧 `speedScale = qBound(0.01, speedPercent/100, 1.0)` 链路本就正确。

**下一步**：**UI 接线 + 参数接入（阶段 2 轮 B，T7–T10）**——MainWindow 创建 SequenceWorker+QThread（T7）；AutoRunPage 5 按钮接线（启动=运行页内方案下拉选中方案 / 复位=HomeAll / 停止=Stop / 初始化=Initialize / 急停=EmergencyStop）+ 坐标面板随 stateUpdated 实时 FK 刷新 + 日志框接 logMessage + 两相机框接 frameReady（T8）；ProcessPage「示教读取」用当前关节 FK 填充点位（T9）；ConfigPage 运动学/TCP 标定参数喂入 Kinematics::SetParams/SetTCP/CoordTransform（T10）。随后 AutoRunPage 两个相机占位框接入 `frameReady` 实时画面；奥比中光（Orbbec）真实相机 SDK 实现 `ICamera`；真机电机轴（轴1 J1 / 轴3 Z / 轴5 夹爪）剩余手动功能测试（Z/夹爪 `calibrationPending` 每圈脉冲标定、Go 定位精度、遥测、拔网线异常）。自动流程中 `PickCycleController` 视觉抓取单周期模板后续作为 SequenceWorker Vision 动作的委托实现。

### UI 接线 T7–T10 + 评审修复 D1–D17（2026-08-20/21 完成，编译+冒烟通过）

- **T7**：`MainWindow` 创建 `SequenceWorker`+QThread（worker 线程 `finished→deleteLater`）；退出走 `ShutdownWorker`（Stop(cancel) → quit → wait(20ms)+processEvents 循环 → terminate 兜底），防退出挂死。
- **T8 AutoRunPage**：方案下拉（选中方案传给 RunSequence）/5 按钮（启动/复位=HomeAll/停止=Stop/初始化=Initialize/急停=EmergencyStop）/坐标面板 FK 实时刷新/日志框接 logMessage/双相机 QLabel 接 frameReady。
- **T9 ProcessPage**：「示教读取」= GetPosition→FK 填充点位表并 push 进 `action.points` + save（仅 Move 类型动作）。
- **T10 ConfigPage**：运动学 L1/L2/Z0/h1 与 TCP 编辑完成即发 `paramsChanged` → `SequenceWorker::ReloadFromConfig()`（running 门禁：运行中跳过防数据竞争）。
- **关键修复（D1–D17，评审发现全部修复，清单见 TEST_RECORD.md / doc/worklog/2026-08-21.md）**：D1 方案下拉时序（`ProcessManager::load()` 提前到 MainWindow SetupUI 前 + showEvent 刷新）；D2 失败后启动按钮恢复；D4 ShutdownWorker；D5/D6 坐标面板用 stateUpdated/servoStateUpdated 缓存关节位 + Kinematics 成员缓存（避免 50ms 读舵机串口）；D11 急停无条件先 HardwareManager::EmergencyStop；D16 抽 `src/UI/KinematicsHelper.h`（UI 层统一 FromConfig/ReadConfigParams，Core 不依赖 Config）。
- **moc 陷阱**：信号参数类型前向声明不够，AutoRunPage.h 需 include `HAL/interfaces/ICamera.h` 等完整定义；外层 lambda 必须捕获 this（内层捕 this 报 C3493）。

### 全局急停升舱 + 业务停止就近（2026-08-28；2026-09-03 阶段 5 真机验证通过）

- **理念**：急停全局唯一入口（MainWindow 顶栏圆形红色按钮），业务停止就近跟随场景（ProcessPage 单步执行旁停止按钮）。
- **MainWindow 顶栏**：右侧新增 46px 圆形急停（完整 QSS selector）；点击 = `HardwareManager::EmergencyStop()` + `sequenceWorker_` 判空调 `EmergencyStop()`（**必须含 worker 中断**，否则方案运行中急停后 SequenceWorker 会继续跑下一动作）。
- **信号驱动状态清理**：`HardwareManager` 新增 `emergencyStopTriggered` 信号（`EmergencyStop()` 内 emit，唯一触发点）；ManualControlPage 连接它做 `ResetAxisStates()`+提示，AutoRunPage 连接它恢复启动按钮/状态标签/日志（原 `OnEmergencyClicked` 删除，UI 响应改 `OnEmergencyTriggered` 槽）。
- **按钮变化**：AutoRunPage 5→4（删"⚔ 急停"）；ManualControlPage 顶部 4→3（使能/断使能/一键回零）；ProcessPage「单步执行」右侧红色「停止」已接 `SequenceWorker::Stop()`（业务停止就近，经 `SetSequenceWorker` 注入 worker，与 AutoRunPage 同模式）；ProcessPage「单步执行」经 `SetStepMode(true)+RunSequence` / `NextStep` 接单步模式（此前为 stub，本轮补齐）。
- **语义区分**：停止=业务（可恢复、保持使能）、急停=安全（断使能、需重新使能）。TEST_RECORD 记 🟡，阶段 5 用例 5.6/5.7 覆盖真机验证。
- **⚠ 停止语义实测修正（2026-09-03，勿按"就近刹车"预期）**：标题/上文"业务停止就近"仅指"按钮位置就近跟随场景"——`SequenceWorker::Stop()` 实际**只置 `cancel` 原子标志、不下发任何停轴命令**（SequenceWorker.cpp:210-215，卡轴/舵机继续走完当前 MoveAbs 才被 WaitFor* 循环感知停止），并非"立即刹车"。真机实测（TR-063）：单步中点「停止」会**执行完当前动作才停**，然后会话复位、可重新单步（从第一个动作开始）。`Stop()` 首行 `if (!running) return;`（:212），非运行态调用静默无效无日志。急停（EmergencyStop）才是真刹车（硬断使能）。

### 真机联调进展（2026-08-24/25 进行中）

- **阶段 1 已完成**（计划与实测记录见 `doc/test/real_machine_plan_phase2.md`）：1.1 连接使能 ✅；1.3 J1 回零显示 -102° ✅。**1.2 预期修正**：未回零时卡轴显示逻辑=机械规划位(0)−offset（J1 显示 -102 属设计行为）；舵机绝对编码器显示真实机械角−offset。**1.4 J2 回零精度**：实测 -27.4/-27.2/-27.3（机械停 0.6~0.7°，FashionStar 死区 ~1.5°+金属套松动放大波动）——决定紧固后复测、暂不软件补偿（0.6°≈末端 1.7mm 可接受）。**1.5 R 回零**：稳定 0.2° 系统性偏差，暂不处理。
- **舵机回零"卡回零中"阻塞 jog/Go（已修）**：根因 servo 轴 `homingActive_` 只能靠 PollTick 卡轴 `st.running` 清除（舵机不在该向量）。修复：`CheckAxisBusy` busy 超时时同步清 `homingActive_` 并 emit `axisMoveFinished`（HardwareManager.cpp 兜底）；UI 层 `homingAxes_` 向量由 axisMoveFinished 驱动提示更新。
- **舵机软件掉线防误判（2026-08-25，待真机复测）**：真机运行中出现"掉线→1s 全量重连→点动被拦"。根因三层：① `Transaction` 响应解析遇脏字节固定比较 rx[0..1] 且不丢弃 → 必超时；② 校验失败立即放弃；③ 单次查询失败即 `online=false`、连续 4 tick(1s) 即全量重连——EMI/USB 抖动的瞬时坏帧被放大。加固：滑动字节对齐 + 校验失败丢一字节继续等帧；`ReadTelemetry` 失败重试 1 次；超时 40→60ms；离线阈值 2s 且重连前 **Ping 门卫**（`IAxisServo::Ping()` 新增接口，SimServo 返回 connected_），Ping 通仅清计数不重连。**重连后无法点动属门禁正确行为**（重连强制复位使能，扭矩已释放须人工确认），不做自动使能；UI 在 offline→online 边沿提示「舵机已重连（扭矩已释放），请重新执行全局轴使能」（ManualControlPage::OnServoStateUpdated）。重连成功后直接点「全局使能」即可恢复（无需先断使能）。
- **重连指数退避 + 日志降噪（2026-08-26，日志分析驱动）**：真机日志 09:42 启动即 `Open port COM3 failed`（CreateFileA 失败，USB 转串口不可用），无退避下 80 分钟重连 1646 次、26320 条"串口未打开" WARN。修复：重连改指数退避 2s→4s→…→30s cap（成功归零）；`ReconnectServos` 失败打 GetLastError 原因、不再无条件 "reconnect done"；`ReadTelemetry` 仅 online→offline 边沿打 WARN（`onlineWarned_` 去重）。**根本原因在 USB 转串口硬件层**（线/供电/驱动/占用），软件仅缓解刷屏——现场排查需查 USB 线缆接触、供电、是否有程序占用 COM3。日志分析要点：读 `log/creampuff_YYYY-MM-DD.log`，区分「启动即失败」（初始化 `Open port failed`）与「运行中断线」（遥测超时/发送失败），前者是环境问题后者才是软件抖动。
- **重连抖动循环治理（2026-08-28，"成功归零"盲区，待真机复测）**：设备"半死"（时通时断）时指数退避**失效**——重连**成功**即归零 → 1-2s 后又 offline → 立即再重连（日志实证 1 分钟 8+ 次全量重连、52 次 Connected to COM3），且重连本身 CloseHandle→CreateFileA→DTR 翻转**扰动总线** → 越连越抖；离线时遥测 250ms × 2 次尝试 × 60ms 超时**占满 UI 线程** → 急停/全局使能点击排队秒级无响应（**"卡死"真因**）。修复五项：① 重连成功也冷却 30s（重连硬上限 2 次/分钟；在线后遥测成功自动恢复 online 不依赖重连，仅句柄失效拔 USB 需重连、冷却最多延迟恢复 30s）；② 离线时遥测降频 250ms→1s（`pollDiv` 5→20）；③ 已离线 `ReadTelemetry` 单次尝试（重试仅用于防在线误判）；④ Ping ok skip 退避 2s→4s→8s→15s cap（`servoPingSkipCount_`，恢复在线清零）；⑤ 删 XRServo 重连纯噪声 INFO 2 条（Reusing shared handle / Servo id set to）。
- **舵机失联硬件定性（2026-08-28，官方软件复测实锤）**：现场用官方 FashionStar 测试软件**也连不上 id=0（J2）**，断电重连后恢复，舵机已连续通电 24h+ → **排除本软件全部链路**，属舵机侧状态类故障（最可能：MCU 长时间运行异常/过热/供电劣化；J2 负载最重+全程锁力发热大，日志失联多以 `ping J2=false` 呈现与之吻合）。**与 8-26 的 `COM3 Open failed` 是两个不同故障**（那次 PC 侧 USB 打不开串口；这次串口能开、舵机不应答），排查方向勿混淆。运维规程：不要 24h 连续通电、不用时断使能（释放锁力降温）或断电、失联先断电重启舵机；复现时记录周期/摸 J2 外壳温度/带载测电压；证据齐全后向供应商反馈。舵机死机期软件重连无效属预期（冷却 30s 设计合理）；TEST_RECORD 相关 🟡 复测需等硬件稳定后进行。
- **连接状态分段着色 + 全局使能连接门禁（2026-08-25，已真机验证；2026-09-02 修正补全）**：① 手动页顶部「运动卡/舵机」连接状态标签（`ManualControlPage::OnConnectionChanged`）富文本**分段着色**：未连接黄 `#e0a520`、已连接绿 `#7ed67e`——**span 必须整段包裹「运动卡: X」**（初版只包状态词，前缀走默认 palette 在深色主题下发暗，2026-09-02 真机反馈后改整段）；② `connectionChanged` 信号从"仅 Initialize emit"改为 **`PollTick` 连接状态边沿检测**（成员 `connStateInited_/lastCardConnected_/lastServoConnected_`，Initialize 同步初值防首次 tick 重复广播）——舵机离线/热重连/运动卡掉线都会广播，手动页标签与自动运行页日志实时更新；③ **AutoRunPage 的 HardwareManager 连接（stateUpdated/servoStateUpdated/frameReady/connectionChanged）必须在构造函数连接**，不能放 `SetSequenceWorker`（MainWindow 在 `HardwareManager::Initialize()` 之后才调它 → 会错过 Initialize 那次 connectionChanged emit，启动连接日志丢失，2026-09-02 真机踩坑）；连接变更经 lambda 写入自动运行页日志框「硬件连接状态变更：运动卡: X | 舵机: X」；④ EnableAll 入口连接门禁（任一未连接直接拒绝，axisEnabled_ 保持全 false），UI 判据同步 AND、提示「未连接硬件，命令可能无效」。DisableAll 不设门禁（安全操作）。

**下一步**：真机联调**阶段 4 重做修复已全部真机复测通过（2026-08-28 🟢）**；舵机重连抖动治理真机观察中（硬件定性见上）。SequenceWorker 方案真机验证**阶段 5 全部通过（2026-09-03 单步 5.1-5.5 + 2026-09-04 自动运行 5.6-5.15，TR-041/TR-040/TR-069）**，待做**阶段 6（AutoRunPage UI 接线真机）与阶段 7（异常/回归）**。待补：**三页操作互斥**（手动/工艺流程/自动运行，2026-09-03 用户确认后续补充，TR-066）；**5.9 真实相机未接走降级路径，接相机后需复测**（Orbbec SDK 待同事提供后接入 `ICamera`）。

### 真机联调阶段 3 已完成（2026-08-26，Z 标定 + 运动学全链路闭环）

- **3.1-3.4 全部通过**：低速点动标定、行程实测、软限位、标定收尾；**Z0 基准与 h1（大臂落差）标定完成，`calibrationPending` 可置 false**。
- **Gemini 五步底层验证全部通过**：参数（Pulse/Rev=25600+导程+HOME 极性）/ 方向与单轴回零 / Go -100 钢卷尺实测精确 100mm / 软限位探底拦截 / **3D 坐标系验收**（Z0=470/h1=175/tcpDown=130：回零 Z 显示 165.0mm、Go -165 尖端触桌面显示 0.0mm）。
- **手动页坐标面板 FK 接线（2026-08-26）**：全局使能下方 X/Y/Z/R 曾为硬编码假数据不更新；已接 stateUpdated/servoStateUpdated → 关节缓存 → KinematicsHelper::FromConfig FK 实时刷新（对齐 AutoRunPage 模式，参数变化才重建 Kinematics）。冒烟注意：Start-Process 继承控制台被 shell 回收触发 CTRL_CLOSE 主动退出（0xC0000374 为伪影非崩溃），须用 `-WindowStyle Hidden` 独立窗口验证。

### 真机联调阶段 2 已完成（2026-08-25，除 Z 待硬件）

- **通过项**：2.1 J1 点动撞 +8 自动停/状态点橙/底部提示/反向放行；2.2 J1 Go 逻辑 5°→机械 107°→回读 5°（H02）；2.3 越界拒绝（H08）；2.5 J2 Go 逻辑 5°→机械 33°→回读 5°；2.6 R offset=0 与旧版一致；2.7 夹爪点动方向正确。**Home Offset 逻辑坐标全链路真机验证完成**。
- **遗留三项**：① Z 轴待硬件调试后测（阶段 3）；② J2 点动顿挫依然存在（用户决定暂不处理，疑点动节流阈值/匀速模式与负载匹配，专项排查时从 `kServoJogSendThreshold` 与 FSUS_PARAM_ACCEL_SWITCH 入手）；③ J2 位置显示小误差（可接受）。
- 实测记录见 `doc/test/real_machine_plan_phase2.md` 阶段 2 表格下方。

### 基线复位事件（2026-08-26 晚，务必知悉）

- 因对"切换其他模型后产生的未提交修改"信任存疑，执行 `git reset --hard` 到 **`b9c7ed3`**（"config: add Axis_Z home params"），**丢弃了全部未提交工作区修改**（hard reset 无法用 git 找回）。当前基线 = `b9c7ed3`。
- **b9c7ed3 已固化**：舵机协议/热重连/指数退避/Ping 门卫、连接状态分段着色、手动页 FK 接线、软限位、Home Offset、回零完成检测保护期（`homeStartedMs_`）、Z 回零 home 参数 + BoPaiCard post-start 检测（当前仅 WARN 不 reject）。
- **被丢弃的真机验证代码修复（2026-08-28 已全部重做，且当日真机复测全部通过 🟢）**：① 回零中点动误中断门禁 `jogInProgress_`（卡轴+舵机统一）；② 急停状态残留 `ResetAxisStates`（急停后轴1/3 误显示"限位"而非"未使能"）；③ post-start 竞态（一键回零后 J1/Z/夹爪/挤出全部没反应）；④ Z 回零 reject 分支 + UI"轴X 未启动"提示。**重做细节**：① `jogInProgress_` 门禁（MoveJog 置位 / StopJog 仅 `jogInProgress_ && jogAxis_==axis` 放行；复位点：StopJog 放行、JogTick 撞限、PollTick 撞界、HomeAxis、StopAxis、DisableAll、EmergencyStop、ReconnectServos、MoveAbs、CheckAxisBusy 超时）；② `ManualControlPage::ResetAxisStates()`（急停 lambda 调用）；③ BoPaiCard HomeAxis post-start 等待 running 置位（≤200ms/50ms 步进）+ reject 分支（MC_HomeStop+MC_Stop 收尾 HOME 状态机）；④ `OnGlobalHome` 未启动轴逐轴列出。根因链与原始修复方案记录在 `doc/worklog/2026-08-26.md`。清单见 TEST_RECORD.md 4 条 🟢。**舵机重连抖动治理（成功冷却 30s + 离线遥测降频）暂不专项验证，真机观察中**（根因已定性为舵机侧硬件状态类故障，见下）。
- **文档状态**：`AGENTS.md`/`TEST_RECORD.md`/`doc/config.md`/`doc/test/real_machine_plan_phase2.md` 均为 b9c7ed3 版本（本次重做后已局部更新）；`doc/worklog/2026-08-24/25/26.md` 为独立未跟踪文件，保留全部真机记录。**阶段 4（回零回归 + 停止/急停）四项重做修复已真机复测通过（2026-08-28 🟢）**；worklog 中阶段 4 曾全部真机验证通过，代码曾被 reset 回退后重做。
- config `Axis_Z.calibrationPending` **已置 false**（2026-08-28，阶段 3 已标定且真机验证通过；b9c7ed3 时曾遗留 true，随阶段 4 复测通过一并收尾；`homeRapidVel` 同日现场调 102.4→82.4，快速段 10→8.05mm/s）。
