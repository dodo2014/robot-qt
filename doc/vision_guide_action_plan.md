# 视觉引导动作（Vision Guide）方案

> **需求来源**：用户 2026-09-17 裁定。本文性质：**方案 → 🟡 全文已实施待真机**（2026-09-20）。**§四「动作编号机制」已随 P2 落地**（TR-098）；**§三 枚举陷阱、§四.3 校验、§五 动作定义、§八 改动清单已随 P5 全部落地**（2026-09-20，**台账 TR-101**，编排 `doc/implementation_roadmap.md` P5）。**唯一未完成的是真机验收**——须先完成手眼矩阵标定（§9.2 遗留 1），标定前视觉引导坐标**只能用于仿真验证**。
> **前置条件**：相机为**眼在手外（Eye-to-Hand，固定安装）**已确认；相机安装参数与基座标的关系**标定待实现**。
> **关联文档**：`doc/camera_integration_plan.md`（相机接入）、`doc/process.md`（动作 schema）、`doc/architecture.md`（`SequenceWorker` 引擎）。

## 一、需求

现有 `ActionType::Vision`（中文名「识别」，见 `ProcessManager::actionTypeName()`）已能采帧 → 检测 → 手眼换算，但**结果算完即丢**（`SequenceWorker::StepRunner` 的 `case ActionType::Vision` 分支里 `robot` 是局部变量）。工艺上需要「识别 → 按识别结果移动」的闭环。

用户裁定：

1. 方案中新增「**视觉引导**」动作类型。
2. **动作加明确编号**，视觉引导动作**指定识别动作的编号**（不依赖时序邻近）。
3. 「目标上方」的偏移量做成**动作参数**。
4. 视觉引导动作含**两组坐标**：目标上方 + 目标。
5. **`PuffResult.yaw` 是相机平面内的目标偏航角，本项目实际用不上**（目标为泡芙，yaw 无工艺意义）——它**不参与坐标构造**；视觉引导构造的 `Pose.r` 使用**轴 4(R) 的固定值 180°（或 179.9°）**。
6. 相机眼在手外（固定安装）。
7. 视觉引导的**两组坐标在 UI 上可见、只读**。
8. `sourceActionId` **不允许跨方案引用**（仅同方案内）。
9. `offsetZ` 按**末端高度（`Pose.z`）**算，正值向上，与 `PointData.j3` 严格区分。
10. **TCP 由运动学内化处理**，视觉引导侧不重复做 TCP 变换（§5.6）。

## 二、现状核实

| 事实 | 位置 |
| :--- | :--- |
| `ActionType` 枚举定义 | `ProcessManager.h` 的 `enum class ActionType` |
| `type` 在 `process.json` 里是**整数持久化** | `config/process.json`（现有 0~5，含 `VisionGuide = 5`） |
| `Vision` 的中文名是「识别」 | `ProcessManager::actionTypeName()` |
| 识别动作的执行分支已做手眼换算但结果丢弃 | `SequenceWorker::StepRunner` 的 `case ActionType::Vision` |
| `CameraToRobot` 只返回 x/y/z，**r 由上层业务决定** | `CoordTransform.h` 类注释「姿态(r)由上层业务决定」 |
| `PuffResult.yaw` 字段存在；`SimAlgo` 恒返回 0。**本方案不使用该字段** | `IPuffAlgorithm.h`（`PuffResult.yaw`）、`SimAlgo.cpp`（`res.yaw = 0.0`） |
| 移动动作的执行路径已含 R 归一化 / FK 校验 / IK 回退 / 到位等待 | `SequenceWorker::PrepareRunnerSegment` → `ResolveRunnerJoints` → `DispatchRunnerSegment` |
| **`Pose` 即夹爪尖端目标，TCP 已内化**（`tcpForward_` / `tcpDown_`） | `Kinematics::SetTCP` / `Kinematics::Forward`、`SequenceWorker::ReloadFromConfig`（读 `tcpCalibration.*` 后调 `SetTCP`） |
| 动作编辑 UI 的两处 `switch (node->type)` | `ProcessPage::RefreshNodeDetail`（详情刷新）、`ProcessPage::OnSaveAction`（保存） |
| 相机流启停入口只在视觉页 | `VisionTestPage::OnStartStream`（`AcquireCamera("VisionPage")`） / `VisionTestPage::OnStopStream`（`ReleaseCamera("VisionPage")`） |

