# 动作树与并发分组方案

> **需求来源**：用户 2026-09-18 提出——动作列表由**一维列表**改为**二级树**（节点类型：动作 / 分组），分组内子动作**并发执行**以压缩节拍，每个动作含独立编号。用户同时提供了 Gemini 的实现方案，本文对该需求与方案一并评审。
> **本文性质**：**方案 → 🟢 三步全部已实施**（2026-09-20）。**第一步「编号 + 数据模型」已落地**（P2，TR-098）；**第二、三步（UI 树化、执行层并发）已落地**（P6，TR-102）。文中现状事实均带 `file:line`，核实时点为 2026-09-18，可用源码复核。
> **需求状态**：§二 的 5 项补充定义**已由用户于 2026-09-18 裁定**（见 §二 表），技术路线已定为**单线程协作式调度**。实施顺序见 `doc/implementation_roadmap.md` 的 **P2（编号机制）** 与 **P6（UI 树化 + 并发）**。
> **关联文档**：`doc/process.md`（`process.json` schema）、`doc/architecture.md` §3.2 与 §5（执行引擎线程模型与关键不变量，**改 `SequenceWorker` 前必读**）、`doc/auto_run_loop_production.md`（循环生产 TR-091，§十 涉及其交互）、`doc/vision_guide_action_plan.md`（动作编号机制的前置，§七 与其对齐）、`doc/config_ownership_plan.md`（配置读写现状）。

## 一、需求判定与结论

### 1.1 需求判定

| 需求点 | 判定 | 说明 |
| :--- | :--- | :--- |
| 动作列表改为二级树（动作 / 分组） | **合理** | 一级节点 + 分组内一级子节点，即**不允许嵌套**。二级结构足以覆盖工业现场的节拍优化场景，且避免了递归执行的复杂度 |
| 分组内子动作并发执行 | **合理，且有真实收益** | 见 §五。收益来源是**等待时间的重叠**，不是计算并行 |
| 每个动作含独立编号 | **合理，且是前置条件** | 现状的「编号」是列表行号（`ProcessPage.cpp:674` 的 `idx + 1`），**移动或删除动作后会漂移**。`doc/vision_guide_action_plan.md` 的「视觉引导」动作需要按编号引用前序「识别」动作，编号必须持久化且稳定 |
| 组内禁止多个「移动」动作 | **成立** | 见 §六。但规则应从「类型互斥」升级为「资源互斥」 |

### 1.2 结论（技术路线）

**不采纳 Gemini 推荐的多线程方案（`QtConcurrent::run` + `QFutureSynchronizer`）。** 该方案在本项目会破坏 4 条既有前提（见 §四），且收益并不优于单线程方案。

**建议改为单线程协作式调度**：把 5 个动作函数由「阻塞式」改为**可步进的状态机**，由 `SequenceWorker` 的单线程调度器推进；分组 = 调度器同时持有 N 个子状态机，全部完成即汇合。理由见 §五。

## 二、需求补充定义（5 项，**已裁定**）

以下 5 项需求原文与 Gemini 方案均未涉及，但**不定义就无法实施**。**2026-09-18 用户确认按本表实施**，此表即约束，实施时不得再自行取舍。

| # | 项 | **裁定** | 落地位置 |
| :--- | :--- | :--- | :--- |
| Q1 | **「并发」的语义** | **同时下发、统一汇合**。组内所有子节点先各自完成「下发」，再统一等待全部到位；语义比「各自等待」简单，节拍收益相同 | §五 调度器；§九 第 3 项 |
| Q2 | **分组失败语义** | 置 `cancel`，**等待所有子动作安全停止后再报错**。**禁止立即返回**——否则硬件仍在运动，退出即失控 | §九 第 4 项 |
| Q3 | **分组 × 循环生产（TR-091）** | `LoopConfig.retryCount` 对分组 = **重试整个分组**。部分重试会破坏组内资源独占（一个子动作已走完、另一个重试 → 状态错配） | §九 第 1 项（循环外壳位置不变） |
| Q4 | **编号的分配规则** | 方案内自增（当前最大值 + 1）、**删除不复用**、**跨方案不唯一**（与「`sourceActionId` 不跨方案」一致）。**不用 UUID 或时间戳** | §7.2 |
| Q5 | **分组能否嵌套** | **不能**。写死两级；**数据校验与 UI 两处都要拦** | §八 第 5 项（防呆落点 `ProcessPage.cpp:1176`） |

