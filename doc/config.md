# CreamPuffRobot 配置文件说明

## 文件位置

`config/config.json` — 机器人控制系统的全部配置参数，系统启动时自动加载，用户修改后自动保存。

## 顶层结构

| 键 | 类型 | 对应界面 Tab |
|---|---|---|
| `communication` | object | 通信与连接 |
| `simulation` | object | **无界面入口**（只能手改本文件，见下） |
| `kinematics` | object | 运动学参数 |
| `vision` | object | 视觉与工艺参数 |
| `tcpCalibration` | object | TCP与标定 |
| `production` | object | 自动运行页（循环模式 / 次数 / 回安全位控件） |
| `axes` | object | 电控与映射 |
| `ui` | object | 视觉与工艺参数（`autoRunStreamPreview` 开关） |
| `version` | string | 配置版本号（当前 `"1.0.0"`） |
| `$schema` | string | 描述字段（非配置项） |

顶层还包含 `"$schema": "CreamPuffRobot configuration schema"`（纯描述，加载时忽略）。

---

## simulation — 硬件类型选择

`HardwareManager` 启动时按此配置经 `HALFactory` 创建对应硬件实现，创建失败/未注册时回落仿真。

| 字段 | 可选值 | 说明 |
|---|---|---|
| `enabled` | bool | ⚠ **幻影键**——全库无任何读点（见下方核实），改它**不产生任何效果** |
| `motionCardType` | `SimCard` / `Bopai` / `ZMotion` | 运动控制卡实现名 |
| `servoType` | `SimServo` / `XRServo` | 串口舵机实现名 |
| `cameraType` | `SimCamera` / `OrbbecCamera` | 相机实现名 |
| `algorithmType` | `SimAlgo` | 视觉算法实现名（当前仅 `SimAlgo` 一个实现） |

当前已注册实现（`REGISTER_*` 宏，`HALFactory.h`）：运动卡 `SimCard` / `Bopai`；舵机 `SimServo` / `XRServo`；相机 `SimCamera` / `OrbbecCamera`；算法 `SimAlgo`。`ZMotion` / `Leisai` 为预留品牌（SDK 目录已建）。

**当前配置**（`config/config.json`）：`motionCardType=SimCard`、`servoType=SimServo`、`cameraType=OrbbecCamera`、`algorithmType=SimAlgo`、`enabled=true`——即**执行机构走仿真（`SimCard` + `SimServo`）、相机走真机（`OrbbecCamera`）**。
> ⚠ 该组合下跑 Sim 冒烟会**去抢真实相机**（`simulation.cameraType` 决定 `HardwareManager` 实例化谁，与 `enabled` 无关）。冒烟前须把 `cameraType` 改回 `SimCamera`，否则可能与用户真机调试互抢。

> ⚠ **本段无界面写入口**（2026-09-18 核实，2026-09-20 P1 复核，2026-10-13 补）：全库对 `simulation.*` 的读点只有 `HardwareManager.cpp:105,113,153,168,236`（四类硬件选型 + 重连取卡型）与 `VisionTestPage.cpp:239`（相机型号下拉框回填），**没有任何 `ConfigManager::set()` 调用**。`ConfigPage.cpp` 实际只有 5 个标签页（通信与连接 / 运动学参数 / 视觉与工艺参数 / TCP与标定 / 电控与映射），**不存在「硬件类型选择 (仿真/真机)」标签页**。要切换仿真/真机，只能手改 `config/config.json`。
> ⚠ **`simulation.enabled` 是幻影键**（2026-10-13 核实）：全库 `grep '"simulation'` 只命中 `motionCardType` / `servoType` / `algorithmType` / `cameraType` 四个键，**`enabled` 无任何读点** ⇒ 它既不是「总开关」，也不影响任何行为。切换仿真/真机靠的是上面四个 `*Type` 键本身（`SimCard`/`SimServo`/`SimCamera` ↔ `Bopai`/`XRServo`/`OrbbecCamera`）。
> ~~另注：`CameraManager.cpp:43` 读取的 `simulation.cameraDeviceId` 在本文件中不存在，永远走硬编码默认值 `"CAM-SIM-001"`。~~ **✅ 已于 P1（C2，2026-09-20）修复**：该读取已删除，相机设备标识收敛为单一键 `communication.camera.sn`（`CameraManager.cpp:50` 读；`ConfigPage.cpp` 与视觉检测页写）。详见 `doc/config_ownership_plan.md` §3.4 / C2 / C4。

