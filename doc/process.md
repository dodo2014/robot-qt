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
| `nextActionId` | int | **动作编号计数器**（方案内单调递增，删除动作后**不回退**）。P2（2026-09-20）新增；旧文件无此键时加载即按「方案内最大编号 + 1」兜底 |
| `nodes` | array | **节点列表**（一级节点 = 动作或分组）。**P6（2026-09-20）由 `actions` 改名**；读取时**兼容旧键** `actions`（§7.3），首次保存写回 `nodes` |

当前默认包含 4 个方案：`方案_1`（移动/夹爪/识别/挤出完整工艺）、`方案_2`、`方案_966`、`方案_186`（空方案）。

---

## ActionData — 动作

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | int | **方案内唯一的动作编号**（正整数，P2 新增）。规则见下文「动作编号机制」。**分组也占编号** |
| `nodeType` | string | 节点类型：`"action"`（动作，默认）/ `"group"`（分组）。**P6 新增**。与 `type` **正交**——`Group` 不进 `ActionType` 枚举（见下文「分组节点与组内并发」） |
| `children` | array | **分组内的子节点**（仅 `nodeType == "group"` 非空，元素结构与 `ActionData` 相同）。**不允许嵌套**——分组只能在一级 |
| `name` | string | 动作名称 |
| `type` | int | 0=移动, 1=识别, 2=挤压, 3=延时, 4=夹爪, **5=视觉引导**（⚠ 只能追加在末尾，见下文） |
| `points` | array | 点位列表（仅 type=0 有效，结构见 PointData） |
| `visionType` | string | 识别类型（仅 type=1 有效: CCD/深度/激光） |
| `exposure` | double | 曝光时间（仅 type=1，当前默认 30.0） |
| `templateName` | string | 匹配模板（仅 type=1） |
| `threshold` | double | 置信度阈值（仅 type=1，当前 0.85）。**当前仅打印日志**，实际生效的是全局 `vision.confidenceThreshold` —— 见下文「全局参数与动作级参数的优先级」 |
| `extrudeAmount` | double | 挤出量（仅 type=2，当前 5.0） |
| `extrudeSpeed` | double | 挤出速度（仅 type=2，当前 2.0） |
| `suckBackAmount` | double | 回抽量（仅 type=2，当前 1.0） |
| `suckBackSpeed` | double | 回抽速度（仅 type=2，当前 3.0） |
| `delayMs` | int | 延时毫秒数（仅 type=3） |
| `isGripperOpen` | bool | true=张开, false=闭合（仅 type=4） |
| `gripperTarget` | double | 夹爪行程 = 轴5 绝对目标坐标，单位 mm；**0 = 夹紧，负值 = 松开**（仅 type=4，受轴5 软限位 −5.00~0.00 约束） |
| `sourceActionId` | int | **引用的识别动作编号**（仅 type=5，P5 新增）。必须指向**本方案内**类型为 `Vision` 的动作；**不允许跨方案引用** |
| `offsetZ` | double | 「目标上方」相对目标的 Z 偏移，单位 mm，**正值向上**（仅 type=5）。按**末端高度 `Pose.z`** 算，与 `PointData.j3`（Z 电机坐标）**不是同一个量**，不可混用 |
| `guideR` | double | 构造路径点的**轴 4(R) 固定值**，单位 °，默认 **179.9**（仅 type=5）。**与 `PuffResult.yaw` 无关** |
| `speedPercent` | int | 速度百分比（所有动作通用，当前 100） |

---

## 动作编号机制（2026-09-20 定，P2）

**权威定义在 `doc/vision_guide_action_plan.md` §四**，此处只记 `process.json` 侧的落点。

| 项 | 规则 |
| :--- | :--- |
| 编号类型 | **方案内唯一的正整数**（`id`）；作用域是**方案内**，跨方案不唯一 |
| 分配 | 新增动作时取 `scheme.nextActionId` 并推进计数器（`ProcessManager::allocateActionId`） |
| 删除 | **不复用**——计数器只增不减 |
| 插入 / 删除 / 上移 / 下移 | **都不重排、不回收**编号（重排会让 `sourceActionId` 引用静默错位） |
| 旧数据迁移 | 加载时 `id` 缺失 / 非正 / 重复 → 按「方案内最大编号 + 1」依次补发（**幂等**）；**不要求用户手工迁移**，首次保存写回新结构 |

