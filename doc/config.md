# CreamPuffRobot 配置文件说明

## 文件位置

`config/config.json` — 机器人控制系统的全部配置参数，系统启动时自动加载，用户修改后自动保存。

## 顶层结构

| 键 | 类型 | 对应界面 Tab |
|---|---|---|
| `communication` | object | 通信与连接 |
| `kinematics` | object | 运动学参数 |
| `vision` | object | 视觉与工艺参数 |
| `tcpCalibration` | object | TCP与标定 |
| `axes` | object | 电控与映射 |

---

## communication — 通信与连接

```
communication
├── motionCard
│   ├── ip       "192.168.1.100"   网口 IP
│   └── port     502               端口
├── servo
│   ├── port     "COM3"            舵机串口号
│   └── baudRate "115200"          波持率
└── camera
    └── sn       "SN-2023-0801"    奥比中光相机序列号
```

---

## kinematics — 运动学参数

```
kinematics
├── links
│   ├── l1       285.0             L1 (大臂长度 mm)
│   ├── l2       215.0             L2 (小臂长度 mm)
│   └── z0       45.0              Z0 基准高度 (mm)
└── axisParams[0..2]               每个轴的参数
    ├── name     "J1"/"J2"/"Z"     轴名称
    ├── pulsesPerUnit  0.012       脉冲当量 (脉冲/度 或 脉冲/mm)
    ├── limitMin       -180        软限位最小值
    └── limitMax       180         软限位最大值
```

---

## vision — 视觉与工艺参数

```
vision
├── confidenceThreshold  0.85      识别置信度阈值
├── depthZMin            10        深度过滤 Z 最小值 (mm)
└── depthZMax            120       深度过滤 Z 最大值 (mm)
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

---

## axes — 电控与映射

`axes` 是一个 JSON 对象，每个 key 的命名规则为 `{类型前缀}_{物理端口ID}`：
- `hardwareType = 0`（运动控制卡）→ `motor_{portId}`
- `hardwareType = 1`（串口总线舵机）→ `servo_{portId}`

相同类型（motor / servo）的物理端口 ID 不可重复，UI 会弹窗提示。

每个轴对象包含以下字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | string | 轴名称（独立命名） |
| `hardwareType` | int | 0=运动控制卡, 1=串口总线舵机 |
| `portId` | int | 物理端口 ID |
| `direction` | int | 0=正向 Normal, 1=反向 Inverted |
| `maxSpeed` | double | 最大速度 |
| `maxAccel` | double | 最大加速度 |
| `limitMin` | double | 软限位最小值 |
| `limitMax` | double | 软限位最大值 |
| `homeOffset` | double | 原点偏移 |

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

### 默认轴列表

| Key | 名称 | 默认硬件类型 |
|---|---|---|
| `motor_0` | [1] 轴1 (大臂 J1) | 运动控制卡 |
| `servo_1` | [2] 轴2 (小臂 J2) | 串口总线舵机 |
| `motor_1` | [3] 轴3 (Z轴) | 运动控制卡 |
| `servo_1_2` | [4] 轴4 (翻转 R) | 串口总线舵机 |
| `servo_2` | [5] 轴5 (夹爪) | 串口总线舵机 |
| `motor_6` | [6] 轴6 (挤出) | 运动控制卡 |

---

## 加载流程

1. ConfigPage 构造时调用 `ConfigManager::load()`
2. 查找顺序: 源码目录 `config/config.json` → 运行目录 `config.json`
3. 各 Tab 创建时通过 `dVal()`/`sVal()`/`iVal()` 读取当前值填入控件
4. 用户修改后通过信号 (editingFinished / valueChanged / currentIndexChanged) 触发 `ConfigManager::set()` → 300ms 去抖后自动写回文件