**与 `doc/vision_guide_action_plan.md` 的对齐**：Q4 的编号规则是**两篇文档的共享前置**——本方案的节点 `id` 与视觉引导的 `sourceActionId` 必须**同一套编号**，一次设计、两处引用。实施时以 §7.2 为准，勿在另一篇文档里另立一套。详见 `doc/implementation_roadmap.md` 的 P1 阶段。

## 三、Gemini 方案的事实错误

| # | Gemini 的说法 | 事实 | 影响 |
| :--- | :--- | :--- | :--- |
| **E1** | 「你原本的 `config.json` 里，方案下直接就是一个一维的 `actions` 数组」 | 方案与动作在 **`config/process.json`**（结构为 `{"schemes":[{"actions":[…]}]}`），**不在 `config.json`**。`config.json` 存放硬件、运动学、标定与视觉配置 | 照此实施会改错文件。两个文件的职责边界见 `doc/config.md` 与 `doc/process.md` |
| **E2** | 「如果在移动的过程中拍照识别，**照片是糊的**！识别肯定会失败」 | 本项目相机为**眼在手外（Eye-to-Hand，固定安装）**（2026-09-17 用户确认）。**机械臂运动不会造成图像模糊** | 论据不成立。真实风险是另外两条：① 机械臂进入视野造成**遮挡**；② 识别结果与移动目标的**时效错配**（见 §6.4） |
| **E3** | 「当用户选中一个分组，点击【新建动作】弹出 `ActionEditDialog` 时…」 | **项目中不存在 `ActionEditDialog`**。实际实现是 `OnNewAction`（`ProcessPage.cpp:1155`）内联构造的 `QDialog` + `QComboBox`，动作类型项在 `:1176` **硬编码 5 项** | 防呆逻辑的落点描述错误。实际落点只有一处：`:1176` 的 `typeCombo` 构造（见 §八） |

## 四、Gemini 方案的重大遗漏

以下 4 项均为**会导致崩溃或静默错误**的问题，Gemini 方案未涉及。

### 4.1 `QtConcurrent` 会破坏 `Impl::lastError` 的线程独占前提

`SequenceWorker.cpp:113-116` 的注释明写：

> 失败原因（D17 方案 A 单点化）：`ExecuteAction` 子树只写此字段，`emit errorOccurred` 收敛到会话级 2 处出口。**仅 worker 线程内写/读，不跨线程共享 → 无需加锁。**

这是 TR-091 / D17「`errorOccurred` 单点化」的**地基**（见 `doc/architecture.md` §5）。改用 `QtConcurrent` 后，N 个池线程会**并发写同一个 `QString`**（隐式共享 + 引用计数），构成数据竞争，可导致堆损坏。

**必须处理**：改为每线程独立错误槽 + 汇合时择一上报，或对该字段加锁（但加锁后 §5 的单线程方案就失去意义了）。

### 4.2 线程归属脱离 `ShutdownWorker` 的管理范围

`ShutdownWorker`（`MainWindow.cpp:298-320`）的管理对象是**一个** `QThread`（`workerThread_`，`MainWindow.cpp:238-243`）：`quit()` → `wait(20)` 轮询 → 超时 `terminate()`。

`QtConcurrent::run` 使用的是**全局线程池**，池线程**不属于 `workerThread_`**，`terminate()` 无法回收它们。

