# CreamPuffRobot 系统架构

> 2026-08-28 整理。权威以源码为准，本文是导航图。构建/编码规约见根目录 `AGENTS.md`，测试台账见 `TEST_RECORD.md`。

## 1. 目录结构

```
CreamPuffRobot/
├─ CMakeLists.txt          顶层：Qt6/Eigen3/OpenCV/spdlog + 5 子目录；HAL 以 WHOLEARCHIVE 整库链接（REGISTER_* 宏静态自注册生效的前提）
├─ build_release.bat       Release 一键构建+打包（%~dp0 可重定位）
├─ config/
│  ├─ config.json          运行时配置：通信/仿真类型/运动学(含 safePos 全局安全位)/视觉/TCP/axes 六轴（换算、限位、回零参数）
│  └─ process.json         工艺方案：方案→动作(Move/Vision/Extrude/Delay/Gripper)→点位(coord 嵌套)
├─ doc/                    设计文档、测试计划（real_machine_plan_phase2 等）、worklog/（每日日志）
├─ 3rdparty/bopai/         博派 SDK（include/lib/bin；POST_BUILD 自动复制 DLL 到输出目录）
├─ out/build/x64-Debug     用户实际运行的 Debug 输出（真机验证用）
├─ out/build/x64-Release   Release 输出
└─ src/
   ├─ Config/              配置与工艺数据层
   │  ├─ ConfigManager     config.json 单例；点路径读写；300ms 去抖写回；防御性编程（root_ 恒为 object 等）
   │  └─ ProcessManager    方案/动作/点位内存模型 + process.json 序列化；缺省测试方案自动生成
   ├─ HAL/                 硬件抽象层（六子目录；品牌实现以 REGISTER_* 宏自注册，新增品牌不改中心代码）
   │  ├─ interfaces/       纯虚接口：IMotionCard(脉冲单位契约) / IAxisServo / IEndEffector / ICamera / IPuffAlgorithm
   │  ├─ core/
   │  │  ├─ HardwareManager   组装层+门面（单例 QObject）：唯一硬件入口，物理单位(mm/度)对外
   │  │  ├─ AxisConverter     物理↔脉冲双向换算（rotation 含 gearRatio；参数由 HardwareManager 注入）
   │  │  ├─ AxisMap           逻辑轴枚举 J1..Extruder ↔ 硬件绑定（卡轴 index / 舵机总线 id），config 驱动
   │  │  ├─ AxisConfigService 每轴速度/加速度/单位/软限位查询（实时读 config）
   │  │  └─ HALFactory        运行时字符串注册工厂
   │  ├─ motioncard/       BoPaiCard(真机网口,MC_*)、SimCard(仿真,脉冲域软限位)
   │  ├─ servo/            XRServo(FashionStar 0x4C12/0x1C05 协议,共享串口句柄+引用计数)、SimServo
   │  ├─ camera/           SimCamera(测试图案)、OrbbecCamera(真机 Orbbec Gemini 336，USE_ORBBEC 开关，默认按 SDK 存在性自动探测)、
   │  │                    CameraCaptureWorker(采集 QThread)、FrameSaver(存储 QThread)、
   │  │                    FrameConverter(RGB/深度渲染+叠加)、CameraManager(相机生命周期 + communication.camera.* 读取点)、
   │  │                    SimVision(目标规格)
   │  │                    — 相机流启停走 HardwareManager 的**持有者集合**（AcquireCamera/ReleaseCamera，
   │  │                      「流运行 ⟺ 集合非空」），页面不直接 Start/StopStream
   │  └─ algorithm/        SimAlgo(颜色匹配检测→PuffResult)
   ├─ Core/                运动学核心（纯算法，不依赖 Qt/Config）
   │  ├─ Kinematics        2D SCARA：Pose{x,y,z,r}/Joints{j1,j2,z,r}；Forward/Inverse/InverseSmart(就近选解)/
   │  │                    SetTCP(TCP 内化为等效小臂 l2_eff=L2+tcpForward)；甜甜圈工作空间校验
   │  ├─ CoordTransform    Eigen 4×4 手眼矩阵：PixelToRobot/CameraToRobot
   │  └─ Trajectory        轨迹（预留）
   ├─ Logic/               流程编排
   │  └─ SequenceWorker    方案执行引擎（moveToThread 独立线程；HardwareManager 调用经 BlockingQueuedConnection
   │  │                    回主线程；Move=示教 joints 优先/InverseSmart→逐轴 MoveAbs→RunScheduler 轮询 IsAxisBusy；
   │  │                    2026-09-07 重构：显式状态机 WorkerState{Idle,Running,Paused,Fault}+
   │  │                    PauseReason{None,User,Step}；点级暂停 PauseGate（减速停→挂起→原地续走）；
   │  │                    StopImmediate(减速停保持使能)/Fault 锁存+ClearFault/RunSafePos(全局安全位 2 点临时方案)；
   │  │                    单步/停止/急停/使能/回零门禁）
   │  │                    —— `src/Logic/` 下唯一实现。原 `PickCycleController`（视觉抓取单周期模板）
   │  │                    为死代码且其 `ExecuteOneCycle` 跳过 `CameraToRobot` 把相机系坐标当基座坐标用，
   │  │                    已于 P3（2026-09-20）删除，见 `doc/camera_integration_plan.md` S0 第 4 条
   └─ UI/                  MainWindow + 5 页面（AutoRun/ManualControl/Process/VisionTest/Config）+ ToggleSwitch
                           + KinematicsHelper（UI 层统一 FromConfig/ReadConfigParams，Core 不依赖 Config）
```