**⚠ 为什么计数器必须落盘而不是现算 `max(id)+1`**：删掉当前最大编号的动作后再新增，`max(id)+1` 会**复用刚被删除的编号**，使既有的 `sourceActionId` 引用（P5，2026-09-20 已落地）静默指向新动作。计数器只增不减才能满足「删除不复用」。

**⚠ 编号不连续是正常的**（删除动作后留空）——**不要做「整理编号」的功能**，那会破坏引用。

**⚠ 升级是惰性的**：只加载不编辑就不会写盘，`process.json` 会保持旧结构（无 `id` / `nextActionId`），下次任意编辑触发 `save()` 时才写回。功能上无影响。

---

## 视觉引导动作（type=5，2026-09-20 定，P5）

**权威定义在 `doc/vision_guide_action_plan.md`**，此处只记 `process.json` 侧的落点。

| 项 | 规则 |
| :--- | :--- |
| 存什么 | **只存动作定义**——`sourceActionId` / `offsetZ` / `guideR`（+ 通用字段） |
| **不存什么** | **识别坐标绝不写入 `process.json`**。坐标是运行结果，只走日志（`[SequenceWorker] Vision target: cam(…) → robot(…)`）+ 会话内存（`QHash<int, Pose> visionResults_`，**会话结束即丢弃**）。理由：`process.json` 是方案定义，被运行数据污染后难以人工维护与版本管理 |
| 执行 | 按 `sourceActionId` 取识别结果 → 构造 `目标上方 (x, y, z + offsetZ, guideR)` 与 `目标 (x, y, z, guideR)` → 两段移动动作（复用移动动作执行路径），**顺序严格：先上方、后目标** |
| 取不到结果 | 报错且**不产生任何运动**（`sourceActionId <= 0`，或该编号本次会话没有结果） |
| 校验 | **编辑期**：`ProcessPage::OnSaveAction` 拒绝保存（未选择 / 编号不存在 / 类型非 `Vision`）；**执行期**：`SequenceWorker::PrepareRunnerSegment` 的 `case ActionType::VisionGuide` 写 `failReason`（由 `RunScheduler` 汇总进 `Impl::lastError`）并返回 `false` |
| 编号引用 | 按**稳定编号**引用而非「上一个识别动作」——插入 / 删除 / 上移 / 下移都不会让引用错位（见下文「动作编号机制」） |

---

## 分组节点与组内并发（2026-09-20 定，P6）

**权威定义在 `doc/action_tree_concurrency_plan.md`**（§六 校验 / §七 数据模型 / §九 执行层 / §Q1~§Q5），此处只记 `process.json` 侧与校验侧的落点。

| 项 | 规则 |
| :--- | :--- |
| 结构 | **两级树**：一级节点是动作或分组；分组的 `children` 内只能是动作。**不允许嵌套**（§Q5），数据层与 UI 层**双层拦截** |
| 分组与编号 | 分组**占一个方案内编号**（与动作同一编号空间，§7.2）；删除后编号**不复用**（计数器只增不减） |
| `Group` 不进枚举 | `nodeType` 是独立字段。理由：`ActionType` 整数持久化且 `VisionGuide` 已占 `5`，且类型下拉的**索引直接就是枚举值**（§7.1） |
| 组内并发 | **同时下发、统一汇合**——组内子动作在同一 20 ms 轮询节拍内推进，等待时间重叠，故整组耗时 ≈ **最长子动作**而非各子动作之和 |
| 资源互斥 | 校验**按资源不按类型**：`Move` → `{Axis_J1, Axis_J2, Axis_Z, Axis_R}`；`Gripper` → `{Axis_Gripper}`；`Extrude` → `{Axis_Extruder}`；`Vision` → `{Camera}`；**`VisionGuide` 与 `Move` 共用四轴集合**；`Delay` → **空集**（多个延时同组**合法**，§6.3） |
| 数据依赖 | 同组内**不允许**「`Vision` + 引用该 `Vision` 编号的 `VisionGuide`」（§6.4）——并发时移动目标尚未算出 |
| 失败归因 | **首个失败者**：其失败原因写入 `Impl::lastError`，其余子动作的失败仅记日志；分组失败**先安全停下全部子动作再上报**（§Q2），绝不提前返回 |
| 重试 | `production.loop.retryCount` 重试的是**整个分组**（§Q3） |
| 旧数据迁移 | 加载时非法嵌套被**扁平化**（内层分组的子节点上提为外层子节点，记 `Group 'X' had nested group(s): flattened N child node(s) -> M`）；旧键 `actions` 与缺失的 `nodeType`（缺省即 `"action"`）均可读。**升级仍是惰性的**——只加载不编辑不写盘 |