## 三、⚠ 枚举值陷阱（实施前必读）

`enum class ActionType { Move = 0, Vision, Extrude, Delay, Gripper }`（**加 `VisionGuide` 之前**的形态），而 `type` 在 `process.json` 里是**整数持久化**的。

**禁止插在中间**——若写成 `{ Move, Vision, VisionGuide, Extrude, Delay, Gripper }`：

| 动作 | 原值 | 插队后 | 后果 |
| :--- | :--- | :--- | :--- |
| `Extrude` | 2 | 3 | 原「挤压」动作被解析为「延时」 |
| `Delay` | 3 | 4 | **原「等待 1 秒」被当「夹爪」执行** |
| `Gripper` | 4 | 5 | 原「夹爪」动作被解析为未知类型 |

**必须追加到末尾**（**✅ 已于 P2（2026-09-20，TR-098）落地**）：

```cpp
enum class ActionType { Move = 0, Vision, Extrude, Delay, Gripper, VisionGuide };  // VisionGuide = 5
```

## 四、动作编号机制（新增）

> **✅ 已实施（2026-09-20，P2 / TR-098）**——本节的 4.2 编号机制与 4.3 的「编辑期校验」分工如下：**4.2 已全部落地**（见下方 4.2 表后的实施说明）；**4.3 的编辑期校验已随 P5 落地**（2026-09-20，TR-101——`sourceActionId` 字段本身也是 P5 才加的）。
>
> 落地要点：编号计数器落在 `SchemeData::nextActionId`（**单调递增、删除不回退**）；`ProcessManager::allocateActionId()` 分配、`normalizeActionIds()` 在加载时对缺失/非正/重复编号按「最大编号 + 1」依次补发（**幂等**）；`id` 与 `nextActionId` 随 `save()` 落盘。`ActionType::VisionGuide = 5` 已按 §三 追加在枚举末尾，`actionTypeName` 与类型下拉项同步。**`process.json` 的 `actions` → `nodes` 改名已于 P6（2026-09-20，TR-102）落地**——写盘统一写 `nodes`，读取兼容旧键 `actions`，升级惰性（理由见 `doc/action_tree_concurrency_plan.md` §7.3）。

### 4.1 为什么需要

视觉引导要引用「哪个识别动作」的结果。若靠「上一个识别动作」的时序约定，一旦中间插入动作或调整顺序，引用会**静默错位**（错位后仍能跑，只是移到错误的坐标）。用**稳定编号显式引用**消除这个隐患。

### 4.2 设计

| 项 | 设计 |
| :--- | :--- |
| 新字段 | `ActionData` 加 `int id = 0;`（方案内唯一） |
| 分配 | 每个 `SchemeData` 维护 `nextActionId`，新增动作时分配递增编号 |
| **稳定性** | **插入 / 删除 / 上移 / 下移动作都不重排、不回收编号**——引用按 `id` 查找，重排会让引用静默错位 |
| 旧数据迁移 | `normalizeActionIds()` 加载时**保留显式且不冲突的编号**，只对缺失 / 非正 / 重复的按「方案内最大编号 + 1」依次补发（**幂等**，不重排已有编号）；下次保存落盘 |
| UI 显示 | 动作列表显示 `#N` 前缀（如 `#3 视觉识别`） |
| 持久化 | `process.json` 的 action 对象加 `"id": N` |

**编号不连续是正常的**（删除动作后留空），不应做「整理编号」的功能——那会破坏引用。

### 4.3 校验

| 时机 | 规则 | 落地（2026-09-20，TR-101） |
| :--- | :--- | :--- |
| 编辑期 | `sourceActionId` 必须指向**存在且类型为 `Vision`** 的动作，否则提示并拒绝保存（`ProcessPage::OnSaveAction`） | ✅ `OnSaveAction` 的 `VisionGuide` 分支：编号须在**本方案内**存在且类型为 `Vision`，否则 `QMessageBox::warning` + `return`（**不落盘**）；引用下拉**首项为 `（未选择）` 占位**（`userData = 0`，对应 `sourceActionId` 的「未设置」，2026-09-28 TR-120 补——缺了它 `QComboBox::addItem` 会把首个识别动作自动置为 current，「未选择」状态在 UI 上无从构造），对**悬垂引用显式列出 `#N（已不存在）`**，不静默落到第一个合法项 |
| 执行期（必须） | 查不到该编号的识别结果 → `impl_->lastError` + 返回 `false`，**不产生任何运动** | ✅ `PrepareRunnerSegment` 的 `case ActionType::VisionGuide` 两条失败路径（`sourceActionId <= 0` / 结果表 `constFind` 未命中）均写 `r.failReason`（由 `RunScheduler` 汇总进 `impl_->lastError`）并 `return false`，**先于任何下发** |

