# CreamPuffRobot 配置文件说明

## 文件位置

`config/config.json` — 机器人控制系统的全部配置参数，系统启动时自动加载，用户修改后自动保存。

## 顶层结构

| 键 | 类型 | 对应界面 Tab |
|---|---|---|
| `communication` | object | 通信与连接 |
| `simulation` | object | 硬件类型选择 (仿真/真机) |
| `kinematics` | object | 运动学参数 |
| `vision` | object | 视觉与工艺参数 |
| `tcpCalibration` | object | TCP与标定 |
| `axes` | object | 电控与映射 |
| `version` | string | 配置版本号（当前 `"1.0.0"`） |
| `$schema` | string | 描述字段（非配置项） |

顶层还包含 `"$schema": "CreamPuffRobot configuration schema"`（纯描述，加载时忽略）。

---

## simulation — 硬件类型选择

`HardwareManager` 启动时按此配置经 `HALFactory` 创建对应硬件实现，创建失败/未注册时回落仿真。

| 字段 | 可选值 | 说明 |
|---|---|---|
| `enabled` | bool | 总开关 |
| `motionCardType` | `SimCard` / `Bopai` / `ZMotion` | 运动控制卡实现名 |
| `servoType` | `SimServo` / `XRServo` | 串口舵机实现名 |
| `cameraType` | `SimCamera` / ... | 相机实现名（预留） |
| `algorithmType` | `SimAlgo` / ... | 视觉算法实现名（预留） |

当前已注册实现：`SimCard`（运动卡仿真）、`SimServo`（舵机仿真）、`Bopai`（博派运动卡）、`XRServo`（XR 串口舵机）。`ZMotion`/`Leisai` 为预留品牌（SDK 目录已建）。

**当前配置**（`config/config.json`）：`motionCardType=Bopai`、`servoType=XRServo`、`cameraType=SimCamera`、`algorithmType=SimAlgo`、`enabled=true`（真机模式）。

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
└── camera
    └── sn       "336L"            相机序列号
```

> `motionCard.port`、`servo.baudRate`、`camera.sn` 均为字符串，配置界面直接以文本读写。

---

## kinematics — 运动学参数

```
kinematics
├── links
│   ├── l1       138.83           L1 (大臂水平投影 mm，2026-08 重测：174.35 → 138.83)
│   ├── l2       216.0             L2 (小臂长度 mm)
│   ├── z0       45.0              Z0 基准高度 (mm)
│   └── h1       0.0              大臂垂直高度落差 (mm，向下倾斜固定值；腕点真实高度 = Z电机高度 + z0 − h1)
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

---

## vision — 视觉与工艺参数

```
vision
├── confidenceThreshold  0.85      识别置信度阈值
├── depthZMin            6.0       深度过滤 Z 最小值 (mm)
└── depthZMax            496.0     深度过滤 Z 最大值 (mm)
```

---

## tcpCalibration — TCP与标定

```
tcpCalibration
├── toolOffsetX   12.5             工具偏移 X (mm)
├── toolOffsetY   -3.2             工具偏移 Y (mm)
├── toolOffsetZ   45.0             工具偏移 Z (mm)
└── handEyeMatrix [16 numbers]     手眼标定 4×4 矩阵 (行主序)
```

当前 `handEyeMatrix`：`[1,0,0,12.3, 0,1,0,-5.7, 0,0,1,38.1, 0,0,0,1]`。

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
| `axisType` | string | `"rotation"`(角度°)/`"linear"`(直线 mm)，仅卡轴（舵机轴无此字段）。换算用 360° 还是 导程×减速比 |
| `direction` | int | 0=正向 Normal, 1=反向 Inverted |
| `maxSpeed` | double | 最大速度，单位按轴类型：旋转轴 `°/s`、直线轴 `mm/s`（卡轴经 `AccelToPulse`/`SpeedToPulse` 换算下发） |
| `maxAccel` | double | 最大加速度，单位按轴类型：旋转轴 `°/s²`、直线轴 `mm/s²`。**卡端 `TTrapPrm.acc` 为 Pulse/ms²**，BoPaiCard 内部经 `AccelToPulse = maxAccel×PulsePerUnit/1e6` 换算 |
| `jogSpeed` | double | 点动 (JOG) 速度，单位按轴类型：旋转轴 `°/s`、直线轴 `mm/s` |
| `calibrationPending` | bool | 换算参数待真机标定（仅部分卡轴） |
| `limitMin` | double | 软限位最小值。**已强制执行**：`MoveAbs/Go` 目标越界拒绝下发；点动到达边界自动停止；点动启动方向已在边界则拒绝。由 `HardwareManager` 实时读取本配置（`GetLimitMin`/`IsWithinSoftLimits`），在「电控与映射」中修改立即生效。**拦截按运动方向区分**：仅停止"仍朝越界方向运动"的轴（惯性冲过边界后反向离开/Go 回界内均放行，见 `AGENTS.md`「拦截必须区分运动方向」） |
| `limitMax` | double | 软限位最大值。同 `limitMin`，单位与轴一致（旋转轴 °，直线轴 mm）。配置错误（`limitMin >= limitMax`）时视为不限制并打印警告 |
| `homeOffset` | double | 原点偏移 |
| `homeDir` | int | 回零搜索方向：1=正方向, 0=反方向（仅卡轴） |
| `homeSns` | int | HOME 信号极性：`-1`=不修改(沿用卡默认)；`0`/`1`=调用 `MC_HomeSns` 设置该轴 HOME 高有效（触发电机反向搜索）。真机标定时用 ±1 对比确定搜索方向 |
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