**真实风险**：`ConsoleSignalHandler`（`main.cpp:36-46`）运行在**独立的控制台信号线程**上，直接调 `HardwareManager::ShutdownHalt()`，且该函数在 `HardwareManager.cpp` 中**无任何加锁**（见 4.4）。池线程此时仍可能正在下发运动指令 → 与关断使能构成竞争。

> **修正记录（2026-09-18 复核）**：本节初稿曾写「`ShutdownWorker` 的 `wait(20)` 循环不泵主线程事件循环，因此池线程的 `InMainThread` 无法被服务」——**该说法错误**。`MainWindow.cpp:313-316` 的循环内**显式调用了** `QCoreApplication::processEvents(QEventLoop::AllEvents, 20)`，且函数上方注释写明「否则 worker 的 BlockingQueuedConnection（InMainThread）会因主线程不再跑事件循环而永久阻塞 → 挂死」。故池线程的 `InMainThread` 在退出阶段**是被服务的**，不会退化为超时强杀；风险只剩「池线程不被 `terminate()` 回收」这一条。

### 4.3 `InMainThread` 的阻塞往返会被放大 N 倍

`WaitForAxes`（`SequenceWorker.cpp:812-835`）是 `QThread::msleep(20)` 轮询，**每个轴每 20 ms 一次**：

```cpp
for (auto a : axes) {
    if (InMainThread([&] { return hw.IsAxisBusy(a); })) { allDone = false; break; }
}
```

`InMainThread`（`SequenceWorker.cpp:27-37`）走 `QMetaObject::invokeMethod(..., Qt::BlockingQueuedConnection)` **阻塞转发到主线程**。

4 个并发子动作 × 2 个逻辑轴 × 50 Hz ≈ **400 次/秒的阻塞往返**，全部落在主线程（UI 线程）上。主线程同时还要绘制四宫格画面（`VisionTestPage`），会出现明显卡顿。

### 4.4 硬件层加锁覆盖不全，Gemini 的落点描述有误

| 层 | 现状 | 说明 |
| :--- | :--- | :--- |
| `HardwareManager` | **零个 `lock_guard`**（`HardwareManager.cpp` 全文 0 处） | 它的设计意图是「只从主线程调用」——这正是 `InMainThread` 存在的原因。Gemini 说「`HardwareManager` 里所有的 API 内部必须加锁」，方向对但**落点错** |
| `BoPaiCard` | 有 `mutable std::mutex mutex_`（`BoPaiCard.h:68`），但 30 个函数定义中**仅 18 个加锁，12 个未加锁** | 未加锁的 12 个：`SetHost`（`:90`）、`IsConnected`（`:110`）、`PulsePerUnit`（`:118`）、`Cfg`（`:141` / `:147` 两个重载）、`AccelToPulse`（`:153`）、**`RefreshStatus`（`:164`）**、**`MoveLinear`（`:419`）**、`SetSpeed`（`:466`）、`SetAccel`（`:475`）、`SetAxisConfig`（`:504`）、`GetLastError`（`:552`）。其中两条最危险：① **`RefreshStatus` 未加锁写 `impl_->sysStatus`**，而 `GetAxisStatus`（`:517`）/ `GetAllStatus`（`:525`）**加锁读**同一数据 → 锁形同虚设；② **`MoveLinear` 是「循环调 `MoveAbs`」**（`MoveAbs` 自身加锁），故 N 轴下发**不是一次原子操作**，两个并发线程的 `MoveLinear` 可逐轴交错 |
| `XRServo` | 有 `g_serialMutex`（`XRServo.cpp:27`），但只护了 3 处（`:106` / `:282` / `:355`） | 覆盖不完整 |
| 构建 | **`Qt6::Concurrent` 当前未链接** | 根 `CMakeLists.txt:123-124` 只链了 `Qt6::Core` 与 `Qt6::Widgets`。Gemini 未提这个构建改动 |