---

## communication — 通信与连接

```
communication
├── motionCard
│   ├── ip       "192.168.0.1"     运动控制卡网口 IP
│   ├── pcIp     "192.168.0.100"   本机(PC) 网卡 IP（BoPai 网口连接需两端 IP，须与卡同网段）
│   └── port     "60000"           端口 (string)
├── servo
│   ├── port     "COM3"            舵机串口号
│   └── baudRate "115200"          波特率
├── servos[0..1]                   舵机 ID 与限位（HardwareManager 喂给 XRServo）
│   ├── name     "J2"/"R"          逻辑名称
│   ├── id       0/1               串口总线舵机 ID（真机实测 J2→id0、R→id1）
│   ├── minAngle 0.0               最小角度 (°)
│   ├── maxAngle 180.0/120.0       最大角度 (°)
│   └── speed    50.0              运行速度 (°/s)
└── camera                       相机通信与流参数（P3 / 2026-09-20 补齐，见 doc/camera_integration_plan.md §六）
    ├── sn             "CP9KB530005S"  相机序列号（设备选择唯一键；真机 SN）
    ├── colorWidth     1920      彩色流宽（当前 1920×1080@15）
    ├── colorHeight    1080      彩色流高
    ├── colorFps       15.0      彩色流帧率（实测严格生效——配 15 则实测 15.0）
    ├── colorFormat    "MJPG"    彩色格式首选值（"MJPG"/"YUYV"/"RGB"；空串=用实现内置尝试队列）
    ├── depthWidth     848       深度流宽（常用档 848×480）
    ├── depthHeight    480       深度流高
    ├── depthFps       15.0      深度流帧率
    ├── depthFormat    "Y16"     深度格式
    ├── d2cAlign       "SW"      D2C 对齐："SW"（软对齐，高分辨率必须）/ "HW"（仅 640×480+848×480 档）/ "OFF"
    └── autoExposure   true      自动曝光（本版固定 true，曝光控件已移除——决策 4）
```

> ⚠ **`d2cAlign` 必须保持 `"SW"`**：`OrbbecCamera::intrinsics_` 是**彩色**内参，且接口上**没有深度内参通路**；`"OFF"` 时深度回落到原生 848×480 而 `CameraFrame.width/height` 仍是彩色尺寸（`0xC0000005` 越界根因，TR-116），且深度 XY 无内参可用。

> `motionCard.port`、`servo.baudRate`、`camera.sn` 均为字符串，配置界面直接以文本读写。

> **键归属（P1 C4 / P3 S0 统一）**：相机**选型**在 `simulation.cameraType`（与 `motionCardType` / `servoType` 同族）；
> 相机**通信与流参数**全部归 `communication.camera.*`。幻影键 `simulation.cameraDeviceId` 与
> `vision.frameWidth` / `frameHeight` / `frameFps` 的读取**已删除**（这些键在 `config.json` 中本就不存在）。
> 读取点集中在 `CameraManager::BuildStreamConfig()`（`CameraManager.cpp`）——**相机实现自己不读 config**，
> 沿用「HAL 设备实现不主动读配置」的既有边界（与 `BoPaiCard` / `XRServo` 一致）。

---

## kinematics — 运动学参数

```
kinematics
├── links
│   ├── l1       138.83           L1 (大臂水平投影 mm，J1 轴心到 J2 轴心的水平距离, 2026-08 重测：174.35 → 138.83)
│   ├── l2       166.86           L2 (小臂长度 mm, 2026-08 实测)
│   ├── z0       470.0            Z0 基准高度 (mm), 轴1不锈钢法兰的上表面距地面 470mm（2026-08-26 阶段 3 标定）
│   └── h1       175.0            大臂垂直高度落差 (mm，向下倾斜固定值；腕点真实高度 = Z电机高度 + z0 − h1) , 测算方法：轴1不锈钢法兰的上表面距离地面470mm(目前机器人放置在地面上)， 夹爪垂直朝下，夹爪的长度130mm, 夹爪末端距离地面165mm, 470 - 130 - 165 = 175（2026-08-26 阶段 3 已标定）
└── axisParams[0..2]               每个轴的参数
    ├── name     "J1"/"J2"/"Z"     轴名称
    ├── pulsesPerUnit  0.012       脉冲当量 (脉冲/度 或 脉冲/mm)
    ├── limitMin       -180        软限位最小值
    └── limitMax       180         软限位最大值
```