## 2. 分层依赖（单向，禁止反向）

```
UI  →  Logic  →  Core  →  HAL
                          ↑
              HAL(core) → Config（仅 HardwareManager 读配置喂底层；
                          底层卡/舵机代码禁止 #include ConfigManager）
```

- **include 约定**：HAL 内部互 include 用平铺文件名（靠 CMake include dir 追加子目录解析）；外部引用 HAL 用 `HAL/<子目录>/Xxx.h`
- **线程边界**：UI 主线程（PollTick 50ms 定时器、串口事务）、采集线程（CameraCaptureWorker）、存储线程（FrameSaver）、SequenceWorker 线程。跨线程只经信号槽（值类型需 qRegisterMetaType）

## 3. 三条核心数据流

### 3.1 手动控制（下发）
```
ManualControlPage（点动/Go/回零/使能按钮）
  → HardwareManager 门面：使能门禁(axisEnabled_) → 报警门禁(lastAlarm_) → 回零门禁(homingActive_)
    → 软限位校验(逻辑坐标) → Home Offset 转换(机械=逻辑+offset, inverted 再取反)
    → AxisConverter 换算 → BoPaiCard(脉冲) / XRServo(机械角+周期)
```

### 3.2 自动流程（SequenceWorker 线程模型）
```
AutoRunPage.RunSequence → SequenceWorker(worker 线程排队) → StartExecution(worker 线程)
  → 每个动作经 InMainThread(QMetaObject::invokeMethod BlockingQueuedConnection) 回主线程
    调 HardwareManager（与 PollTick 串行，避免数据竞争）→ 同 3.1 链路
  → 信号上行：actionStarted/Finished/schemeFinished/interrupted/
    stateChanged(WorkerState{Idle,Running,Paused,Fault}, PauseReason{None,User,Step}) → UI 日志/按钮状态
  → 循环生产（TR-091，2026-09-14）：RunSequence(scheme, LoopConfig) → 会话内循环外壳
    ExecuteActions（按 LoopConfig 重复 ExecuteOnce；每轮 emit cycleChanged(i,total)）
    → 循环间隔 ReturnToSafePosInline()（内联回安全位，与手动 RunSafePos 共用 BuildSafePosActionFrom）
    → schemeFinished 仍只在**全部循环完成**时发一次（否则机器还在动、UI 已显示「✅ 完成」）
  → 动作树与分组并发（P6，2026-09-20）：ExecuteOnce 遍历 scheme.nodes（一级节点）
    → ExecuteNode 按 nodeType 分派：ExecuteAction（动作）或 ExecuteGroup（分组）
    → ExecuteGroup 先 validateGroupChildren 兜底（资源互斥 / 数据依赖 / 不嵌套），
      再为每个子节点建一个 RunnerState，交 RunScheduler 单线程协作式调度
    → RunScheduler（20 ms tick，与改造前 WaitForAxes 同节拍）：**同时下发、统一汇合**；
      组内暂停 = 整组减速停 + 汇合后统一挂起 + 恢复统一重发当前段；
      停止 / 内部失败 = stopAndWaitAll 先安全停下全部子动作再返回（§Q2，绝不提前返回）
    → 失败归因 = 首个失败者（其余仅记日志）；内部中止用独立 aborting 标记，
      与 cancel（用户停止 / 急停）分流——置 cancel 会把动作失败误报成「用户停止」
    → 单动作走同一条链（ExecuteAction 内部即 1 元素 runners），故暂停 / 停止 / 续走只有一份语义
  → 暂停：Pause() 置标志 → 挂起点(PauseGate/RunScheduler 到位轮询 ≤20ms) StopAxes 减速停 → 挂起
    → Resume() 重发当前绝对目标 MoveAbs 原地续走（Delay 按剩余时间续算）
  → 停止：StopImmediate() = cancel + StopAllAxes(减速停保持使能)；急停 = EmergencyStop(断使能)
```