## 五、视觉引导动作定义

### 5.1 参数（`ActionData` 新增字段）

| 字段 | 类型 | 说明 |
| :--- | :--- | :--- |
| `sourceActionId` | `int` | 引用的**识别动作编号**（必填） |
| `offsetZ` | `double` | 「目标上方」相对目标的 Z 偏移，单位 mm，正值向上 |
| `guideR` | `double` | 构造路径点的**轴 4(R) 固定值**，默认 **179.9°**（亦可用 180°） |
| `speedPercent` | 复用现有 | 速度百分比 |
| `id` | `int` | 本动作自身的编号（§四） |

### 5.2 两组坐标（执行时构造）

识别动作产出基座坐标 `P = (x, y, z)`（`PixelToRobot` 的结果——2026-10-13 TR-124 起由 `CameraToRobot` 改为 `PixelToRobot`：像素 + 深度经**真实内参**反投影再走手眼变换），视觉引导据此构造两个路径点：

| 路径点 | 坐标 | 说明 |
| :--- | :--- | :--- |
| **目标上方** | `(x, y, z + offsetZ, guideR)` | 先抬到目标上方，避免从低位水平横扫桌面 |
| **目标** | `(x, y, z, guideR)` | 再下降到目标 |

两个点的 `r` 一律取 `guideR`（**轴 4 固定值**），**不是** `PuffResult.yaw`——yaw 是相机平面内的偏航角，不参与坐标构造（见 §七）。

**顺序严格：先上方、后目标**——与 `doc/archive/global_safe_position.md` 的「先抬 Z 再水平移动」同源约束。

### 5.3 执行流程

```
PrepareRunnerSegment(r) 的 case ActionType::VisionGuide：
  1. 按 action.sourceActionId 从会话视觉结果表取 Pose
     └─ 取不到 → r.failReason = "视觉引导：…" → return false（不产生任何运动）
  2. 构造上方点 pt1 = { x, y, z + offsetZ, r = guideR }
  3. 构造目标点 pt2 = { x, y,        z,        r = guideR }
  4. ResolveRunnerJoints(pt) → R 归一化 + FK 校验 + InverseSmart 回退 IK
  5. DispatchRunnerSegment() → DispatchPointMove() 下发 → 调度器轮询到位
```

**复用移动动作的同一套执行路径**（`PrepareRunnerSegment` → `ResolveRunnerJoints` → `DispatchRunnerSegment`）：零新运动代码，自动获得 IK 双解就近、软限位拦截、FK 校验、到位等待、暂停 / 停止响应。

### 5.4 视觉结果表

| 项 | 设计 |
| :--- | :--- |
| 存储 | `SequenceWorker::Impl` 新增 `QHash<int, Pose> visionResults_;`（key = 识别动作的 `id`） |
| 写入 | `StepRunner` 的 `case ActionType::Vision` 分支：`impl_->visionResults_.insert(action.id, robot);` + `emit visionResultReady` |
| **清空** | **每次 `RunSequence` 开始必须清空**——防上次会话的残留结果被误用（与 TR-091「循环外壳在会话内」同类问题） |
| 循环生产 | 每轮识别动作覆盖同 `id` 的结果，语义正确（每轮重新识别） |

### 5.5 坐标的持久化与 UI 可见性

**识别坐标不写入任何文件。** 现状与设计：

| 项 | 现状 / 设计 |
| :--- | :--- |
| `process.json` | 存的是**动作定义**（`templateName` / `threshold` / `exposure` / `visionType`），**不是运行结果**——视觉引导所需的坐标**不会**写进去 |
| 会话内 | `QHash<int, Pose> visionResults_` 内存缓存，**会话结束即丢弃**（`RunSequence` 开始清空） |
| **日志** | 坐标**已经落盘**——`StepRunner` 的 `case ActionType::Vision` 分支内有 `SPDLOG_INFO("[SequenceWorker] Vision target: cam(…) → robot(…) [node #N yaw=… ignored]")`，可从 `log/creampuff_YYYY-MM-DD.log` 追溯 |
| 存图 | `FrameSaver` 可存 RGB / Depth / 叠加图（`VisionTestPage` 手动触发），与坐标无关 |