未显式配置时程序使用的默认 `transmission` 值：`encoderResolution=131072`、`microSteps=512`、`gearRatio=50`、`lead=20.0`、`minPulse=500`、`maxPulse=2500`、`minAngle=0`、`maxAngle=180`。

**当前各轴实际参数**（`config/config.json`）：

| Key | hardwareType | portId | direction | axisType | jogSpeed | maxSpeed | maxAccel | transmission |
|---|---|---|---|---|---|---|---|---|
| `Axis_J1` | 0 卡 | 0 | 1 反向 | rotation | 20.0 | 50.0 | 360.0 | 25600 / 1 / 0.01 / lead360（驱动器 25600 脉冲/圈 + 谐波减速 1:100） |
| `Axis_J2` | 1 舵机 | 0 | 0 | – | 15.0 | 120.0 | 232.0 | minPulse500 / maxPulse2500 / minAngle0 / maxAngle180 |
| `Axis_Z` | 0 卡 | 1 | 0 | linear | 5.0 | 500.0 | 100.0 | 32000 / 1 / 0.5 / lead5（皮带20:40 + 丝杆导程5mm；`calibrationPending`） |
| `Axis_R` | 1 舵机 | 1 | 0 | – | 30.0 | 180.0 | 240.0 | minPulse500 / maxPulse2500 / minAngle0 / maxAngle180 |
| `Axis_Gripper` | 0 卡 | 3 | 0 | linear | 1.0 | 10.0 | 50.0 | 40000 / 1 / 1 / lead2（驱动器 XINJE DP3L1-224 拨码 SW5-SW8 全 OFF=40000 Pulse/rev，已标定；lead 暂定 2mm） |
| `Axis_Extruder` | 0 卡 | 2 | 0 | rotation | 1.0 | 20.0 | 100.0 | 32000 / 1 / 1 / lead10 |

> transmission 列格式（卡轴）：`encoderResolution / microSteps / gearRatio / lead`。

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
homeRapidVel(3°/s) = 3 × 7111.11 / 1000 ≈ 21.3 Pulse/ms
homeLocatVel(1°/s) = 1 × 7111.11 / 1000 ≈ 7.1  Pulse/ms
```

**回零全部参数说明**：`homeDir`（搜索方向）、`homeSns`（信号极性，-1=不改/0=低有效/1=高有效）、`homeRapidVel`/`homeLocatVel`（速度）、`homeBackDis`（碰信号后反向退出精定位，0=不退出）、`homeMaxDis`（最大搜索距离，**须非零**）。J1 当前 `homeBackDis=0`（无反向退出），定位段仅在碰信号瞬间生效。

### 默认轴列表（当前 config.json 实际值）

| Key | 名称 | 硬件类型 | sortOrder | limitMin | limitMax | jogSpeed | 回零配置 |
|---|---|---|---|---|---|---|---|
| `Axis_J1` | [1] 轴1 (大臂 J1) | 运动控制卡 | 0 | -180 | 180 | 20.0 | homeDir=0, homeSns=0, homeRapidVel=21.3, homeLocatVel=7.1, homeBackDis=0, homeMaxDis=1,500,000 |
| `Axis_J2` | [2] 轴2 (小臂 J2) | 串口总线舵机 | 1 | 0 | 180 | 15.0 | – |
| `Axis_Z` | [3] 轴3 (Z轴) | 运动控制卡 | 2 | 0 | 200 | 5.0 | – |
| `Axis_R` | [4] 轴4 (翻转 R) | 串口总线舵机 | 3 | 0 | 180 | 30.0 | – |
| `Axis_Gripper` | [5] 轴5 (夹爪) | 运动控制卡 | 4 | -5 | 20 | 1.0 | – |
| `Axis_Extruder` | [6] 轴6 (挤出) | 运动控制卡 | 5 | 0 | 100 | 1.0 | – |

> 仅 `Axis_J1` 配置了回零参数（`homeDir/homeSns/homeRapidVel/homeLocatVel/homeBackDis/homeMaxDis`）；其余卡轴使用代码默认值（`homeDir=1`、`homeSns=-1`、`homeRapidVel=5.0`、`homeLocatVel=1.0`、`homeBackDis=0`、`homeMaxDis=0`）。**所有卡轴 `homeMaxDis` 均应设非零值**（见字段说明）。

---

## 加载流程

1. ConfigPage 构造时调用 `ConfigManager::load()`
2. 查找顺序: 源码目录 `config/config.json` → 运行目录 `config.json`
3. 各 Tab 创建时通过 `dVal()`/`sVal()`/`iVal()` 读取当前值填入控件
4. 用户修改后通过信号 (editingFinished / valueChanged / currentIndexChanged) 触发 `ConfigManager::set()` → 300ms 去抖后自动写回文件