`axisParams` 当前值：

| name | pulsesPerUnit | limitMin | limitMax |
|---|---|---|---|
| J1 | 0.012 | -180 | 180 |
| J2 | 0.008 | -90 | 90 |
| Z  | 0.025 | 0 | 200 |

### safePos — 全局安全位（2026-09-07 新增）

```
kinematics
└── safePos
    ├── j1       0.0              安全位 J1 关节角 (°)
    ├── j2       0.0              安全位 J2 关节角 (°)
    ├── z        0.0              安全位 Z 电机坐标 (mm，回零后=0、向下为负，与示教 joints.j3 同坐标系)
    ├── r        179.5            安全位 R 关节角 (°)
    └── enabled  true             安全位总开关（同时控制回零后自动回 + 手动「回安全位」按钮；**代码默认 false**——出厂全 0 不应乱走；TR-082）
```

> **当前值**（`config/config.json`，2026-10-13 核对）：`enabled=true`、`j1=0.0`、`j2=0.0`、`z=0.0`、`r=179.5`——即**安全位已启用**。

- **用途**：一键回零完成后（含急停恢复链路的重新回零）自动走到安全位——**先原地抬 Z 再水平移动**，避免从低位直奔方案第一步横扫桌面。执行通道为 `SequenceWorker::RunSafePos()`（构造 2 点临时方案复用 `RunSequence`，详见 `doc/archive/global_safe_position.md`）。
- **enabled=true 时**：每次 `homeStateChanged(true)`（全轴回零完成）上升沿自动执行；运行期间 worker=Running，自动运行页启动被互斥（正常）。
- **UI 写入入口**：① 设备配置页·运动学参数区 4 个输入框 + 「启用安全位」checkbox（TR-082 改名，原名「回零后自动回安全位」——该开关是**总开关**，手动「回安全位」按钮同样受其控制，非仅自动）；② 手动控制页「示教安全位」按钮（读当前四轴关节角写入并置 enabled=true，需已回零且轴静止）；③ 手动控制页「回安全位」按钮手动触发走位——**enabled=false 时手动与自动均不执行**（`RunSafePos` 统一门禁，TR-082）。
- **注意**：急停后仅重新使能**不会**触发（已回零轴 homed 保持）——必须执行一键回零才走安全位（急停后舵机 TorqueOff 可能垂落，旧坐标不可信，属预期行为）。

---

## vision — 视觉与工艺参数

```
vision
├── confidenceThreshold  0.85      识别置信度阈值
├── depthZMin            6.0       深度过滤 Z 最小值 (mm)
├── depthZMax            496.0     深度过滤 Z 最大值 (mm)
└── templatePath         ""        算法模板配置路径（可选；P4 新增）
```

> **写入口与生效状态（2026-09-20，P1 核实 + P4 更新）**：
> - `confidenceThreshold` — **P1 起真正生效**：`SimAlgo::Detect()` 按该值过滤掉 `confidence` 低于阈值的识别结果（此前只显示、不过滤）。
> - `depthZMin` / `depthZMax` — 由 `SimAlgo::Detect` **每次检测实时读**并做深度筛选：深度有效但落在 `[zMin, zMax]` 之外的目标**丢弃**（不产生结果，语义与 `confidenceThreshold` 一致）；深度缺失（无深度图或深度值 ≤ 0）时仍报告 2D 位置、`z = 0` → 生效。**（TR-103 修复**：此前超范围只是把 `z` 置 0 而结果照常入表，结果表会出现 `x=y=z=0` 的伪目标）
> - `templatePath` — **P4 新增**，写入口＝视觉检测页「模板配置」的「浏览…」（`VisionTestPage::OnBrowseTemplate`）。**⚠ 目前无读点**：`IPuffAlgorithm` 只有 `Detect/LocateBest/LoadConfig`，**没有模板参数**，故本键目前只做「入口 + 持久化」，由将来的算法实现消费（需求原文即写「可选填；留空则用算法内置默认值」）。**不要误以为它已在工作**。
> - **P4 起这三个检测参数的 UI 写入口已从视觉页移除**（方案 `vision_test_page_redesign_plan.md` 的 V5 裁定：贴合「算法参数不上面」）→ **调参一律手改本文件**。⚠ **手改本文件后必须重启程序**——三个键在检测时都是「实时读」（`confidenceThreshold` 经 `EffectiveThreshold()`、`depthZMin/depthZMax` 经 `Detect()`），但读的是 `ConfigManager` 的**内存** `root_`，而 `root_` 只在启动时由 `load()` 填充（`ConfigManager.cpp:49`）→ 不重启不会刷新。设备配置页的「视觉与工艺参数」标签页仍保留写入口，**经该页改则即时生效**（走 `set()` 更新内存 `root_`）。
> - **`exposure` 键已随 P4 删除**（决策 4：本版固定自动曝光）。它此前唯一的写入口是视觉页的「曝光」输入框，该控件移除后**全库无任何读点**，属「看起来在工作、实际不工作」的死键。相机曝光由 SDK 自动曝光控制（`communication.camera.autoExposure`，见 `camera_integration_plan.md` §六）。注意与**动作级 `ActionData.exposure`**（挤出动作的参数，`doc/process.md`）区分——那是另一个东西。
> - 上述键**均无全局参数与动作级参数的覆盖关系实现**，动作级覆盖规则的落地属 P5。