**若后续需要结构化持久化**（如按批次追溯识别结果做质量分析），建议**独立的运行记录文件**（CSV / JSON 追加），**不要混进 `process.json`**——后者是方案定义，被运行数据污染后难以人工维护与版本管理。

**UI 可见性（用户 2026-09-17 裁定）**：视觉引导动作的**两组坐标在界面上可见、只读**——用于运行前核对与运行后追溯。落点在 `ProcessPage` 的视觉引导编辑区（见 §八），**不可编辑**（坐标由识别结果决定，手改会与识别结果不一致）。

### 5.6 坐标链路与 TCP

**`Pose` 的语义 = 夹爪尖端（TCP）的目标位置**，TCP 补偿**已经内化在运动学里**，不在调用侧处理：

| 环节 | 实现 |
| :--- | :--- |
| 配置 | `config.tcpCalibration.toolOffsetX/Y/Z` |
| 喂入 | `SequenceWorker::ReloadFromConfig` 读 `tcpCalibration.toolOffsetX/Y/Z` → `impl_->kin.SetTCP(tox, toy, toz)` |
| 内化 | `Kinematics::SetTCP`：`tcpForward_ = offsetX`（沿小臂 L2 向前）、`tcpDown_ = -offsetZ`（竖直向下，恒正） |
| 正解 | `Kinematics::Forward` 的 `pose.z = joints.z + z0_ - h1_ - tcpDown_`（夹爪尖端真实高度） |
| 逆解 | `Kinematics::Forward` 的 `l2_eff = l2_ + tcpForward_`（等效小臂） |

**因此**：视觉给出的目标点坐标**直接作为 `Pose` 输入** `Inverse` 即可，`Kinematics` 会自动按 TCP 补偿算出关节角——**不需要在视觉引导里再写一遍 TCP 变换**（旧 `ApplyTCPOffset` / `AddTCPOffset` 已删除，见 `AGENTS.md`）。

⚠ **待确认的对应关系**：视觉返回的点是目标（泡芙）的**哪个部位**，与「夹爪尖端要到达的位置」是否一致——若算法返回顶部中心而夹爪要夹腰部，两者之间还需一个**工艺偏移量**（这不是 TCP，TCP 描述的是工具几何）。**待相机安装与算法输出定义明确后确认**。

## 六、眼在手外的影响

相机固定安装 → 机器人移动**不改变**已识别目标的基座坐标有效性 → **视觉引导与识别动作之间可以间隔其他动作**，不必紧邻。这正是采用「按编号引用」而非「时序约定」的前提。

⚠ **相机安装参数与基座标的关系标定待实现**——现行 `config.tcpCalibration.handEyeMatrix` 是**非标定值**（平移 `180, −5.7, 38.1` mm；2026-09-28 阶段 12B 为让 `SimAlgo` 的构造目标落进可达环而把 x 由 `12.3` 手工改为 `180`，**仅为仿真验证凑数，不是标定结果**）。标定完成前，视觉引导的坐标**只能用于仿真验证**。

> ⚠ **为什么必须改 x**：`SimAlgo` 在 640×480 图像里把目标放在 `cx=320` 附近、`ampX=120`、`fx=520`、`z=55` ⇒ 相机系 x ∈ ±12.7 mm；叠加原手眼平移 x=12.3 后机器人 x ∈ [−0.4, 25.0] mm，**极径只有几十毫米量级，远落进 IK 不可达环**。改成 180 后机器人 x ∈ [167.3, 192.7] mm，落进可达环。**可达环下界不是几何内边界** `|L1 − L2_eff| = 81.03`（`Kinematics.cpp` 的 annulus 检查用的正是它），而是 **J2 限位**（`config.json` 的 `axes.Axis_J2 = [−90, 152]`）决定的 **117.10 mm**：`r(J2) = √(L1² + L2_eff² + 2·L1·L2_eff·cos J2)`，`L2_eff = L2 + toolOffsetX = 166.86 + 53.0 = 219.86`；`r` 在 `J2 = 152` 处取最小 `117.0960`、在 `J2 = 0` 处取最大 `358.69`。故 **实际可达环 = [117.10, 358.69]**。