## 全局参数与动作级参数的优先级（2026-09-20 定，P1）

`config/config.json` 的 `vision.*`（**全局**）与 `ActionData` 中的同名字段（**动作级**）同时存在，优先级规则定为：

> **动作级覆盖全局；全局作为默认值。**

即：动作级字段**有值**时以动作级为准；动作级**未提供**（缺字段 / `0` / 空串）时回落到全局 `vision.*`。该规则适用于 `threshold` ↔ `vision.confidenceThreshold`、`exposure` ↔ `vision.exposure` 等成对参数。

### ⚠ 实现现状（2026-09-20 核实，勿误以为已生效）

| 动作级字段 | 消费点 | 现状 |
|---|---|---|
| `visionType` | `SequenceWorker.cpp:886` | **仅打印日志**（「视觉：采帧中 (%1)」），不影响采帧方式 |
| `threshold` | `SequenceWorker.cpp:889` | **仅打印日志**（「视觉：检测中 (阈值 %1)」）。紧随其后的 `algo->Detect(frame)`（`:889`）**不接收该参数** → 动作级阈值不影响识别结果。**当前真正生效的是全局 `vision.confidenceThreshold`**（P1 起由 `SimAlgo::Detect()` 按该值过滤，见 `doc/config.md`） |
| `templateName` | 无 | **无读点**（仅序列化往返 + 工艺流程页可编辑） |
| `exposure` | 无（动作级） | 动作级**无读点**；全局 `vision.exposure` 有写入口（视觉检测页曝光框） |

> **⚠ 本节的落地（让动作级参数真正生效）不在 P5 范围内**——P5（`doc/vision_guide_action_plan.md`，2026-09-20 落地，TR-101）只做「视觉引导」动作本身，**未触碰动作级参数链路**。P1 只**确立优先级规则并记录现状**；上表四项（`visionType` / `threshold` 仅打印日志、`templateName` 无读点、动作级 `exposure` 无读点）**至今未变**，待后续阶段处理。

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
| `joints` | object | 关节角（2026-09-04 TR-071 起落真实示教值；手改坐标保存时该字段删除） |
| &nbsp;&nbsp;`hasJoints` | bool | 关节角有效标记（true 时执行优先按关节角直发） |
| &nbsp;&nbsp;`j1`~`j4` | double | 轴1~4（J1/J2/Z/R）关节角；**j3=Z 电机坐标**（回零后=0、向下为负，与 coord.z 差固定偏移 165，勿混用） |

注意事项：

- 内存中 `PointData` 仍为扁平字段（`x,y,z,r,posture,hasJoints,j1~j4`），嵌套只影响 JSON 序列化格式。
- **加载只读取 `coord` 下的 `x/y/z/r`**，不支持旧版扁平 `x/y/z/r` 格式。
- ~~`joints` 目前为占位（保存恒为 0.0）~~ **已落地（TR-071，2026-09-04）**：示教读取落盘真实关节角 + `hasJoints=true`；执行 `ResolveRunnerJoints` 优先按 joints 直发（FK 一致性校验 ±0.5mm/0.5°，不匹配回退 IK——拦截旧版全 0 占位残留）；表格手改坐标保存 → 该点 joints **删字段**（非置 0，0 是合法关节角）。