## 五、技术路线：单线程协作式调度

### 5.1 关键判断

**并发分组省的是「等待时间」，不是「计算时间」。而等待不需要线程——只需要「不阻塞地下发」。**

现状 `ExecuteMove` 的结构是**逐点交替**：`DispatchPointMove`（非阻塞下发）+ `WaitForAxes`（阻塞等待）。顺序执行之所以慢，是因为下发与等待交替：

```
下发 Move(Z) → 等 Z 到位 → 下发 Gripper → 等 Gripper 到位        总时长 = Z + G
```

把「下发」与「等待」拆开后，分组可以：

```
下发 Move(Z) → 下发 Gripper → 执行 Vision → 等待 {Z, G} 全部到位   总时长 = max(Z, G) + V
```

**被省下的正是重叠掉的那部分等待**，而这通常是节拍的大头。

### 5.2 协作式调度的模型

把 5 个动作函数（`ExecuteMove` / `ExecuteVision` / `ExecuteExtrude` / `ExecuteDelay` / `ExecuteGripper`）由「阻塞式函数」改为**可步进的状态机**：

```
每个子动作暴露：Step() -> Progress   // 推进一次（可下发、可检查、可等待）
                IsDone() -> bool     // 是否已完成
```

调度器在 worker 线程内单线程轮询：

```
分组执行：
  1. 为每个子节点创建一个子状态机，全部入队
  2. 循环：对所有未完成的子状态机调用 Step()
  3. 全部 IsDone() → 汇合完成，继续下一个一级节点
```

**多点点位「移动」的并发**：子状态机自己按点位推进，`doc/architecture.md` §5 的「严格先抬 Z 到位再做水平移动」（TR-091 / D19）不变量**在每个子状态机内部继续成立**，不受并发影响。

### 5.3 两种路线的代价对比

| 项 | Gemini 路线（`QtConcurrent` 多线程） | 建议路线（单线程协作式） |
| :--- | :--- | :--- |
| 节拍收益 | 等待重叠 | 等待重叠（**相同**） |
| `Impl::lastError` | 需加锁或重构 | **不动** |
| 线程归属 / 退出回收 | 需新增池线程回收机制（§4.2） | **不动** |
| 主线程阻塞往返 | ×N（§4.3） | **不变** |
| 硬件层加锁 | 必须补齐 `BoPaiCard` 12 处 + `XRServo` | **不需要**（仍单线程） |
| `PauseGate` / `cancel` / `Stop` / `StopImmediate` 语义 | 分散到 N 个线程，需重新论证 | **原样保留** |
| 单步执行语义 | 组内 `stepGo` 被 N 个线程争抢，顺序不确定 | 可精确控制（组作为一步） |
| 构建改动 | 需链 `Qt6::Concurrent` | **无** |
| 主要工作量 | 线程安全加固 + 调度 | 5 个动作函数改状态机 |

### 5.4 何时才真需要多线程

当分组内出现「CPU 密集且需要真并行」的动作时（例如高帧率图像处理、多相机同时采集）。当前 `Vision` 是同步检测（数十 ms 量级，`SimAlgo::Detect`），单线程足够。

**结论：多线程不是本次要解决的问题。** 若将来确实需要，应作为独立议题单独论证，而不是随本次改造一并引入。

## 六、组内冲突校验：按资源，不按类型

### 6.1 用户判断成立

「组内禁止多个移动动作」的判断正确：`Move` 动作内部就是 `(x, y, z, r)` 四轴联动（`PointData` 的 `coord`），需要同时动 X 与 Z 时**在同一个 `Move` 动作里配好即可**，两个并发的 `Move` 会向运动控制卡下发互相矛盾的绝对目标。

### 6.2 规则应从「类型互斥」升级为「资源互斥」

Gemini 把它推广为「组内每种动作类型最多 1 个」。方向正确，但粒度应调整：