---

## ui — 界面偏好

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `autoRunStreamPreview` | bool | `false` | **自动运行页预览源**（S6 决策 6 的「视频流预览」保留能力，TR-113）。`false` = 显示「**检测结果叠加图**」（左格该次检测的原图 / 右格带检测框），由 `SequenceWorker::detectionReady` 驱动、**不订阅原始帧流** → 与视觉检测页解耦，视觉页停流不会让本页定格。`true` = 显示「**原始实时视频流**」，本页**可见期间**以持有者名 `"AutoRunPreview"` 占用相机（消费 ⟺ 持有），隐藏即释放 |
| `inspectionPanel.show` | bool | `true` | **是否在自动运行页创建「检测结果」页签**（`doc/inspection_result_panel_plan.md`，⏳ 计划中）。`false` = **不建**该页签，自动运行页布局与改动前**完全一致**（不留页签栏）。⚠ 即使本键为 `true`，若 `config/inspection.json` 缺失或解析失败（无规格）**同样不建**——两层信号 |

> ⚠ **`inspectionPanel.show` 当前不存在于 `config.json`，且全库无任何读点**（2026-10-13 核实：`src/` 内 `grep inspectionPanel` 零命中；`config/` 下也无 `inspection.json`）——它只是 `inspection_result_panel_plan.md` 的**计划键**，页签功能本身尚未实施。**手改本文件加这个键不会有任何效果**，不要把它当成已生效的开关。

> **写入口与生效状态（TR-113，2026-09-23）**：
> - 写入口 = 设备配置页「**视觉与工艺参数**」页签的勾选框「自动运行页预览源用「实时视频流」」（`ConfigPage::CreateTab3Vision`）→ 经 `set()` 更新内存 `root_` 并 `scheduleSave()` 落盘，同时 emit `autoRunPreviewSourceChanged`；`MainWindow` 已把它接到 `AutoRunPage::SetStreamPreviewEnabled` → **切换立即生效、无需重启**。
> - **手改本文件则需重启**（同 `vision.*` 的成因：`getValue` 读内存 `root_`，`root_` 只在启动时由 `load()` 填充）。启动时 `MainWindow` 按本键应用一次。
> - 该开关只改**自动运行页的画面来源**，不改相机流的启停逻辑（流仍由持有者集合驱动，流运行 ⟺ 集合非空）。两条预览路径的持有者名**刻意不同**：方案运行期 = `"AutoRun"`，预览路径 = `"AutoRunPreview"`。
> - 与 `vision.*` 三个键的区别：那三个是**算法参数**（影响检测结果本身），本键只影响**画面怎么显示**，不进检测链路。
>
> **检测结果页签的生效状态（2026-10-10 成文，2026-10-13 核实更新）**：
> - 该键**尚未存在于 `config.json`**（`inspection_result_panel_plan.md` 未实施）⇒ 既无 UI 写入口，**手改本文件加键也无效**（无读点）。上表列出的 `true` 是**计划默认值**，不是当前生效值。
> - ⚠ **与 `autoRunStreamPreview` 的关键差异**：后者只改**画面来源**、可即时切换；本键改的是**页签的增删**，属**结构级**变更。运行时 `addTab` / `removeTab` 还要额外维护当前索引、布局重算与补绘状态机 ⇒ **本期不做**（只在构造期判定）。
> - **两层信号**：本键为 `true` **且** `config/inspection.json` 存在且解析成功，才创建该页签。规格文件缺失 ⇒ 回落空规格 ⇒ 不建页签（避免出现「什么都测不了却显示合格」的面板）。
> - ⚠ `config/inspection.json` 是**独立文件**，不走 `ConfigManager` 的内存 `root_`，其生效时机由 `inspection_result_panel_plan.md` §4.2 单独定义——**不要**套用本节的「经 UI 改即时生效」规则。