### 3.2.1 全局安全位（2026-09-07 新增）
```
homeStateChanged(true)（全轴回零完成，PollTick 主线程）
  → MainWindow 上升沿判定(lastHomed_) + safePos.enabled 检查 + worker Idle 守卫
  → SequenceWorker::RunSafePos()：读 config kinematics.safePos(关节角) → 构造 2 点临时方案
    （点1=原地抬Z、点2=水平走安全位）→ RunSequence → 同 3.2 链路（先抬Z再水平，防横扫）
  → 手动入口：ManualControlPage「回安全位」按钮；「示教安全位」读当前关节角写 config 并置 enabled
  → 急停恢复链路：急停→切手动→使能→一键回零→（急停锁解除，不自动回安全位）→手动回安全位→切自动启动方案（TR-081 修正：急停不清已回零轴 homed，回零完成无上升沿）
```

### 3.3 状态上行（轮询广播）
```
PollTick(50ms) ─┬─ CheckAxisBusy：忙超时兜底 → axisMoveFinished（Go 按钮恢复）
                ├─ PollCardAxis：脉冲→逻辑坐标 → stateUpdated（状态灯/FK 面板）
                │    ├ 软限位方向拦截（仅"仍朝越界方向"停，反向放行）
                │    ├ 回零完成检测（homeStartedMs_ 1s 保护期）
                │    └ 报警/限位边沿 + 异常签名日志（DescribeAxisStatus）
                ├─ PollServoTelemetry：离线降频遥测(1s)/在线 250ms → servoStateUpdated
                │    └ 热重连：Ping 门卫 → 失败指数退避 2s→30s / 成功冷却 30s（防抖动循环）
                └─ PollCamera：降频 500ms（50ms tick × 10）→ 链路状态**边沿检测** → cameraStateChanged
                     └ 只建基线/只广播，**恢复动作一律不在这里**（L3 重建 Context 要 sleep 6s，
                       放 PollTick 里会卡死 UI 线程）；实际分级恢复在 OrbbecCamera 的取帧失败路径内
```

视觉链路：CameraCaptureWorker(采集线程) `frameReady` → UI 渲染（FrameConverter 叠加）+ FrameSaver(存储线程) 落盘；SimAlgo 检测 → CoordTransform::CameraToRobot → InverseSmart。**相机流的启停由持有者集合驱动**（`HardwareManager::AcquireCamera/ReleaseCamera`，持有者 `{"VisionPage", "AutoRun", "AutoRunPreview"}`）——页面与自动流程只声明「我要用相机」，不自己启停流。⚠ `"AutoRun"` = 自动运行页**方案运行期**持有（启动 acquire / Idle·Fault 释放）；`"AutoRunPreview"` = 同页的**视频流预览**路径持有（S6 决策 6 的保留能力，由 `SetStreamPreviewEnabled(true)` 启用，默认关闭，见 `AutoRunPage.h`）；两者生命周期不同，刻意分开。⚠ 自动运行页**默认不消费 `frameReady`**——它改订阅 `SequenceWorker::detectionReady`（S6 决策 6，TR-112）。

## 4. HardwareManager 门面设计原则（不可再拆的核心）

