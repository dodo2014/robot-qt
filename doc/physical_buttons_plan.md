# 物理蘑菇头按钮接入计划（初始化 / 启动 / 停止 / 复位 / 急停）

> 状态：**PLAN，待手册信息确认后实施**
> 硬件：BoPai 运动卡（网口 MultiCard）DI 输入；软件走既有 `IMotionCard::GetDI` + `HardwareManager::PollTick` 50ms 轮询架构。
> 确认人：待定（手册端子号/极性需现场核对）

## 一、总体接线方案

```
                        ┌──────────────────────────────────────┐
 [急停蘑菇头]──NC触点1──→│ BoPai 卡 EMG 专用急停输入端子          │ ← 卡固件自动停轴（软件死机也有效，
                        │                                      │    仅覆盖卡轴 J1/Z/夹爪）
             (第二组触点) │                                      │
             硬线串入 ──→│ 接触器线圈                            │ ← 双保险·必做：接触器主触点同时切断
                        │        ├─→ 步进驱动器动力电（J1/Z/夹爪）      ① 步进动力电 ② 舵机电源
                        │        └─→ XRServo 舵机电源（J2/R）          （见「二、舵机保护分析」）
                        │                                      │
 [启动 NO]─────────────→│ IN0                                  │
 [停止 NO]─────────────→│ IN1     DI 公共端 COM                │
 [复位 NO]─────────────→│ IN2      （接 +24V 或 GND，           │
 [初始化 NO]────────────→│ IN3      按卡的输入型式定）            │
                        └──────────────────────────────────────┘
```

### 按钮分配

| 按钮 | 触点类型 | 接入位置 | 软件动作（均为现成函数） |
|------|---------|---------|------------------------|
| **急停** | **常闭 NC（强制，断线即触发 fail-safe）** | 卡**专用急停输入 EMG 端子** | 卡固件自动停所有轴 + `MC_EStopGetSts` 锁存检测 → 软件 `EmergencyStop()` |
| 启动 | 常开 NO | IN0 | `RunSequence(当前选中方案)` |
| 停止 | 常开 NO | IN1 | `SequenceWorker::Stop()` + 停手动点动（见下方作用域表）|
| 复位 | 常开 NO | IN2 | `HomeAll()` 一键回零 |
| 初始化 | 常开 NO | IN3 | `Initialize()` |

### 设计决策（已确认）

1. **急停双保险（必做，非可选）**：① EMG 专用输入让卡固件在软件死机/崩溃时也能自动停所有**卡轴**（轴状态字 `AXIS_STATUS_IO_EMG_STOP = 0x100` 可查）；② 第二组常闭触点硬线驱动接触器，主触点**同时切断步进驱动器动力电与舵机电源两路负载**——这是软件死机时能停住舵机的唯一手段，原因见「二、舵机保护分析」。
2. **命令按钮全走 GPI 普通输入**：启动/停止/复位/初始化是软件命令，统一经 BoPai 卡 DI 读入后由软件分发到对应设备，**无需为舵机单独布按钮线**。
3. **本期只做输入，不做指示灯**（DO 反馈预留后续）。
4. 启动优先接 IN0~IN3 正好四路；若卡 GPI 数量不足或被占用再调整映射（config 可配，不必改代码）。

### 命令按钮的作用域

| 按钮 | 动作域 |
|------|-------|
| 启动 | `RunSequence(选中方案)`——SequenceWorker 内部含使能门禁 |
| 停止 | `worker_->Stop()` + `HardwareManager` 停手动点动（比屏幕停止按钮作用域更宽：手动页点动不受 SequenceWorker 管理，物理停止应一并兜住）|
| 复位 | `HomeAll()` 一键回零——继承「先使能」「回零门禁」既有拦截 |
| 初始化 | `Initialize()` |

### 布线工艺要求

- 所有 DI 线用双绞屏蔽线，与电机动力线分开走槽（现场 EMI 已有前科：舵机瞬时坏帧放大成重连）。
- 按钮壳体就近接地。
- 急停必须常闭 NC——只有一组触点的双联按钮优先保证 EMG 那一路。

## 二、舵机（J2/R）保护分层分析

**核心事实：FashionStar 总线舵机没有硬件安全输入引脚（仅电源 + 串口）**，EMG 急停只覆盖卡轴。舵机必须靠下面的分层防护兜底：