**按类型互斥 ≈ 按资源互斥**——当前两者恰好 1:1：

| 动作类型 | 占用的资源 |
| :--- | :--- |
| `Move` | `Axis_J1` / `Axis_J2` / `Axis_Z` / `Axis_R` |
| `Gripper` | `Axis_Gripper` |
| `Extrude` | `Axis_Extruder` |
| `Vision` | 相机（见 6.3） |
| `Delay` | **无硬件资源** |

一旦将来出现复合动作类型（如「夹爪 + 挤出」合并为一个动作），按类型的规则就会失效。**建议在数据里显式声明每个动作类型的资源占用集合，校验用集合求交**——规则可扩展，且校验代码只有一处。

### 6.3 Gemini 两处论据需修正

| 项 | Gemini 的说法 | 修正 |
| :--- | :--- | :--- |
| `Vision` 的冲突原因 | 「独占相机硬件，并发抢占会导致 SDK 崩溃」 | **不成立**。相机流已定为**持有者集合**模型（`doc/camera_integration_plan.md` §2.1），流本身就是多持有者共享的；`Vision` 动作只需从最新帧槽取一帧，不「独占相机」。真正该禁「同组两个 `Vision`」的理由是**语义**：两个检测结果由谁消费，现行无法表达 |
| `Delay` | 「允许同组有多个，或者 1 个」 | **不应限制**。`Delay` 不占硬件资源，多个并发等价于取最大值，语义清晰且无害。把「类型互斥」机械地套到 `Delay` 上属于过度约束 |

### 6.4 另需新增一类校验：数据依赖

资源冲突之外还有**数据依赖冲突**，Gemini 未提：

**`Vision` 不能与「消费它结果的那个 `Move`」同组。**

用户举的例子「移动 + 打开夹爪 + 识别」需要澄清语义：若该 `Move` 的终点来自该 `Vision` 的识别结果（即「视觉引导」），则两者并发在逻辑上自相矛盾——移动的目标尚未算出。这类冲突不是资源抢占，而是**数据依赖**，需要在 UI 校验中一并拦截。

建议规则：**同组内若存在 `Vision` 与「引用该 `Vision` 编号的 `VisionGuide` 动作」，拒绝配置。**

## 七、数据模型

### 7.1 节点类型不进 `ActionType` 枚举

**`Group` 不能作为 `ActionType` 的第 6 个值。** 三条理由：

1. `ActionType` 在 `process.json` 里是**整数持久化**的（现有 `type` 为 0~4），而新增值**必须追加在末尾**（见 `doc/vision_guide_action_plan.md` §三）。
2. 已规划的 `VisionGuide` 已预定为 `5`。若 `Group` 也取 `5`，两者冲突。
3. 类型下拉框的索引**直接就是枚举值**：`a.type = static_cast<ActionType>(typeCombo->currentIndex())`（`ProcessPage.cpp:1200`）。把 `Group` 塞进这个下拉框会立即污染类型映射。

**做法**：`Group` 用**独立的 `nodeType` 字段**表达（`"action"` / `"group"`），与动作类型正交。

### 7.2 编号规则：方案内唯一整数，不用 UUID

Gemini 建议「必须给每个节点生成全局唯一的 ID（比如时间戳或 UUID）」，与用户 2026-09-17 已裁定的「动作加明确编号」冲突——编号需要**人可读**（UI 显示 + `sourceActionId` 引用），UUID 与时间戳两者都不适合。

| 项 | 建议 |
| :--- | :--- |
| 编号类型 | **方案内唯一的正整数** |
| 分配 | 方案内自增（取当前最大值 + 1） |
| 删除 | **不复用**（避免旧引用指向新动作） |
| 作用域 | **方案内唯一，跨方案不唯一**（与「`sourceActionId` 不允许跨方案引用」一致） |
| 分组是否占编号 | **占**。分组也是一级节点，UI 上需要显示，且需要被引用定位 |