1. **单点门禁**：使能/报警/回零/软限位门禁只在 `MoveAbs/MoveJog/HomeAxis/HomeAll` 入口拦一道，UI 与自动流程共用——拆散门禁会破坏"先使能再运动"安全模型
2. **Home Offset 只在门面**：`机械 = 逻辑 + offset`（下发）与 `逻辑 = 机械 - offset`（回读）只存在于 HardwareManager 一处，其余全部逻辑坐标
3. **已拆出的部分**（2026-08-25 重组）：CameraManager（相机生命周期）、AxisConfigService（参数查询）——对外 API 保留转发。**P3（2026-09-20）追加**：相机流**持有者集合**（`AcquireCamera`/`ReleaseCamera` + `std::set<std::string>`）与相机链路状态边沿广播（`PollCamera` → `cameraStateChanged`）都在 `HardwareManager`，`OrbbecCamera` 只提供快照与内部分级恢复，**不碰 Qt 对象**
4. **P1 已拆**（2026-08-28）：PollTick → `PollCardAxis()` + `PollServoTelemetry()` 两个 private 函数（纯重排零行为变化）
5. **待拆路线**（阶段 5 真机验证通过后执行）：
   - P2：`ServoSupervisor`（HAL/servo/）——遥测分频 + 热重连退避（servoPollCounter_/servoNextReconnectMs_/servoReconnectBackoffMs_/servoPingSkipCount_ + ReconnectServos），调参不再动主文件
   - P3：`JogController`——舵机点动状态机（JogTick 时间累积模型 + 8 个 jog* 成员）
   - **不拆**：运动/使能/回零/软限位门禁与 Home Offset（见原则 1/2）

## 5. 关键不变量（改代码前先对照 TEST_RECORD.md）