| 层级 | 机制 | 覆盖场景 | 状态 |
|------|------|---------|------|
| 软件层 | `HardwareManager::EmergencyStop()` → `servoJ2_/J3_->TorqueOff()`（阻尼松力，`HardwareManager.cpp:718`、`XRServo.cpp:388`）| 程序正常 + 按钮事件被 50ms PollTick 收到时 | ✅ 已存在并真机验证（手动页 P3 急停项）|
| 固件层 | **无能力**——总线协议无 EMG 输入概念 | — | 架构事实，不依赖 |
| 电源层 | 接触器常闭触点**切断舵机电源** | **软件死机/崩溃/串口失效时唯一有效手段** | 🔧 本次硬件接线必做 |

### 电源层为什么必要（残余风险分析）

- 舵机的 `SET_ANGLE(cmd 8)` 是**限时运动**（interval 到达后自动停在目标位锁力，点动 interval 钳制 50–30000ms）。软件死机时不会无限走，但正在执行的一次运动（最长可达 30s 点动或一次 Go 全程）会**继续走完当前目标**——扫到人/障碍物无法中断。
- 切电源后舵机失力——注意 **J2 承重大臂会垂落**（与断电重启后默认释放锁力是同一行为，用户已确认属正常）。现场评估：急停拍下前是否有臂上负载需支撑；必要时 J2 加抱闸或配重（不在本期范围，但要知情）。

## 三、⚠️ 待手册/同事确认清单（实施前置条件）

| # | 确认项 | 用途 | 确认结果 |
|---|-------|------|---------|
| 1 | 卡具体型号 | 定 GPI 总数（SDK 结构体显示 4~8 路不等）、端子定义图 | ☐ |
| 2 | **EMG 专用急停输入端子针脚号**及输入型式（NPN/PNP、干接点 or 24V） | 急停布线 | ☐ |
| 3 | EMG 输入的**有效极性**（低/高电平触发） | 对应 `MC_EStopSetIO(nCardIndex, nIOIndex, nEStopSns, lFilterTime)` 的 `nEStopSns` 参数 | ☐ |
| 4 | GPI 的 COM 公共端接 +24V 还是 GND（卡的输入型式 SINK/SOURCE） | 命令按钮布线 | ☐ |
| 5 | IN0~IN7 是否可自由使用（有无被 HMI/手轮等默认功能占用）；`MC_GetDi(MC_GPI=4)` 读到的 bit 与面板端子号的对应关系 | 软件 channel 映射 | ☐ |
| 6 | EMG 触发后是否锁存、解锁方式（`MC_EStopClrSts()` 即可清？还是需要重新使能流程） | 复位流程设计 | ☐ |
| 7 | 急停滤波时间建议值（`lFilterTime` 单位 ms），防机械抖动误触发 | 急停配置 | ☐ |
| 8 | 舵机供电电压/电流规格（电源铭牌或 FashionStar 型号参数） | 接触器选型（主触点须同时承受两路负载电流）| ☐ |
| 9 | 步进驱动器动力电的进线位置与使能回路形式（信捷 MP3-57H023 / XINJE DP3L1-224 接线图） | 确定接触器接入点，避免破坏驱动器供电要求 | ☐ |
| 10 | J2 大臂断电垂落的机械风险评估（负载、行程下方障碍） | 拍急停前的现场安全评估 | ☐ |

SDK 能力参考（`3rdparty/bopai/include/MultiCardCPP.h`）：
- `MC_GetDi(nDiType, pValue)` — 按 `MC_GPI=4` 类型读通用输入位图（现有 `BoPaiCard::GetDI(channel)` 即按位展开，`src/HAL/motioncard/BoPaiCard.cpp:505`）
- `MC_EStopSetIO(1042) / MC_EStopOnOff(1043) / MC_EStopGetSts(1044) / MC_EStopClrSts(1045) / MC_EStopConfig(1046)`
- `AXIS_STATUS_IO_EMG_STOP (0x100)` / `AXIS_STATUS_IO_SMS_STOP (0x80)` 轴状态位
- IO 中断回调可选：`MC_IntEnable(cardIdx, GAS_IOCallBackFun)`（回调在 DLL 线程，首期不用，轮询足够）

## 四、软件实施计划

### P1 — Config 配置项

**文件**：`config/config.json`、`doc/config.md`

```jsonc
"communication.motionCard.physicalButtons": [
    { "channel": 0, "action": "start",   "activeLevel": 1 },
    { "channel": 1, "action": "stop",    "activeLevel": 1 },
    { "channel": 2, "action": "homeAll", "activeLevel": 1 },
    { "channel": 3, "action": "init",    "activeLevel": 1 }
],
"communication.motionCard.estopInput": { "channel": /* 手册#2 */, "sns": /* 手册#3 */, "filterMs": /* 手册#7 */ }
```

- `channel` 可按实测对应关系（手册#5）改配置修正，无需改代码；`activeLevel` 兼容常开/常闭接入。
- HardwareManager 实时读（对齐 GetMaxSpeed/软限位模式），**改完即生效不重启**。