> **✅ 已实施（2026-09-20，P2 / TR-098）**——「分配」一项按上表「自增」的字面含义实现，但**必须补一句上表没写的**：「取当前最大值 + 1」**单独用不足以满足同表的「删除不复用」**。删掉当前最大编号的动作后再新增，`max(id)+1` 会**复用刚被删除的编号**，使既有的 `sourceActionId` 引用静默指向新动作。故实际实现是**持久化计数器** `SchemeData::nextActionId`（**单调递增，删除不回退**），新增动作时 `allocateActionId()` 取该值并推进；计数器随 `process.json` 落盘。这同时为第二步的「分组也占编号」留好了同一分配入口。

### 7.3 `process.json` 向后兼容

现有文件结构为 `{"schemes":[{"actions":[…]}]}`，扁平数组。

**建议**：读取时**自动升级**——`actions` → 同级 `nodes`，每个动作补 `id`（按数组顺序分配），首次保存写回新结构。**不要要求用户手工迁移**，否则现有工艺方案会全部失效。

> **✅ 已实施的部分（2026-09-20，P2 / TR-098）**：**`id` 的自动升级已落地**——加载时 `id` 缺失/非正/重复一律按「方案内最大编号 + 1」依次补发（`ProcessManager::normalizeActionIds()`，**幂等**），旧 `process.json` 的 `actions` 数组按**列表顺序**补成 `1..N`，**不要求用户手工迁移**；首次保存写回 `"id"` 与 `"nextActionId"`。**升级是惰性的**：只加载不编辑不会写盘。
>
> **⚠ 键名 `actions` → `nodes` 的改名与 `nodeType` 字段未在 P2 落地，是有意推迟到第二步的**——理由：① 第二步才会引入分组节点，此时 `nodes`/`nodeType` 才有对应实体；② 现在改键名而 `SchemeData` 的成员仍叫 `actions`，会造成「文件结构与内存模型不同名」的中间态，第二步还要再动一次；③ P2 的既定边界是「引擎零改动、小且低风险」，改键名属结构性变更。**第二步落地时必须同时完成**：`actions` → `nodes`、加 `nodeType`、旧键读取兼容（读 `actions` 或 `nodes` 都接受）。

## 八、UI 改造清单（`ProcessPage`，1527 行）

| # | 项 | 现状 | 改动 |
| :--- | :--- | :--- | :--- |
| 1 | 动作列表控件 | `QListWidget`（`ProcessPage.cpp:237`） | → `QTreeWidget` |
| 2 | 选中变化信号 | `QListWidget::currentRowChanged`（`:249`） | → `QTreeWidget::currentItemChanged` |
| 3 | **当前节点标识** | `m_currentActionIdx` 为**扁平行号 `int`**，用于 `:252` / `:642` / `:651-658` / `:674-675` / `:794-798` / `:1201` | → 节点标识（编号）；**全部调用点改写**。这是本页改动量最大的一项 |
| 4 | 当前动作标签 | `"当前动作: [%1] %2"`，`%1 = idx + 1`（`:674`）——**编号即行号** | → 显示持久化编号（§7.2） |
| 5 | 新建动作 | `OnNewAction`（`:1155`）内联 `QDialog` + `QComboBox`，类型项**硬编码 5 项**（`:1176`） | ① 新增「新建分组」按钮；② **防呆落点就在 `:1176` 这一处**——按父节点已占用的资源集合，对冲突类型置灰 |
| 6 | 插入位置 | `insertPos = m_currentActionIdx + 1`（`:1201`）——扁平插入 | → 按选中节点在树中的位置插入（同级之后 / 分组内末尾） |
| 7 | 上移 / 下移 | `OnMoveUp` / `OnMoveDown`（`:387-388`） | 限定**同级移动**；跨组移动需校验资源冲突与「分组不能嵌套」 |
| 8 | 详情面板 | `m_detailStack`（`QStackedWidget`，`:340`），5 个面板 | 新增「分组」页（仅名称与编号） |
| 9 | 执行选中动作 | `m_worker->RunSingleAction(scheme, row)`（`:1144`），`row` 为扁平行号 | → 传节点标识；`SequenceWorker::RunSingleAction` 签名同步修改 |
| 10 | 单步信号 | `actionStarted` / `actionFinished` 的 lambda（`:919-931`），形参为 `int i` | → 携带节点标识与「分组/动作」区分 |
| 11 | `AutoRunPage::OnActionStarted` | 形参 `int /*index*/` **已忽略索引**，只用 `name`（`AutoRunPage.cpp:639`） | **无需改动** ✓ |

