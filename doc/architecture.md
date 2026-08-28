# CreamPuffRobot 系统架构

> 2026-08-28 整理。权威以源码为准，本文是导航图。构建/编码规约见根目录 `AGENTS.md`，测试台账见 `TEST_RECORD.md`。

## 1. 目录结构

```
CreamPuffRobot/
├─ CMakeLists.txt          顶层：Qt6/Eigen3/OpenCV/spdlog + 5 子目录；HAL 以 WHOLEARCHIVE 整库链接（REGISTER_* 宏静态自注册生效的前提）
├─ build_release.bat       Release 一键构建+打包（%~dp0 可重定位）
├─ config/
│  ├─ config.json          运行时配置：通信/仿真类型/运动学/视觉/TCP/axes 六轴（换算、限位、回零参数）
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
   │  ├─ camera/           SimCamera(测试图案)、CameraCaptureWorker(采集 QThread)、FrameSaver(存储 QThread)、
   │  │                    FrameConverter(RGB/深度渲染+叠加)、CameraManager(相机生命周期)、SimVision(目标规格)
   │  └─ algorithm/        SimAlgo(颜色匹配检测→PuffResult)
   ├─ Core/                运动学核心（纯算法，不依赖 Qt/Config）
   │  ├─ Kinematics        2D SCARA：Pose{x,y,z,r}/Joints{j1,j2,z,r}；Forward/Inverse/InverseSmart(就近选解)/
   │  │                    SetTCP(TCP 内化为等效小臂 l2_eff=L2+tcpForward)；甜甜圈工作空间校验
   │  ├─ CoordTransform    Eigen 4×4 手眼矩阵：PixelToRobot/CameraToRobot
   │  └─ Trajectory        轨迹（预留）
   ├─ Logic/               流程编排
   │  ├─ SequenceWorker    方案执行引擎（moveToThread 独立线程；HardwareManager 调用经 BlockingQueuedConnection
   │  │                    回主线程；Move=InverseSmart→逐轴 MoveAbs→WaitForAxes；单步/停止/急停/使能门禁）
   │  └─ PickCycleController  视觉抓取单周期模板（状态机；供 Vision 动作委托复用）
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
  → 信号上行：actionStarted/Finished/schemeFinished/interrupted → UI 日志/按钮状态
```

### 3.3 状态上行（轮询广播）
```
PollTick(50ms) ─┬─ CheckAxisBusy：忙超时兜底 → axisMoveFinished（Go 按钮恢复）
                ├─ PollCardAxis：脉冲→逻辑坐标 → stateUpdated（状态灯/FK 面板）
                │    ├ 软限位方向拦截（仅"仍朝越界方向"停，反向放行）
                │    ├ 回零完成检测（homeStartedMs_ 1s 保护期）
                │    └ 报警/限位边沿 + 异常签名日志（DescribeAxisStatus）
                └─ PollServoTelemetry：离线降频遥测(1s)/在线 250ms → servoStateUpdated
                     └ 热重连：Ping 门卫 → 失败指数退避 2s→30s / 成功冷却 30s（防抖动循环）
```

视觉链路：CameraCaptureWorker(采集线程) `frameReady` → UI 渲染（FrameConverter 叠加）+ FrameSaver(存储线程) 落盘；SimAlgo 检测 → CoordTransform::CameraToRobot → InverseSmart。

## 4. HardwareManager 门面设计原则（不可再拆的核心）

1. **单点门禁**：使能/报警/回零/软限位门禁只在 `MoveAbs/MoveJog/HomeAxis/HomeAll` 入口拦一道，UI 与自动流程共用——拆散门禁会破坏"先使能再运动"安全模型
2. **Home Offset 只在门面**：`机械 = 逻辑 + offset`（下发）与 `逻辑 = 机械 - offset`（回读）只存在于 HardwareManager 一处，其余全部逻辑坐标
3. **已拆出的部分**（2026-08-25 重组）：CameraManager（相机生命周期）、AxisConfigService（参数查询）——对外 API 保留转发
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