- IMotionCard 契约：接口层一律脉冲；物理↔脉冲换算只在 AxisConverter 与 BoPaiCard::PulsePerUnit（两处公式必须一致，含 gearRatio）
- 软限位拦截语义：仅拦"正在点动且朝越界方向"；静止停在边界、反向离开、回零中的轴均放行
- 回零：MC_HomeStart 到卡端 running 有启动间隙 → 完成判定必须有 1s 保护期；reject 必须 MC_HomeStop+MC_Stop 收尾 HOME 状态机
- StopJog 门禁：仅 `jogInProgress_ && jogAxis_==axis` 放行（回零中松点动键不得打断回零）
- 舵机重连：两实例共享句柄必须一起断开；重连后使能复位（门禁要求人工重新使能）
- **观察者指针（TR-084）**：跨线程 / 长生命周期 QObject（如 `SequenceWorker`）的持有方一律用 `QPointer`，**禁用裸指针长期持有**——所有权方（`deleteLater` / 父对象）负责销毁，观察者只持弱引用。原因：观察者无法感知对象失效，一旦"对象先于观察者析构"即悬垂访问（实例：退出时 `ShutdownWorker` 的 `processEvents` 派发已排队信号，命中已被 `deleteLater` 的 worker → `0xc0000005`）。当前 4 处：`MainWindow::sequenceWorker_`、`AutoRunPage::m_worker`、`ManualControlPage::worker_`、`ProcessPage::m_worker`。
- **停止曲线（TR-085/TR-090）**：全工程所有业务停止——自动 ⏸ 暂停 / ■ 停止 / 点动松键 / 点动停止按钮——卡轴共用 `MC_Stop(mask,0)` 平滑减速档（`BoPaiCard::StopAxis:334` 与 `StopJog:462` 同指令同实参）、舵机共用 CMD24 停止+锁力——**禁止断脉冲**（开环丢步红线，仅 EmergencyStop 允许断使能）。滑行距离 = 停止瞬间速度 × 卡端停止减速。**TR-090 已接线 `MC_SetStopDec`**：config 每轴 `stopDecSmooth`（无量纲系数，手册 0.1~2、卡默认 0.5；0=不下发），`MoveAbs/MoveJog` 前实时读 config 经 `BoPaiCard::SetStopDec` 去重下发（decAbrupt 档读卡端现值保留不暴露）；**标定从 1.0 起逐步调大，过陡开环丢步**。点动"快停"诉求走**降点动速度**；不再有"运行时改档并发"问题（系数是持久设置非运行时切换）。
- **`errorOccurred` 单点化（TR-091 / D17 方案 A）**：`SequenceWorker::ExecuteAction` 及其子树（`ExecuteNode` / `ExecuteGroup` / `RunScheduler` / `StepRunner` / `PrepareRunnerSegment`）**一律不 emit `errorOccurred`**，失败原因写入 `Impl::lastError`；emit 只允许出现在**会话级出口 2 处**——`ExecuteOnce` 失败分支与 `StartSingleExecution` 兜底（均 `lastError.isEmpty() ? action.name : lastError`）。原因：动作级重试（`production.loop.retryCount > 0`）期间若子树照旧 emit，UI 会在重试还没开始就跳「⛔ 错误」；顺带修掉"具体失败原因被 `action.name` 覆盖"的既有瑕疵。**新增动作类型时勿在子函数里 emit**。**P6 补充分组语义**：分组并发下同样不新增 emit——失败原因仍只写 `Impl::lastError`，上报规则为**首个失败者**；调度器另用独立的 `aborting` 标记区分「内部失败中止」与 `cancel`（用户停止 / 急停），避免把动作失败误报成「用户停止」。单线程路线下**不需要加锁**。
- **分组并发的不变量（P6，TR-102）**：① **组内冲突按资源、不按类型**——`resourceKeysOf(ActionType)` 给出资源集合，校验用**两两求交**（`Move` 与 `VisionGuide` **共用四轴**集合；`Delay` 为**空集**，多个延时同组合法）；② **数据依赖单独一类**——同组内不允许「`Vision` + 引用该 `Vision` 编号的 `VisionGuide`」（并发时移动目标尚未算出），它与资源抢占是两回事，须分开拦；③ **分组不能嵌套**（`nodeType` 独立于 `ActionType`，`Group` 不进枚举）；④ **失败归因 = 首个失败者**，其余子动作的失败仅记日志，`Impl::lastError` 只承载首个原因；⑤ **停止 / 内部失败必须先安全停下全部子动作再返回**（`stopAndWaitAll`，最长等 3000 ms 后 WARN `group stop: axes still busy after 3000 ms`）——提前返回会让硬件仍在运动时退出执行循环；⑥ **单步以整组为一步**——组内**一律不调用** `WaitForStep`（否则多个子状态机争抢同一个 `stepGo`，放行顺序不确定）。
- **循环生产的会话边界（TR-091）**：循环外壳在**会话内**（`ExecuteActions` 是壳、`ExecuteOnce` 是原单轮主体），不是 `StartExecution` 外层或 `RunSequence` 层——这样 cancel / PauseGate / Fault / `schemeFinished` 全部复用既有单点。两条硬性约定：① `schemeFinished` **只在全部循环完成时发一次**（每轮发 `cycleChanged`），否则机器还在动、UI 已显示「✅ 完成」，操作员会据此伸手取料；② worker 线程**绝不读 ConfigManager**（json 读写非线程安全），`LoopConfig` 与安全位数值（`safeJ1/J2/Z/R`）都在主线程组装/快照后随 `RunSequence` 进 worker。循环间隔回安全位用**内联** `ReturnToSafePosInline()`（三不：不置 `safeSession`、不发 `actionStarted/actionFinished`、不重入门禁），与手动 `RunSafePos` 共用 `BuildSafePosActionFrom` 构造与执行链。
- **「严格先抬 Z 到位再做水平移动」（TR-091 / D19）**：回安全位与循环间隔共用同一条链——`PrepareRunnerSegment` 逐点推进、到位由 `RunScheduler` 轮询 `IsAxisBusy` 判定（P6 前为 `ExecuteMove` 逐点串行 + `MoveToPoint` 在 `WaitForAxes` 阻塞）；点1 = `{J1/J2/R 保持当前值, Z=safeZ}`、点2 = 安全位，故水平位移只可能发生在 Z=safeZ 高度。**加固 = `DispatchPointMove()` 对「目标与读回当前值之差 ≤ 0.01」的轴跳过下发**（点1 实际只剩 Z 轴 → "抬 Z 阶段零水平运动"由代码保证，不再依赖"舵机读回值 = 指令值"的假设）；跳过前仍过 `IsWithinSoftLimits`，**不旁路软限位硬拦截**。注意 `IsAxisBusy` 判的是 `MarkAxisBusy` 写的**预计运动时长 + 200ms**，不是位置反馈（开环无编码器）——严格表述是「Z 预计到位 + 200ms 余量后才下发水平移动」。
- **相机流持有者集合（TR-099）**：`ICamera` 的启停**只能经 `HardwareManager::AcquireCamera(owner)` / `ReleaseCamera(owner)`**，禁止任何地方直接调 `StartCameraStream` / `StopCameraStream`（否则一处关闭采集就会打断另一处正在用的流）。三条约定：① 持有者是**具名集合**（`std::set<std::string>`），同一持有者重复 acquire 幂等、不叠加；② **只有集合由空变非空时才真启流，由非空变空时才真停流**；③ **自动运行期间视觉页的相机开关 / 启停采集按钮必须置灰**（`VisionTestPage::SetParamsLocked`），否则 release 掉 `AutoRun` 持有者会打断正在执行的识别动作——这是对 TR-075 的修正；检测/截图/视图切换等只读操作保留可用。