---

## ProcessManager API

文件: `src/Config/ProcessManager.h/.cpp`

```cpp
class ProcessManager {
    static ProcessManager& instance();
    QVector<SchemeData>& schemes();
    void load(bool force = false);  // 从 process.json 读取（幂等，见下）
    void save();                    // 写入 process.json（QSaveFile 原子写）
    static int allocateActionId(SchemeData& scheme);      // 分配动作编号并推进计数器（P2）
    static int normalizeActionIds(SchemeData& scheme);    // 规范化编号，返回改写数（P2，幂等）
    static QString actionTypeName(ActionType t);
    static QString generateUniqueSchemeName(const QVector<SchemeData>& existing);
};
```

> **`load()` 的幂等语义（2026-09-20，P1）**：`load()` 有**两个调用点**——`MainWindow` 构造期（`MainWindow.cpp:226`）与 `ProcessPage` 构造期（`ProcessPage.cpp:78`）。此前两次调用都会**无条件清空 `m_schemes` 再重读文件**，若两次构造之间存在未落盘的编辑，第二次读取会把内存中的改动**整个丢弃**。现改为 `m_loaded` 闩锁：**已加载且 `force=false` 时直接返回**；确需强制重读（如「从配置重新加载」）才传 `force=true`。
>
> **`save()` 的原子写（2026-09-20，P1）**：原实现用 `std::ofstream` **就地截断**写目标文件——写到一半失败会**丢失全部方案**（不止当前方案）。现改用 `QSaveFile`（同目录临时文件 → `commit()` 原子替换），写入长度不足即 `cancelWriting()`，原文件保持不变。该缺陷此前**未见于任何方案文档**（`doc/config_ownership_plan.md` §七 只记了 `ConfigManager::save()` 的同类问题），属 P1 实施中发现并一并修复。

### 数据模型 (定义在 ProcessManager.h 中)

```
ActionType: Move=0, Vision=1, Extrude=2, Delay=3, Gripper=4, VisionGuide=5
            （⚠ 只能追加在末尾——type 是整数持久化的，插中间会让所有已保存方案错位）

SchemeData
├── schemeName: QString
├── nextActionId: int                 (动作编号计数器，单调递增、删除不回退)
└── nodes: QVector<ActionData>        (由 actions 改名，P6；读取兼容旧键 actions)
    ├── id: int                       (方案内唯一节点编号，正整数；分组也占编号)
    ├── name: QString
    ├── nodeType: NodeType            (Action / Group，P6；与 type 正交，Group 不进 ActionType)
    ├── children: QVector<ActionData> (仅 Group 非空；不允许嵌套，元素结构同 ActionData)
    ├── type: ActionType              (仅 nodeType == Action 有意义)
    ├── points: QVector<PointData>  (仅 Move)
    │   ├── name, posture             (点层级)
    │   └── x, y, z, r                (序列化为 coord 子对象)
    │   └── hasJoints + joints j1..j4 (序列化为 joints 子对象；示教落真实关节角，
    │                                  手改坐标保存时删字段；见上文注意事项)
    ├── visionType, exposure, templateName, threshold  (仅 Vision)
    ├── extrudeAmount, extrudeSpeed, suckBackAmount, suckBackSpeed  (仅 Extrude)
    ├── delayMs  (仅 Delay)
    └── isGripperOpen, gripperTarget  (仅 Gripper)
```

### 依赖

- `nlohmann_json` — JSON 序列化/反序列化
- `spdlog` — 日志输出
- `Qt6::Core` — `QString`, `QVector`, `QSet`（编号去重）, `QFile`, `QSaveFile`（原子写）
- `Config/ConfigPaths.h` — 文件路径解析（`ConfigPaths::processJsonPath()`），**不再自带 `processJsonPath()` 实现**（2026-09-20，P1）
