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

---

## communication — 通信与连接

```
communication
├── motionCard
│   ├── ip       "192.168.1.100"   网口 IP
│   └── port     "60000"           端口 (string)
├── servo
│   ├── port     "COM3"            舵机串口号
│   └── baudRate "115200"          波特率
├── servos[0..1]                   舵机 ID 与限位（HardwareManager 喂给 XRServo）
│   ├── name     "J2"/"R"          逻辑名称
│   ├── id       1/2               串口总线舵机 ID
│   ├── minAngle 0.0               最小角度 (°)
│   ├── maxAngle 180.0             最大角度 (°)
│   └── speed    50.0              运行速度 (°/s)
└── camera
    └── sn       "336L"            奥比中光相机序列号
```

> `motionCard.port`、`servo.baudRate`、`camera.sn` 均为字符串，配置界面直接以文本读写。

---

## kinematics — 运动学参数

```
kinematics
├── links
│   ├── l1       286.0             L1 (大臂长度 mm)
│   ├── l2       216.0             L2 (小臂长度 mm)
│   └── z0       45.0              Z0 基准高度 (mm)
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
├── depthZMin            11        深度过滤 Z 最小值 (mm)
└── depthZMax            135       深度过滤 Z 最大值 (mm)
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
| `direction` | int | 0=正向 Normal, 1=反向 Inverted |
| `maxSpeed` | double | 最大速度 |
| `maxAccel` | double | 最大加速度 |
| `jogSpeed` | double | 点动 (JOG) 速度 |
| `limitMin` | double | 软限位最小值 |
| `limitMax` | double | 软限位最大值 |
| `homeOffset` | double | 原点偏移 |
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

当前默认 `transmission` 值：`encoderResolution=131072`、`microSteps=512`、`gearRatio=50`、`lead=20.0`、`minPulse=500`、`maxPulse=2500`、`minAngle=0`、`maxAngle=180`。

### 默认轴列表

| Key | 名称 | 硬件类型 | sortOrder | limitMin | limitMax | jogSpeed |
|---|---|---|---|---|---|---|
| `Axis_J1` | [1] 轴1 (大臂 J1) | 运动控制卡 | 0 | -180 | 182 | 150 |
| `Axis_J2` | [2] 轴2 (小臂 J2) | 串口总线舵机 | 1 | -90 | 90 | 85 |
| `Axis_Z` | [3] 轴3 (Z轴) | 运动控制卡 | 2 | 0 | 200 | 150 |
| `Axis_R` | [4] 轴4 (翻转 R) | 串口总线舵机 | 3 | -180 | 180 | 100 |
| `Axis_Gripper` | [5] 轴5 (夹爪) | 运动控制卡 | 4 | 0 | 100 | 155 |
| `Axis_Extruder` | [6] 轴6 (挤出) | 运动控制卡 | 5 | 0 | 100 | 160 |

---

## 加载流程

1. ConfigPage 构造时调用 `ConfigManager::load()`
2. 查找顺序: 源码目录 `config/config.json` → 运行目录 `config.json`
3. 各 Tab 创建时通过 `dVal()`/`sVal()`/`iVal()` 读取当前值填入控件
4. 用户修改后通过信号 (editingFinished / valueChanged / currentIndexChanged) 触发 `ConfigManager::set()` → 300ms 去抖后自动写回文件