### P2 — BoPaiCard 急停固件级启用

**文件**：`src/HAL/motioncard/BoPaiCard.h/.cpp`、`src/HAL/interfaces/IMotionCard.h`

- Connect 成功后（useHardware）：`MC_EStopSetIO(cardIdx, ch, sns, filterMs)` + `MC_EStopOnOff(1)`。
- 新增虚函数 `ClearEStopLatch()`（默认空实现，Sim 不用管）：包一层 `MC_EStopClrSts()`；接口默认实现放 IMotionCard 免得污染 SimCard。

### P3 — HardwareManager 按钮轮询与分发

**文件**：`src/HAL/core/HardwareManager.h/.cpp`

- 新增成员：各通道消抖计数、稳定电平缓存。
- PollTick 内新增 `PollPhysicalButtons()`：
  - 逐通道 `motionCard_->GetDI(ch)` → 连续 2 tick 同电平判定有效（100ms 消抖）→ 与稳定态比较，上升沿发信号；
  - estop 是特殊通道：**不排队，就地直调 `EmergencyStop()`**——该函数已覆盖全设备（卡轴 halt + 两舵机 TorqueOff + 使能/回零/忙态标志复位，`HardwareManager.cpp:718-737`），对舵机无需新增任何调用；
  - EMG 触发后首个启动/复位类请求前先调 `ClearEStopLatch()` 解卡端锁存（具体行为依手册#6 实测定）。
- 新信号：`physicalButtonTriggered(QString action)`。
- 门禁逻辑不动：复位=HomeAll 天然被「未回零拒绝运动」「先使能再运动」既有门禁拦截，未连接硬件时按钮无动作。

### P4 — UI 接线

**文件**：`src/UI/AutoRunPage.h/.cpp`（或 MainWindow 统一转接）

- `physicalButtonTriggered` → 映射到 AutoRunPage 五个动作对应的现有私有函数/槽（与屏幕按钮同一路径，日志框同步打点「[物理按钮] xxx」便于现场区分触发来源）。
- 「停止」作用域比屏幕按钮更宽：`worker_->Stop()` 之外追加停止手动点动（`jogTimer_->stop()` + 卡轴 StopAll），兜住不在 SequenceWorker 管理内的手动页点动。

### P5 — Sim 回归

- SimCard `GetDI` 恒 false → 物理按钮路径天然旁路，仿真无回归；SimServo 的急停路径已由既有手动页 P3 自测覆盖（TorqueOff 断使能），如需仿真演示流程再加虚拟开关（非本期范围）。

## 五、验证计划

| 步骤 | 内容 | 通过标准 |
|------|------|---------|
| V1 | Debug+Release 编译 + Sim 冒烟 | 无回归，按钮路径静默旁路 |
| V2 | 万用表/短接线逐通道短接 IN0~IN3 | 日志出现 `[物理按钮]` 对应 action 且仅一次（消抖生效） |
| V3 | 手册端子接真实按钮逐个按压 | 屏幕侧动作与物理按钮一一对应 |
| V4 | 运行中拍急停（程序正常时） | 全设备立即停：卡轴 halt、**两舵机 TorqueOff 失力**，UI 状态同步；解锁流程符合手册#6 结论 |
| V5 | 软件死机状态拍急停（kill 进程后） | 动力电仍被切断 + 卡轴 EMG 自停；**舵机在运动中也会停（电源被切）**——对照残余风险：软件层救不了运动中的舵机，此步验证电源层兜底生效 |
| V6 | 万用表量接触器输出：拍急停前后两路负载（步进动力电 / 舵机电源）电压 | 两路均被切断；松开急停恢复 |
| V7 | TEST_RECORD.md 记账 | V2~V6 结果全部落账 |

## 六、风险与注意事项

- **bit 与端子对应关系未经证实前不要接死线**：先用万用表/短接线从 INn 逐一触发验证（V2），确认 `MC_GetDi` bit 位实际对应再正式接线。
- **J2 大臂断电垂落（机械影响，须现场评估）**：急停断电链会让失电的 J2 立即失去锁力——有负载/配重/行程下方有人手时先想清楚；本项依赖手册#10 结论。
- 急停滤波时间过小会误触发、过大反应变慢；以手册推荐值起步真机微调。
- `physicalButtons` 配置缺失时功能整体旁路并 SPDLOG_WARN 一次，不影响现有使用。
- 本计划为增量功能，不触碰已标 🟢 的换算/软限位/回零路径核心逻辑；P3 改动在 PollTick 内属新增分支，先行阅读 TEST_RECORD.md 回归推演后再动手。