---

## tcpCalibration — TCP与标定

```
tcpCalibration
├── toolOffsetX   53             工具偏移 X (mm)
├── toolOffsetY   0               工具偏移 Y (mm)
├── toolOffsetZ   -130             工具偏移 Z (mm)
└── handEyeMatrix [16 numbers]     手眼标定 4×4 矩阵 (行主序)
```

当前 `handEyeMatrix`：`[1,0,0,180, 0,1,0,-5.7, 0,0,1,38.1, 0,0,0,1]`。

> ⚠ 平移分量 `(180, -5.7, 38.1)` 是**占位/历史值，不是标定结果**（`hand_eye_calibration_plan.md` §5.1 已核实；TR-006 / TR-014 记录的另一组 `(12.3, -5.7, 38.1)` 同为历史值）。`handEyeMatrix` **无 UI 入口**（`src/UI/` 全仓无 `handEye`）⇒ 只能手改本文件 + **重启**。
> `toolOffsetX/Y/Z` 另有 UI 入口（设备配置页「TCP与标定」）。

---

## production — 循环生产（自动运行页）

`SequenceWorker::RunSequence` 的循环参数。写入口＝自动运行页的「循环模式 / 次数 / 回安全位」控件（读 `AutoRunPage.cpp:892-894`、写 `:912-914`）。

```
production
└── loop
    ├── mode              0       循环模式：0=单轮(Single) / 1=指定次数(Count) / 2=无限循环(Infinite)
    ├── count             10      指定次数模式的轮数（读时 qBound 到 1~9999；**仅 mode=1 生效**）
    ├── returnToSafePos   true    每轮结束、进入下一轮前先回安全位（防跨轮横扫）
    ├── retryCount        —       节点级重试次数（**当前文件中没有此键**，读点默认 0 = 不重试）
    └── retryDelayMs      —       重试前等待 (ms)（**当前文件中没有此键**，读点默认 500）
```

> **当前值**（`config/config.json`，2026-10-13 核对）：`mode=0`（单轮）、`count=10`、`returnToSafePos=true`。
> ⚠ `mode=0` 时 `count` **不参与**——本次运行就是**跑 1 轮**，`count=10` 只是上一次「指定次数」留下的残值。

- `mode` / `count` / `returnToSafePos` 三个键**已在本文件中**，经自动运行页控件修改后走 `set()` → 300 ms 去抖落盘，**下一次点「启动」即按新值执行**（无需重启）。
- `retryCount` / `retryDelayMs` 是 `LoopConfig` 的**可选键**（`AutoRunPage.cpp:929-930` 读；结构体默认见 `SequenceWorker.h:85-86`），当前文件里**没有** ⇒ 走默认值 `0` / `500`，行为＝**不重试**。要启用节点级重试须手改本文件 + **重启**。
- `returnToSafePos=true` 且 `mode != 0`（确实是循环）时，`kinematics.safePos.enabled` 必须为 `true`：UI 侧启动门禁会拦（`AutoRunPage.cpp:619-624`，提示「循环运行需先示教安全位…或取消勾选『回安全位』」），`SequenceWorker` 内另有兜底（`SequenceWorker.cpp:259-263`）。**单轮（`mode=0`）不触发该门禁**——不发生跨轮回安全位。
- `mode=2`（无限循环）启动时弹**二次确认**（`AutoRunPage.cpp:626-639`）。

---

## axes — 电控与映射

`axes` 是一个 JSON 对象，每个 key 是**不可变的逻辑身份名**（如 `Axis_J1`、`Axis_J2`、`Axis_Z`、`Axis_R`、`Axis_Gripper`、`Axis_Extruder`），**不**与物理插卡端口绑定，保证软件-硬件解耦。物理属性（`hardwareType`、`portId`）只是对象内的可变字段，切换端口时仅修改属性，key 保持不变。

相同 `hardwareType` 的物理端口 ID 不可重复，UI 会弹窗提示。