## 九、执行层改造清单（`SequenceWorker`，1010 行）

| # | 项 | 现状 | 改动 |
| :--- | :--- | :--- | :--- |
| 1 | 执行入口 | `ExecuteActions`（循环外壳）→ `ExecuteOnce`（单轮）→ `ExecuteAction`（逐动作） | 在 `ExecuteOnce` 与 `ExecuteAction` 之间插入 `ExecuteNode`：按 `nodeType` 分派到 `ExecuteAction`（动作）或 `ExecuteGroup`（分组）。**循环外壳位置不变**（TR-091 约定：循环外壳在会话内） |
| 2 | 5 个动作函数 | `ExecuteMove` / `ExecuteVision` / `ExecuteExtrude` / `ExecuteDelay` / `ExecuteGripper`，**阻塞式** | → 改为可步进状态机（`Step()` / `IsDone()`），供调度器推进（§5.2） |
| 3 | 分组调度 | 不存在 | 新增单线程调度器：持 N 个子状态机，全部完成即汇合 |
| 4 | 失败上报 | 子树写 `Impl::lastError`，会话级 2 处 `emit`（D17） | **保持单线程即可不改**；但分组需定义「哪个子动作的失败原因上报」（建议：首个失败者，其余记日志） |
| 5 | 单步 | `WaitForStep()`（`:138-152`）挂在每个动作边界 | 分组的「一步」= 整个分组；**组内一律不调用 `WaitForStep`**（否则 N 个子状态机争抢同一个 `stepGo` 原子量，放行顺序不确定） |
| 6 | 暂停 | `PauseGate`（`:837-`）在 `WaitForAxes` 内触发 | 组内任一子状态机触发暂停 → **整组减速停** → 汇合后统一挂起 |
| 7 | 信号 | `actionStarted(int index, name)` / `actionFinished(int index, name)` | 索引语义由「扁平行号」改为「节点标识」；分组需新增「分组开始/结束」信号，供 UI 展示折叠进度 |

## 十、实施顺序建议（三步，每步可独立验收）

**把「树形」与「并发」拆开做**，是本次改造最重要的工程决策：若一次性完成，出问题时无法判断是数据结构错还是并发错。

| 步骤 | 内容 | 执行引擎改动 | 验收方式 |
| :--- | :--- | :--- | :--- |
| **第一步** ✅ **2026-09-20（P2，TR-098）** | **编号 + 数据模型**：`ActionData` 加 `id`；`process.json` 加读取兼容与自动升级；`actionTypeName` 等 5 处同步（见 `doc/vision_guide_action_plan.md` §三） | 无 | Sim 冒烟 + 现有方案加载后编号正确、工艺行为不变 |
| **第二步** ✅ **2026-09-20（P6，TR-102）** | **UI 树化**：`QListWidget` → `QTreeWidget`；新增分组节点与资源冲突防呆校验；`m_currentActionIdx` 全部调用点改写。**分组此时按「顺序执行其子节点」处理** | 仅新增 `ExecuteNode` 分支，分组退化为顺序容器 | Sim 冒烟 + 分组顺序执行正确 + 防呆拦截生效 |
| **第三步** ✅ **2026-09-20（P6，TR-102）** | **执行层并发**：5 个动作函数改状态机 + 单线程调度器，启用真正的组内并发 | 主要工作量所在 | Sim 冒烟 + 并发组节拍实测短于顺序执行 + 暂停/停止/急停/单步在组内均正确 |