## 七、R 角处理（易混点）

**结论：`Pose.r` 与 `PuffResult.yaw` 是两个无关的东西。**

| 概念 | 含义 | 本方案中的用法 |
| :--- | :--- | :--- |
| `PuffResult.yaw` | 目标在**相机图像平面内**的偏航角（旋转姿态） | **不用**——不参与坐标构造、不赋给 `Pose.r`。本项目目标为泡芙，yaw 无工艺意义 |
| `Pose.r`（构造路径点的 r） | 轴 4(R) 的**目标角度** | 取**固定值** `guideR`，默认 **179.9°** |

即：识别结果里的 `yaw` 字段照常从算法返回（`IPuffAlgorithm` 接口不动），但**视觉引导构造坐标时完全不引用它**，`pt1` / `pt2` 的 `r` 一律是轴 4 的固定值。

**为什么建议默认 179.9° 而非 180°**：R 轴软限位为 `[0, 185]`（`config/config.json` 的 `axes.Axis_R`），而 R 物理停在 180° 附近时实测读数会在 `{179.9, 180, -179.9}` 之间跳动（见 `HardwareManager::NormalizeRotationAngle` 声明处注释）。`ResolveRunnerJoints` 在 IK **之前**会对 R 做 `NormalizeRotationAngle` 归一化，把 wrap 值拉回 `[limitMin, limitMax]` 中心 ±180° 窗口——R 域 `[0, 185]` 下窗口为 `[-87.5, 272.5]`，故 `-179.9` 会被归一化为 `180.1`。用 179.9 比 180 离回绕奇点稍远，读数更稳。方案中现有示教点用的是 `179.9` / `180.1`，`kinematics.safePos.r` 为 `179.5`。

## 八、改动清单

| 文件 | 改动 | 落地（2026-09-20） |
| :--- | :--- | :--- |
| `src/Config/ProcessManager.h` | `ActionType` **末尾**追加 `VisionGuide`；`ActionData` 加 `id` / `sourceActionId` / `offsetZ` / `guideR` | ✅ `ActionType` 与 `id` 随 P2（TR-098）；`sourceActionId` / `offsetZ` / `guideR` 随 P5（TR-101） |
| `src/Config/ProcessManager.cpp` | `actionTypeName` 加 `VisionGuide → "视觉引导"`；`process.json` 读写加新字段；加载时做 `id` 迁移 | ✅ 命名与 `id` 迁移随 P2；三个新字段的读（`aj.value(...)` 带默认值）与写随 P5 |
| `src/Logic/SequenceWorker.h/.cpp` | `ExecuteAction` 加分支；识别分支写入结果表；新增视觉引导执行路径；`RunSequence` 清空结果表 | ✅ **不新增独立执行函数**——P6 后所有动作共用 `PrepareRunnerSegment` / `ResolveRunnerJoints` / `DispatchRunnerSegment`，视觉引导落在 `PrepareRunnerSegment` 的 `case ActionType::VisionGuide`；`StepRunner` 的 `case ActionType::Vision` 末尾 `visionResults_.insert(action.id, robot)` + `emit visionResultReady`；**两个会话入口**（`RunSequence` **与** `RunSingleAction`）都 `clear()` |
| `src/UI/ProcessPage.cpp/.h` | 动作类型下拉加「视觉引导」；两处 `switch (node->type)` 加分支；视觉引导编辑区（引用编号 + `offsetZ` + `guideR` + 速度 + **两组坐标只读预览**）；列表显示 `#N` | ✅ 类型下拉与列表 `#N` 随 P2；编辑区 + `RefreshGuideSourceCombo` + `UpdateGuidePreview`（四态）+ 编辑期校验随 P5 |
| `config/process.json` | 旧数据补 `id`（加载时自动迁移） | ✅ 随 P2（**惰性升级**：只加载不编辑不写盘）。**P5 不新增任何键**——三个新字段只在编辑保存后写入，**识别坐标永不入文件** |
| `doc/process.md` | schema 同步 | ✅ 2026-09-20 同步（`ActionData` 三字段 + 新增「视觉引导动作（type=5）」章节） |

## 九、已确认决策与遗留

### 9.1 已确认（用户 2026-09-17）