每个轴对象包含以下字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | string | 轴名称（独立命名） |
| `hardwareType` | int | 0=运动控制卡, 1=串口总线舵机 |
| `portId` | int | 物理端口 ID |
| `enabled` | bool | 硬件启用标志（默认 `true`）。`false`=未接硬件（如只接运动卡未接电机）→ 初始化绑定为 None：**不参与使能/回零互锁/运动判定**（`axisHomed_` 恒 true、`IsGlobalEnabled`/`HomeAll` 跳过）。接入电机后改回 `true` 即恢复参与（2026-08-31 新增，回零互锁配套；当前 `Axis_Extruder=false`） |
| `axisType` | string | `"rotation"`(角度°)/`"linear"`(直线 mm)，仅卡轴（舵机轴无此字段）。换算用 360° 还是 导程×减速比 |
| `direction` | int | 0=正向 Normal, 1=反向 Inverted |
| `maxSpeed` | double | 最大速度，单位按轴类型：旋转轴 `°/s`、直线轴 `mm/s`（卡轴经 `AccelToPulse`/`SpeedToPulse` 换算下发） |
| `maxAccel` | double | 最大加速度，单位按轴类型：旋转轴 `°/s²`、直线轴 `mm/s²`。**卡端 `TTrapPrm.acc` 为 Pulse/ms²**，BoPaiCard 内部经 `AccelToPulse = maxAccel×PulsePerUnit/1e6` 换算 |
| `stopDecSmooth` | double | **MC_Stop 平滑档停止减速度系数**（TR-090，手册 V2.9 取值 `0.1~2`、卡默认 `0.5`；无量纲，不做脉冲换算）。作用于所有 `MC_Stop` 触发的停止：自动 ⏸ 暂停 / ■ 停止 / 点动松键 / 回零清理——**越大停距越短**。`0`（默认）=不下发、保持卡默认；`>0` 时 BoPaiCard 在 `MoveAbs/MoveJog` 前按值去重下发。**减速过陡开环步进可能丢步，从 1.0 起真机标定**；急停突停档（decAbruptStop）不暴露，读卡端现值保留。舵机轴无此概念（CMD24 立即停） |
| `jogSpeed` | double | 点动 (JOG) 速度，单位按轴类型：旋转轴 `°/s`、直线轴 `mm/s` |
| `calibrationPending` | bool | 换算参数待真机标定（仅部分卡轴） |
| `limitMin` | double | 软限位最小值。**已强制执行**：`MoveAbs/Go` 目标越界拒绝下发；点动到达边界自动停止；点动启动方向已在边界则拒绝。由 `HardwareManager` 实时读取本配置（`GetLimitMin`/`IsWithinSoftLimits`），在「电控与映射」中修改立即生效。**拦截按运动方向区分**：仅停止"仍朝越界方向运动"的轴（惯性冲过边界后反向离开/Go 回界内均放行，见 `AGENTS.md`「拦截必须区分运动方向」） |
| `limitMax` | double | 软限位最大值。同 `limitMin`，单位与轴一致（旋转轴 °，直线轴 mm）。配置错误（`limitMin >= limitMax`）时视为不限制并打印警告 |
| `homeOffset` | double | 原点偏移 |
| `homeDir` | int | 回零搜索方向：1=正方向, 0=反方向（仅卡轴） |
| `homeSns` | int | HOME 信号极性：`-1`=不修改(沿用卡默认)；`0`=低有效、`1`=高有效（调 `MC_HomeSns` 的**全局位掩码**，只改本轴那一位——`BoPaiCard.cpp:249-257`）。真机标定时用 ±1 对比确定搜索方向 |
| `homeRapidVel` | double | 回零**快速段**速度，单位 **Pulse/ms**（SDK 单位，搜索段） |
| `homeLocatVel` | double | 回零**定位段**速度，单位 **Pulse/ms**（碰 HOME 信号后精定位段） |
| `homeBackDis` | int | 碰信号后的反向退出脉冲数（`ulHomeBackDis`），用于精确定位：找到信号→反向退出→重逼近。0=不退出，直接停在信号沿 |
| `homeMaxDis` | int | 最大搜索距离 Pulse（`ulHomeMaxDis`）。**须设非零值**：部分 BoPai 固件将 0 解释为"搜索 0 距离"而非"不限"，导致回零立即完成。J1 推设为全行程脉冲数的 2~3 倍（如 1,500,000≈211°×7111） |
| `sortOrder` | int | 界面显示排序序号，升序排列 |

### 传动与换算参数 (transmission)

每个轴的 `transmission` 子对象包含两种模式的全部参数，界面根据 `hardwareType` 切换显示：