**第一步落地情况（2026-09-20，P2）**：编号计数器落在 `SchemeData::nextActionId`（**单调递增、删除不回退**，方案原文的「取当前最大值 + 1」不足以满足同表裁定的「删除不复用」——删掉最大编号后现算会复用该编号）；`normalizeActionIds()` 在加载时对缺失/非正/重复编号按「最大编号 + 1」依次补发，**幂等**；`save()` 落盘 `id` 与 `nextActionId`。**升级是惰性的**——只加载不编辑不写盘。`VisionGuide = 5` 已按 §7.1 追加在枚举末尾，类型下拉与 `actionTypeName` 同步；`Group` 仍未进枚举（留待第二步引入独立 `nodeType` 字段）。**UI 侧提前落实了 §八 第 4 项的一半**：动作列表与「当前动作」标签已显示**持久化编号** `#N`（行号已不再是编号）；`m_currentActionIdx` 仍为扁平行号，其调用点改写留第二步。

**第二步完成后数据结构与 UI 已就绪，分组就是「一个可折叠的顺序容器」；第三步只换调度器。** 这样每一步的风险都可独立定位。

**第二、三步落地情况（2026-09-20，P6 / TR-102）**：`process.json` 的 `actions` → `nodes` 改名与 `nodeType` 字段**与旧键读取兼容同批落地**（§7.3 的既定条件已满足）；`QListWidget` → `QTreeWidget`、`m_currentActionIdx`（43 处）→ `m_currentNodeId`（节点编号）；分组新增独立详情页（stack 索引 7）；组内校验按**资源集合求交**（`resourceKeysOf` + `validateGroupChildren`），并**新增数据依赖一类**（§6.4）。**⚠ 一处刻意偏离本文 §十 第二步的表述**：原文写「分组此时按『顺序执行其子节点』处理」，实际实现是**单动作与分组共用同一个调度器**（`RunScheduler`，单动作即 1 元素 `runners`）——若分组在第二步真的按顺序执行状态机，会**阻塞调度器**、把第三步要拿的「等待重叠」收益整个丢掉；共用同一实现也让暂停 / 停止 / 续走只有**一份语义**。第三步因此不是「换调度器」而是「补齐状态机 + 启用调度器」。


## 十一、验收与台账

- 每步完成后 **Debug + Release 双编译**（`EXITCODE=0`）+ **Sim 冒烟**（判据 = 当日日志含 `Initialize complete`）。
- 台账从 **TR-097** 起（当前最大 `TR-096`；注意 `camera_integration_plan.md` 与 `config_ownership_plan.md` 也预定了 TR-097 起，**实际编号按落地顺序分配**）。
- 真机项（须真机执行，Sim 不可替代）：并发组的节拍实测对比、组内暂停/停止/急停的实际停止行为、多轴同时运动时的机械干涉检查。
- 须同步的文档：`doc/process.md`（schema 变更）、`doc/architecture.md` §3.2 与 §5（执行引擎模型与不变量）、`AGENTS.md`（UI Pages 节）。
- **✅ 已补（2026-09-20，P6 / TR-102）**：`doc/architecture.md` §5 的「`errorOccurred` 单点化」条目已写明分组语义——调度器**不新增任何 `errorOccurred` emit**，失败原因仍只写 `Impl::lastError`；分组的上报规则是**首个失败者**（其余子动作的失败仅记日志），且调度器用独立的 `aborting` 标记把「内部失败中止」与 `cancel`（用户停止 / 急停）分流，避免动作失败被误报成「用户停止」。单线程路线下**不需要加锁**。
