# 上下移动动作（`ActionType::ZMove` = 6）

> **状态**：🟡 已实施待真机（2026-10-22，台账 **TR-153**）
> **需求来源**：用户 2026-10-22 提出——「工艺流程中增加一个动作类型：上下移动，从当前的坐标进行上下移动，动作界面有一个下拉框选择 上移/下移，一个输入框输入距离值」。
> **权威性**：本文只记录设计与落点。技术事实以源码为准；`process.json` 的字段级 schema 以 `doc/process.md` 为准。

---

## 一、语义裁定

| 项 | 裁定 | 理由 |
| :--- | :--- | :--- |
| 位移基准 | 执行到本动作时的**当前** `Axis_Z` 位置 | 用户原话「从当前的坐标」；不是方案启动位，也不是上一个点位 |
| 作用轴 | 逻辑轴 `Axis_Z`（**只此一根**） | 与 `PointData.j3` 同一坐标系：回零后 0、**向下为负** ⇒ 上移 = `+distance` |
| J1/J2/R | **完全不动** | 纯竖直位移，不产生水平扫掠；也避免 IK 双解歧义 |
| 是否走 IK | **不走** | 目标直接由「当前 Z ± 距离」得到，不经 `Kinematics::InverseSmart` |
| 距离符号 | 恒为**正值**，方向由独立字段 `zMoveUp` 决定 | 若存「带符号距离」，改方向会覆盖用户已填数值、改数值又会把符号写回方向，两个控件互相踩 |
| 越软限位 | **拒绝执行并报错**，不夹到限位 | 静默夹到限位会让「上移 20 mm」实际只走 5 mm，工艺数据与实走距离不一致 |
| 可重试性 | **不可重试**（`loopCfg.retryCount` 对它无效） | 相对位移不满足「重跑一次结果不变」的重试前提——已走一半时重试等于走 1.5 倍 |

### ⚠ 为什么落在 `Axis_Z` 而不是末端高度 `Pose.z`

两者相差一个**固定偏移**（`Z0 − h1 − tcpDown`），做**相对**位移时数学上等价。选 `Axis_Z` 的唯一好处是：不需要 FK 求当前位姿、不需要 IK 求目标关节角、不需要校验 J1/J2 是否变化——一根轴的绝对目标下发即可。**不要**为了「语义更贴近末端」改成先 FK 再 IK，那会引入双解歧义与 J1/J2 的舍入抖动。

---

## 二、数据模型

```cpp
enum class ActionType { Move = 0, Vision, Extrude, Delay, Gripper, VisionGuide, ZMove };  // ZMove = 6

// ActionData
bool   zMoveUp = true;        // true = 上移（+Z），false = 下移（−Z）
double zMoveDistance = 10.0;  // mm，恒正
```

`process.json` 侧（键名与字段同名）：

| 键 | 类型 | 缺键默认 | 说明 |
| :--- | :--- | :--- | :--- |
| `zMoveUp` | bool | `true` | `true` = 上移 |
| `zMoveDistance` | double | `10.0` | 距离 mm，恒正 |

**缺键默认取 10 mm 而非 0**：旧 `process.json` 里不可能存在 `type = 6` 的节点，该默认值只在「类型被手改成 6 但参数缺失」时起作用；取一个可执行的值比取 0 更安全（0 会在执行期直接报「距离必须大于 0」，用户还得再进一次面板）。

### 组内并发的资源占用

`ProcessManager::resourceKeysOf(ActionType::ZMove)` = **`{Axis_Z}`**（只有一根轴）。

- 与「移动 / 视觉引导」同组 → **被拒绝**（后两者也下发 `Axis_Z` 的绝对目标，两个绝对目标同时下发会让轴按后到者走）；
- 与「夹爪 / 挤压 / 延时 / 识别」同组 → **合法**（不同资源）。

### ✅ 与 `Capture` 预留号的冲突（已消解，2026-10-26）

`doc/third_party_integration_plan.md` 曾为「拍照」动作预留 `ActionType::Capture = 6`。`ZMove` 占用了 6，**该篇已于 2026-10-26 按 `Capture = 7` 落地（TR-157）**——枚举末尾追加、`kGroupPageIndex` 8→9、其余改动点（`actionTypeName` / 类型下拉 / 资源表 / `process.json` 读写）与本节描述一致。**本节保留作历史记录，冲突已不存在。**