**控制卡模式** (`hardwareType=0`):
- `encoderResolution` — 编码器分辨率 (Pulse/Rev)
- `microSteps` — 细分数
- `gearRatio` — 减速比
- `lead` — 导程

**舵机模式** (`hardwareType=1`):
- `minPulse` — 最小控制值
- `maxPulse` — 最大控制值
- `minAngle` — 最小物理角度
- `maxAngle` — 最大物理角度

未显式配置时程序使用的默认 `transmission` 值——**以实际运行路径（`HardwareManager::LoadAxisConfigsFromConfig`）为准**：

| 键 | 缺键时的兜底值 | 代码位置 |
|---|---|---|
| `encoderResolution` | `32000` | `HardwareManager.cpp:446` |
| `microSteps` | `1` | `HardwareManager.cpp:447` |
| `lead` | `360.0` | `HardwareManager.cpp:448` |
| `gearRatio` | `1.0` | `HardwareManager.cpp:449` |
| `minPulse` / `maxPulse` / `minAngle` / `maxAngle` | 仅配置页显示用（`500` / `2500` / `0` / `180`） | `ConfigPage.cpp:1080-1083` |

> ⚠ **舵机模式四个键（`minPulse`/`maxPulse`/`minAngle`/`maxAngle`）当前无消费点**：全库对 `transmission.*` 的**读**只有 `HardwareManager.cpp:446-449` 那四个卡轴键（+ 配置页 UI 的读写）。舵机限位实际取自 `communication.servos[].minAngle/maxAngle`，不是这里。
> ⚠ `AxisConfig` 结构体自身还带一组成员初值（`IMotionCard.h:47-50`：`pulsesPerRev=131072`、`microSteps=512`、`lead=20.0`、`gearRatio=1.0`）——它**只在不经 config 直接默认构造时**才生效；`LoadAxisConfigsFromConfig` 会逐轴覆盖，故**运行期永远用上表的兜底值**。~~旧版本此处记的 `131072 / 512 / 50 / 20.0` 是把结构体初值当成了运行期默认，已更正（`gearRatio=50` 更是两组值都对不上）。~~

**当前各轴实际参数**（`config/config.json`，2026-10-13 核对）：

| Key | hardwareType | portId | direction | axisType | jogSpeed | maxSpeed | maxAccel | transmission |
|---|---|---|---|---|---|---|---|---|
| `Axis_J1` | 0 卡 | 0 | 1 反向 | rotation | 25.0 | 25.0 | 80.0 | 25600 / 1 / 0.01 / lead360（驱动器 25600 脉冲/圈 + 谐波减速 1:100） |
| `Axis_J2` | 1 舵机 | 0 | 0 | – | 15.0 | 15.0 | 232.0 | minPulse500 / maxPulse2500 / minAngle0 / maxAngle180 |
| `Axis_Z` | 0 卡 | 1 | 0 | linear | 21.0 | 21.0 | 100.0 | 25600 / 1 / 0.5 / lead5（皮带20:40 + 丝杆导程5mm，10240 脉冲/mm；2026-08-26 阶段 3 已真机标定，`calibrationPending:false`） |
| `Axis_R` | 1 舵机 | 1 | 0 | – | 55.0 | 55.0 | 240.0 | minPulse500 / maxPulse2500 / minAngle0 / maxAngle180 |
| `Axis_Gripper` | 0 卡 | 3 | 0 | linear | 1.0 | 2.0 | 50.0 | 40000 / 1 / 1 / lead2（驱动器 XINJE DP3L1-224 拨码 SW5-SW8 全 OFF=40000 Pulse/rev，已标定；lead 暂定 2mm） |
| `Axis_Extruder` | 0 卡 | 2 | 0 | rotation | 2.0 | 20.0 | 100.0 | 32000 / 1 / 1 / lead10（**`enabled=false`：只接运动卡未接电机，暂不参与使能/回零/运动**） |

> transmission 列格式（卡轴）：`encoderResolution / microSteps / gearRatio / lead`。
> ⚠ **J1 的 `maxAccel=80` / `maxSpeed=25` 是 TR-095（J1 关节累积偏移）的收口值**，非默认值——调大前先读 `doc/joint_drift_analysis.md`。
> ⚠ **`Axis_J1` / `Axis_Z` 另有 `stopDecSmooth=1.0`**（TR-090 真机标定值），`Axis_Gripper` / `Axis_Z` 的 `calibrationPending=false`。