| # | 决策 |
| :--- | :--- |
| 1 | **两组坐标在 UI 上可见、只读**（不可编辑——坐标由识别结果决定） |
| 2 | **`sourceActionId` 不允许跨方案引用**，仅同方案内 |
| 3 | **`offsetZ` 按末端高度（`Pose.z`）算，正值向上**，与 `PointData.j3`（Z 电机坐标）严格区分、不可混用 |
| 4 | **TCP 由运动学内化处理**：视觉坐标直接作为 `Pose` 输入 `Inverse`，视觉引导侧不重复做 TCP 变换（§5.6） |
| 5 | **识别坐标不写入 `process.json`**；日志已可追溯（§5.5） |

**以上 5 项已于 2026-09-20（TR-101）全部落地**；决策 3 的 `offsetZ` 语义与决策 5 的「坐标不入文件」在 `doc/process.md` 有独立章节。

### 9.2 遗留（待确认）

| # | 待确认项 | 影响 |
| :--- | :--- | :--- |
| 1 | **相机安装参数与基座标的关系标定**（手眼矩阵）——现行 `tcpCalibration.handEyeMatrix` 是占位值 | 标定完成前，视觉引导坐标**只能用于仿真验证**，真机不可用 |
| 2 | **视觉返回点与夹爪尖端的对应关系**：算法返回的是泡芙的哪个部位？与夹爪要到达的位置是否一致（是否需工艺偏移量） | 决定是否要在视觉引导里加额外的 Z 偏移参数 |
| 3 | ~~**`guideR` 默认值取 179.9° 还是 180°**~~ **已于 2026-09-20 按 179.9° 落地**（TR-101） | 本文按 179.9°（离回绕奇点稍远，读数更稳）；若工艺要求严格 180°，改默认值即可 |
| 4 | **识别结果是否需要结构化持久化**（CSV / JSON 运行记录） | 当前仅日志可追溯；若有质量分析需求需另建运行记录文件（**不混入 `process.json`**） |

## 十、验收

| 项 | 判据 | 落地情况（2026-09-20，TR-101） |
| :--- | :--- | :--- |
| 编辑期校验 | 保存引用不存在编号的动作 → 提示并拒绝 | ✅ 未选择 / 悬垂引用 / 跨方案**三种都拒**（用例 12.8~12.10） |
| 执行期校验 | 无识别结果 → 明确报错，**不产生运动** | ✅ 两条失败路径都**先于任何运动**返回（用例 12.11 / 12.12） |
| 顺序 | 日志可验证「先上方、后目标」 | ✅ 程序日志 `VisionGuide #N ← Vision #M: base(…) offsetZ=… r=…`（取基准坐标时只打一次）+ 两条 `IK → J(…)`（**Z 差 == `offsetZ`**）+ 第 2 段**逐轴** `skip axis J1 (in position)`（顺序 J2 → R → J1 → Z）；页面日志另有 `视觉引导：引用 #M → 目标上方 (…) → 目标 (…)`（用例 12.13） |
| R 角 | 构造的路径点 `r` = `guideR` 固定值，**与 `PuffResult.yaw` 无关**（日志可验证） | ✅ 日志显式留证 `[node #M yaw=… ignored]`（用例 12.14） |
| 循环生产 | 每轮重新识别、结果覆盖正确 | ✅ 结果表按 `id` 覆盖：`RunSequence` 入口清表一次，此后每轮识别动作覆盖同 `id`（用例 12.18） |
| Sim 全链路 | `SimCamera` + `SimAlgo` 可跑通（`SimAlgo` 的 `yaw` 恒 0，不影响功能验证） | ✅ 代码已就位；**待 B 档用例实跑取证**（12.7~12.21） |
| 编译 | Debug + Release 双编译 EXITCODE=0 + Sim 冒烟 `Initialize complete` | ✅ Debug `[47/47]` / Release `[15/15]` `EXITCODE=0`（零新警告）；Sim 冒烟 `BEFORE=10 → AFTER=11` |

**⚠ 真机验收仍待人工**：视觉引导抓取落点与重复定位精度（用例 12.22 / 12.23）**须先完成手眼矩阵标定**（§9.2 遗留 1）——标定前坐标只能用于仿真验证。用例全文见 `doc/test/real_machine_plan_phase3.md` **阶段 12（12.1~12.26）**。
