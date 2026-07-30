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
| `schemeName` | string | 方案名称（如 "方案_100"） |
| `actions` | array | 动作列表 |

---

## ActionData — 动作

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | string | 动作名称 |
| `type` | int | 0=移动, 1=识别, 2=挤压, 3=延时, 4=夹爪 |
| `points` | array | 点位列表（仅 type=0 有效） |
| `visionType` | string | 识别类型（仅 type=1 有效: CCD/深度/激光） |
| `exposure` | double | 曝光时间（仅 type=1） |
| `templateName` | string | 匹配模板（仅 type=1） |
| `threshold` | double | 置信度阈值（仅 type=1） |
| `extrudeAmount` | double | 挤出量（仅 type=2） |
| `extrudeSpeed` | double | 挤出速度（仅 type=2） |
| `suckBackAmount` | double | 回抽量（仅 type=2） |
| `suckBackSpeed` | double | 回抽速度（仅 type=2） |
| `delayMs` | int | 延时毫秒数（仅 type=3） |
| `isGripperOpen` | bool | true=张开, false=闭合（仅 type=4） |

---

## PointData — 点位（仅移动动作）

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | string | 点名称（如 "fill_start"） |
| `x` | double | X 坐标 |
| `y` | double | Y 坐标 |
| `z` | double | Z 坐标 |
| `r` | double | R 旋转角 |
| `posture` | string | 姿态: "elbow_up" 或 "elbow_down" |

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
    │   ├── name, x, y, z, r, posture
    ├── visionType, exposure, templateName, threshold  (仅 Vision)
    ├── extrudeAmount, extrudeSpeed, suckBackAmount, suckBackSpeed  (仅 Extrude)
    ├── delayMs  (仅 Delay)
    └── isGripperOpen  (仅 Gripper)
```

### 依赖

- `nlohmann_json` — JSON 序列化/反序列化
- `spdlog` — 日志输出
- `Qt6::Core` — `QString`, `QVector`, `QFile`