### 回零速度换算（Pulse/ms）

`homeRapidVel` / `homeLocatVel` 单位是 **Pulse/ms**（BoPai SDK 中 `dHomeRapidVel`/`dHomeLocatVel` 的单位），与界面速度框（°/s 或 mm/s）不同，需要换算：

```
每度脉冲 PulsesPerDeg = (encoderResolution × microSteps) / (gearRatio × 360)    // 旋转轴
每毫米脉冲 PulsesPerMm = (encoderResolution × microSteps) / (gearRatio × lead)  // 直线轴

homeRapidVel (Pulse/ms) = 目标快速段速度 (°/s) × PulsesPerDeg / 1000
homeLocatVel (Pulse/ms) = 目标定位段速度 (°/s) × PulsesPerDeg / 1000
```

**J1 实例**（encoderResolution=25600, microSteps=1, gearRatio=0.01, 旋转轴）：
```
PulsesPerDeg = (25600 × 1) / (0.01 × 360) = 7111.11 脉冲/度

当前值反推目标速度：
homeRapidVel = 41.3 Pulse/ms → 41.3 × 1000 / 7111.11 ≈ 5.8 °/s
homeLocatVel =  7.1 Pulse/ms →  7.1 × 1000 / 7111.11 ≈ 1.0 °/s
```

**回零全部参数说明**：`homeDir`（搜索方向）、`homeSns`（信号极性，-1=不改/0=低有效/1=高有效）、`homeRapidVel`/`homeLocatVel`（速度）、`homeBackDis`（碰信号后反向退出精定位，0=不退出）、`homeMaxDis`（最大搜索距离，**须非零**）。J1 当前 `homeBackDis=0`（无反向退出），定位段仅在碰信号瞬间生效。

### 默认轴列表（当前 config.json 实际值）

| Key | 名称 | 硬件类型 | sortOrder | limitMin | limitMax | jogSpeed | 回零配置 |
|---|---|---|---|---|---|---|---|
| `Axis_J1` | [1] 轴1 (大臂 J1) | 运动控制卡 | 0 | -102 | 8 | 25.0 | homeDir=1, homeSns=0, homeRapidVel=41.3, homeLocatVel=7.1, homeBackDis=0, homeMaxDis=1,500,000 |
| `Axis_J2` | [2] 轴2 (小臂 J2) | 串口总线舵机 | 1 | -90 | 152 | 15.0 | – |
| `Axis_Z` | [3] 轴3 (Z轴) | 运动控制卡 | 2 | -165 | 0 | 21.0 | homeDir=1, homeSns=-1, homeRapidVel=82.4, homeLocatVel=20.48, homeBackDis=5120, homeMaxDis=2,500,000 |
| `Axis_R` | [4] 轴4 (翻转 R) | 串口总线舵机 | 3 | 0 | 185 | 55.0 | – |
| `Axis_Gripper` | [5] 轴5 (夹爪) | 运动控制卡 | 4 | -5 | 0 | 1.0 | homeDir=1, homeSns=0, homeRapidVel=20.0, homeLocatVel=10.0, homeBackDis=0, homeMaxDis=600,000 |
| `Axis_Extruder` | [6] 轴6 (挤出) | 运动控制卡 | 5 | 0 | 100 | 2.0 | –（`enabled=false` 未接电机，不参与回零） |

> `Axis_J1`/`Axis_Z`/`Axis_Gripper` 三个卡轴已配置回零参数（`homeDir/homeSns/homeRapidVel/homeLocatVel/homeBackDis/homeMaxDis`）；`Axis_Extruder` 未配置，使用代码默认值（`homeDir=1`、`homeSns=-1`、`homeRapidVel=5.0`、`homeLocatVel=1.0`、`homeBackDis=0`、`homeMaxDis=0`），且当前 `enabled=false`（未接电机）**不参与回零互锁**。**所有卡轴 `homeMaxDis` 均应设非零值**（见字段说明）。

---

## 加载流程

1. ConfigPage 构造时调用 `ConfigManager::load()`
2. 查找顺序: 源码目录 `config/config.json` → 运行目录 `config.json`
3. 各 Tab 创建时通过 `dVal()`/`sVal()`/`iVal()` 读取当前值填入控件
4. 用户修改后通过信号 (editingFinished / valueChanged / currentIndexChanged) 触发 `ConfigManager::set()` → 300ms 去抖后自动写回文件