---

## 三、执行层落点

沿用 P6 的**段状态机**（`RunnerState` + `StepRunner`），一个 `ZMove` 节点 = **1 段**：

| 阶段 | 函数 | 行为 |
| :--- | :--- | :--- |
| Begin | `PrepareRunnerSegment` 的 `case ActionType::ZMove` | 校验距离 > 0 → 读**当前** `GetPosition(Axis_Z)` → 算绝对目标 → `IsWithinSoftLimits` 校验 → 记日志 → `r.segTarget` / `r.segSpeed` / `r.axes = {Axis_Z}` |
| Dispatch | `DispatchRunnerSegment` | `MoveAbs(Axis_Z, r.segTarget, r.segSpeed)`，随后按 `GetAxisBusyMs(Axis_Z) × 1.2 + 3000` 设超时（下限 3000 ms） |
| Wait | `StepRunner` 通用逻辑 | 20 ms tick 轮询 `IsAxisBusy(Axis_Z)` |
| 续走 | `ReissueRunnerSegment` | 重发 **`r.segTarget`**（Begin 算好的绝对目标） |

**⚠ 续走必须重发绝对目标，不得按当前 Z 重算**：暂停时轴已走了一部分，重算会从半路再走一整段距离。这是相对动作在「暂停 / 继续」路径上最容易写错的一处，`ReissueRunnerSegment` 内有对应注释。

**⚠ 超时不能固定 30 s**：Z 是卡轴（开环步进），低速（`speedPercent = 10%`）走长距离时固定值会误判超时，故与夹爪同款按估算耗时动态兜底。

**⚠ 失败路径不 `emit errorOccurred`**：沿用 D17 方案 A，失败原因写入 `Impl::lastError`，由会话级单点发出。

---

## 四、UI 落点

| 位置 | 改动 |
| :--- | :--- |
| 类型下拉（`OnNewAction` / `OnEditAction` 两处 `addItems`） | 末尾追加「上下移动」——**索引即枚举值**，只能追加 |
| 详情页 | 新增 `ZMove` 参数页，位于 `m_detailStack` **索引 7**：方向下拉（上移 / 下移）+ 距离 `QDoubleSpinBox`（0.01~500 mm，步进 1 mm，后缀 `mm`）+ 语义说明 + 保存按钮 |
| `kGroupPageIndex` | **7 → 8**。`RefreshNodeDetail` 用 `static_cast<int>(type) + 1` 当 stack 索引，分组页必须排在**所有动作页之后**；漏改会让 `ZMove` 的详情页显示成「分组详情」 |
| 动作运行速度行 | `ZMove` 也显示（`speedPercent` 直接乘到 `Axis_Z` 的最大速度上） |
| 保存校验 | `OnSaveAction` 的 `case ActionType::ZMove`：距离 ≤ 0 时回滚快照 + 弹窗拒绝保存 |

---

## 五、改动清单

| 文件 | 改动 |
| :--- | :--- |
| `src/Config/ProcessManager.h` | 枚举末尾追加 `ZMove`；`ActionData` 加 `zMoveUp` / `zMoveDistance` |
| `src/Config/ProcessManager.cpp` | `actionTypeName` → 「上下移动」；`resourceKeysOf` → `{Axis_Z}`；`readNode` / `writeNode` 加两个键 |
| `src/Logic/SequenceWorker.cpp` | `SegmentCountOf` → 1；`ExecuteAction` 接受该类型；`PrepareRunnerSegment` / `DispatchRunnerSegment` / `ReissueRunnerSegment` 各加一个 `case`；`ExecuteNodeWithRetry` 注释说明**不可重试** |
| `src/UI/ProcessPage.h` | 加 `m_zMoveDirCombo` / `m_zMoveDistanceSpin` |
| `src/UI/ProcessPage.cpp` | `kGroupPageIndex` 7 → 8；新增参数页（索引 7）；两处类型下拉追加；`RefreshNodeDetail` / `OnSaveAction` 加分支；速度行可见条件加该类型 |
| `doc/process.md` | 类型表、`ActionData` 字段表、数据模型块、新节 |
| `doc/README.md` | 登记本文 |

---

## 六、验收用例

