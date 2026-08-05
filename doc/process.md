# 工艺与流程配置 (process.json)

## 文件位置

`config/process.json` — 工艺方案与动作数据，在 ProcessPage 界面中编辑，通过 `ProcessManager` 自动保存。

---

## 顶层结构

```json
{
    "schemes": [ ... ]
}
```

---

## SchemeData — 方案

| 字段 | 类型 | 说明 |
|------|------|------|
| `schemeName` | string | 方案名称（如 "方案_1"） |
| `actions` | array | 动作列表 |

当前默认包含 4 个方案：`方案_1`（移动/夹爪/识别/挤出完整工艺）、`方案_2`、`方案_966`、`方案_186`（空方案）。

---

## ActionData — 动作

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | string | 动作名称 |
| `type` | int | 0=移动, 1=识别, 2=挤压, 3=延时, 4=夹爪 |
| `points` | array | 点位列表（仅 type=0 有效，结构见 PointData） |
| `visionType` | string | 识别类型（仅 type=1 有效: CCD/深度/激光） |
| `exposure` | double | 曝光时间（仅 type=1，当前默认 30.0） |
| `templateName` | string | 匹配模板（仅 type=1） |
| `threshold` | double | 置信度阈值（仅 type=1，当前 0.85） |
| `extrudeAmount` | double | 挤出量（仅 type=2，当前 5.0） |
| `extrudeSpeed` | double | 挤出速度（仅 type=2，当前 2.0） |
| `suckBackAmount` | double | 回抽量（仅 type=2，当前 1.0） |
| `suckBackSpeed` | double | 回抽速度（仅 type=2，当前 3.0） |
| `delayMs` | int | 延时毫秒数（仅 type=3） |
| `isGripperOpen` | bool | true=张开, false=闭合（仅 type=4） |
| `speedPercent` | int | 速度百分比（所有动作通用，当前 100） |

---

## PointData — 点位（仅移动动作）

点对象在 JSON 中为**嵌套结构**，`name`/`posture` 在点层级，坐标在 `coord` 子对象，关节角在 `joints` 子对象：

```json
{
    "name": "fill_start",
    "posture": "elbow_up",
    "coord": { "x": 85.0, "y": 92.0, "z": 22.0, "r": 2.0 },
    "joints": { "j1": 0.0, "j2": 0.0, "j3": 0.0, "j4": 0.0 }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | string | 点名称（如 "fill_start"） |
| `posture` | string | 姿态: "elbow_up" 或 "elbow_down" |
| `coord` | object | 笛卡尔坐标（嵌套对象） |
| &nbsp;&nbsp;`x` | double | X 坐标 (mm) |
| &nbsp;&nbsp;`y` | double | Y 坐标 (mm) |
| &nbsp;&nbsp;`z` | double | Z 坐标 (mm) |
| &nbsp;&nbsp;`r` | double | R 旋转角 (°) |
| `joints` | object | 关节角（占位，当前恒为 0.0） |
| &nbsp;&nbsp;`j1`~`j4` | double | 轴1~4（J1/J2/Z/R）关节角 |

注意事项：

- 内存中 `PointData` 仍为扁平字段（`x,y,z,r,posture`），嵌套只影响 JSON 序列化格式。
- **加载只读取 `coord` 下的 `x/y/z/r`**，不支持旧版扁平 `x/y/z/r` 格式。
- `joints` 目前为占位（保存恒为 0.0），后续接入真机关节反馈后填充真实关节角。

---

## ProcessManager API

文件: `src/Config/ProcessManager.h/.cpp`

```cpp
class ProcessManager {
    static ProcessManager& instance();
    QVector<SchemeData>& schemes();
    void load();   // 从 process.json 读取
    void save();   // 写入 process.json
    static QString actionTypeName(ActionType t);
    static QString generateUniqueSchemeName(const QVector<SchemeData>& existing);
};
```

### 数据模型 (定义在 ProcessManager.h 中)

```
ActionType: Move=0, Vision=1, Extrude=2, Delay=3, Gripper=4

SchemeData
├── schemeName: QString
└── actions: QVector<ActionData>
    ├── name: QString
    ├── type: ActionType
    ├── points: QVector<PointData>  (仅 Move)
    │   ├── name, posture             (点层级)
    │   └── x, y, z, r                (序列化为 coord 子对象)
    │   └── joints 占位 j1..j4=0      (序列化为 joints 子对象)
    ├── visionType, exposure, templateName, threshold  (仅 Vision)
    ├── extrudeAmount, extrudeSpeed, suckBackAmount, suckBackSpeed  (仅 Extrude)
    ├── delayMs  (仅 Delay)
    └── isGripperOpen  (仅 Gripper)
```

### 依赖

- `nlohmann_json` — JSON 序列化/反序列化
- `spdlog` — 日志输出
- `Qt6::Core` — `QString`, `QVector`, `QFile`