**档位约定**：`[A]` 离线可核对｜`[B]` Sim 可做（GUI 交互）｜`[C]` 真机必须。
**★ = 核心判据，不通过即停**。状态：`⏳` 待执行 / `🟢` 通过 / `⚠️` 偏差 / `❌` 失败。

> **编号说明**：本文用例**不进** `doc/test/real_machine_plan_phase3.md` 的批次体系——该文档的阶段 14（`third_party_integration_plan.md`）与阶段 15（`inspection_result_panel_plan.md`）已被两篇 ⏳ 方案预留，新插入会撞号。待本动作进入真机验收批次时，按当时的批次号并入该文档。

| # | 档 | 动作 | 期望 |
| :--- | :--- | :--- | :--- |
| Z1 ★ | A | 核对枚举与两处类型下拉 | `ActionType` 为 7 项、`ZMove` 在**末尾**；两处 `addItems` 末项为「上下移动」；`actionTypeName(ZMove)` = 「上下移动」 |
| Z2 ★ | A | 新建一个「上下移动」动作 → 保存 → 读 `config/process.json` | 节点写出 `"type": 6`、`"zMoveUp": true/false`、`"zMoveDistance": <值>`；重启后详情页回填一致 |
| Z3 | A | 手工把 `type` 改成 6、删掉 `zMoveUp` / `zMoveDistance` → 启动 | 不报错；加载为「上移 10.00 mm」（缺键默认，见 §二） |
| Z4 ★ | A | 核对 `resourceKeysOf(ZMove)` | 恰为 `{Axis_Z}`；与 `Move` / `VisionGuide` 有交集 ⇒ 组内校验会拒绝；与 `Gripper` / `Extrude` / `Delay` 无交集 ⇒ 放行 |
| Z5 ★ | B | Sim 下执行单个「上移 10 mm」动作 | 日志 `上下移动：上移 10.00 mm（Z a → b mm，速度 … mm/s）`，`b − a = 10.00`；J1/J2/R 读数**不变** |
| Z6 ★ | B | 承 Z5，改成「下移 10 mm」 | `b − a = −10.00`；同样不动 J1/J2/R |
| Z7 ★ | B | 构造「上移 200 mm」（超 Z 轴软限位上界） | 执行前即失败，提示含「目标 Z … 超出轴 Z 软限位（… ~ … mm）」；**无任何运动**；状态进 Fault，原因即该提示（不是「用户停止」） |
| Z8 ★ | B | 连续两个「上下移动」：上移 10 mm → 下移 10 mm | 第二个以**第一个的落点**为起点（日志里的 `a` 等于上一段的 `b`），最终回到起始 Z |
| Z9 ★ | B | 分组内放「上下移动」+「移动」→ 保存 | 被拒：`「…」与「…」并发时争用同一资源（Axis_Z）…`；分组内放「上下移动」+「夹爪」→ 通过 |
| Z10 ★ | B | 执行中「暂停」→「继续」 | 续走后走到**同一个绝对目标**（日志 `Z a → b` 的 `b` 与暂停前一致），不出现「再走一整段」；`b` 不是暂停点 ± 距离 |
| Z11 | B | 单步模式执行含「上下移动」的方案 | 该动作算**一步**，放行后一次走完 |
| Z12 | C | 真机：上移 / 下移各 10 mm，用钢尺或百分表核对 | 实走距离与设定值之差 ≤ 1 mm（开环步进，先记基线）；方向与下拉一致（上移 = Z 读数增大） |
| Z13 | C | 真机：在 Z 软限位边界附近执行越界动作 | 拒绝执行、机械臂**不动**；提示数值与实际限位一致 |

> **未执行**：Z1~Z13 全部待执行（本轮只做双编译 + Sim 冒烟，见 TR-153）。

---

## 七、已知限制

1. **相对动作在循环生产里会累积**：`loopCfg.mode = Count/Infinite` 时每轮都走同样距离，若干轮后必然撞软限位 → 届时**拒绝执行并报 Fault**（不会静默漂移）。需要「每轮回同一高度」的工艺应改用「移动」动作的绝对点位。
2. **不与「移动」同组**：`Axis_Z` 资源冲突，组内校验直接拒绝（见 §二）。
3. **真机未验**：Sim 下 `SimCard` 瞬时到位，实走距离、方向、软限位边界三项只能在真机判（Z12 / Z13）。
