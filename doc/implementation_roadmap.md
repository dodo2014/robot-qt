# 五篇需求方案的实施顺序

> **本文性质**：**跨文档的实施编排（⏳ 计划中）**，不是新需求。职责只有一个——给下列 5 篇方案定**先后与并行边界**。各方案的内部细节一律以各自文档为准，**本文不重复维护**。
> **核实时点**：2026-09-18（P1~P5 状态更新于 2026-09-20）。台账当前最大 **TR-102**。
> **关联**：`doc/README.md`（索引与状态）、`doc/joint_drift_analysis.md`（P0 阻塞项）、`TEST_RECORD.md`（记账）。

## 一、结论

```
P0  J1 累积偏移        ← ✅ 已缓解（2026-09-18 真机 247 轮无可见漂移），不再是阻塞项
────────────────────────────────────────────────
P1  配置地基           config_ownership_plan      （C1~C5 + save() 原子写）  ← ✅ 2026-09-20（TR-097）
P2  编号机制           action_tree 第一步 ∩ vision_guide 编号部分   ← ✅ 2026-09-20（TR-098，一次设计）
P3  相机 HAL           camera_integration_plan   （S0~S4）                  ← ✅ 2026-09-20（TR-099，真机枚举 + 取流 + Release 60 s 长稳已验）
P4  视觉页重构         vision_test_page_redesign_plan                            ← ✅ 2026-09-20（TR-100，Sim 冒烟通过；真机三项待人工）
P5  视觉引导动作       vision_guide_action_plan                                ← ✅ 2026-09-20（TR-101，代码 + 双编译 + Sim 冒烟；真机待人工）
P6  动作树 UI + 并发   action_tree_concurrency_plan（第二、三步）              ← ✅ 2026-09-20（TR-102，代码 + 双编译 + Sim 冒烟 + 离线自测；真机待人工）
```

**一句话理由**：**先修「让相机根本连不上」的配置断链（P1），再把两篇文档共享的编号机制一次定稿（P2），然后沿「相机 HAL → 视觉页 → 视觉引导」这条验证链推进（P3→P4→P5），最后做与相机链路正交的动作树改造（P6）。**

## 二、排序的三个硬约束

### 约束 1：依赖（谁不先做，后面就做不了）

| 被依赖项 | 依赖方 | 不做的后果 |
| :--- | :--- | :--- |
| `config_ownership_plan` 的 **C1 / C2 / C5** | `camera_integration_plan` | **相机设备标识两个键互不相通**（`communication.camera.sn` 无读点、`CameraManager.cpp:42-43` 读的 `simulation.cameraDeviceId` 在 config 中不存在、`VisionTestPage.cpp:462` 又无条件覆盖用户输入）→ **真机连不上相机**，P3 之后全部无法真机验证 |
| `config_ownership_plan` 的 **C1**（修 `SimAlgo::LoadConfig` 空实现） | `vision_guide_action_plan` | 算法配置链路无闭环 → 动作级 `threshold` / `visionType` **只打印日志**，视觉引导的判定阈值不可用 |
| **编号机制** | `action_tree` 第二、三步 **且** `vision_guide_action_plan` | 两篇文档都会各自造一套编号 → 分叉。**必须一次设计**（见约束 2） |
| `camera_integration_plan` S0~S4 | `vision_test_page_redesign_plan`、`vision_guide_action_plan` | 没有真实帧，视觉页的相机控件无处可接，视觉引导动作无法验证 |

### 约束 2：编号机制被两篇文档同时依赖，只能设计一次

- `action_tree_concurrency_plan.md` §7.2：树节点需要**持久化编号**（现状「编号」是列表行号 `ProcessPage.cpp:674` 的 `idx + 1`，移动或删除后会漂移）。
- `vision_guide_action_plan.md`：视觉引导动作需要按编号引用前序「识别」动作（`sourceActionId`）。

**两者必须是同一套编号。** 规则已裁定（方案内唯一正整数、自增、删除不复用、跨方案不唯一、分组也占编号），落点以 `action_tree_concurrency_plan.md` §7.2 为准。**P2 把这件事单独成一步做完**，而不是等两篇各自实施时各做一半。

### 约束 3：文件冲突（决定谁必须排在谁后面）

| 方案 | 主要改动范围 |
| :--- | :--- |
| `config_ownership_plan` | `src/Config/`、`src/HAL/core/` |
| `camera_integration_plan` | `src/HAL/camera/`、`src/HAL/core/`、`3rdparty/orbbec/` |
| `vision_test_page_redesign_plan` | `src/UI/VisionTestPage.*` |
| `vision_guide_action_plan` | `src/UI/ProcessPage.*`、`src/Logic/SequenceWorker.*`、`src/Config/ProcessManager.*` |
| `action_tree_concurrency_plan` | `src/UI/ProcessPage.*`、`src/Logic/SequenceWorker.*`、`src/Config/ProcessManager.*` |

- **`config` ∩ `camera` = `src/HAL/core/`（`HardwareManager`）** → 必须串行，且 **config 在前**（camera 依赖其修复）。
- **`vision_guide` ∩ `action_tree` = `ProcessPage` + `SequenceWorker` + `ProcessManager`** → **必须串行**。P5 在 P6 前（`action_tree` 第二步会重写 `ProcessPage` 的列表控件与 `m_currentActionIdx` 全部调用点，先做的话 `vision_guide` 的新动作类型要跟着重写一遍）。
- **`vision_test_page` 只动 `VisionTestPage.*`** → 与所有方案**文件不相交**，是唯一可以自由穿插的一篇。

## 三、各阶段要点与验收

| 阶段 | 内容 | 为什么排在这里 | 验收 |
| :--- | :--- | :--- | :--- |
| **P1** ✅ | `config_ownership_plan` 的 C1~C5 + §七 `save()` 原子写。**分文件本身不做**（该文档结论即「现在不分」） | 无界面变化、风险最低；不修则相机链路是流沙 | Sim 冒烟 + 相机能在真机枚举到设备、`vision.confidenceThreshold` 实际生效 |
| **P2** ✅ | `ActionData` 加 `id`、`process.json` 读取兼容与**自动升级**（不要求用户手工迁移）、`ActionType` 追加 `VisionGuide = 5`、`actionTypeName` 与类型下拉项同步 | 两篇文档的共享前置；引擎零改动，小且低风险 | 现有方案加载后编号正确、工艺行为不变 |
| **P3** | `camera_integration_plan` S0~S4（相机 HAL、持有者集合流模型、轮询探测、软对齐） | 产出真实帧，是 P4 / P5 的共同前置 | ✅ **2026-09-20 已落地（TR-099）**：真机枚举 `CP9KB530005S` + 1280×720@**30.2fps** 取流（RGB888 + 深度 Y16）、**Release 60 s 长稳 1801 帧 / 30.0 fps / 空帧 0 / 退出码 0**；`USE_ORBBEC` ON/OFF 双编译通过。**✅ 真机补验已完成（2026-09-23）**：拔插线 **11 轮全恢复**（用例 11.31，逐级 `L1 → L2 → L3` 日志实证）+ **10 min 长稳**（用例 11.32，700.1 s / 10491 帧 / 空帧 0 / 退出码 0 / 无内存增长）|
| **P4** ✅ | `vision_test_page_redesign_plan`（左 320 px 滚动控制栏 + 右 2×2 四宫格） | 相机控件要对接 P3 接口；**它是 P5 的手动验证工具** | ✅ **2026-09-20 已落地（TR-100）**：左 320 px `QScrollArea` + 右 2×2 四宫格（三格同刷）；结果表 11 列；6 个流参数 combo 由 SDK profile 枚举填充；互锁按 P3 决策 10 修正（自动运行期间相机开关 / 启停采集置灰）；`FrameSaver` 队列上限 + 单 tick 预算 + 通知合并；Debug/Release 双编译 `EXITCODE=0` 零新警告 + Sim 冒烟通过。**⚠ 真机三项待人工**：真实 SN 枚举 / D2C 软对齐 / 插拔恢复（**D2C 软对齐已于 2026-09-23 执行 → 暴露崩溃：取消对齐后必闪退，已修 TR-116；✅ 2026-09-24 复测通过，同期 11.33 / 11.34 / 11.36 亦通过**） |
| **P5** ✅ | `vision_guide_action_plan` 剩余部分（识别结果 → 基座坐标 → 两段移动） | 依赖 P2 编号 + P3 帧 + P4 作为验证工具 | ✅ **2026-09-20 已落地（TR-101）**：`ActionData` 加 `sourceActionId` / `offsetZ` / `guideR`；会话内 `QHash<int, Pose> visionResults_` 结果表（按识别动作 `id` 取用，两入口都清）；视觉引导执行路径 **零新运动代码**（两段全复用移动动作执行路径，取不到结果则报错且**不产生运动**）；UI 编辑区 + **两组坐标只读预览**（四态）+ 编辑期校验（未选择 / 悬垂引用 / 跨方案三种都拒）；`r` 一律取 `guideR`、**与 `PuffResult.yaw` 无关**；识别坐标**不入 `process.json`**。Debug `[47/47]` / Release `[15/15]` `EXITCODE=0` 零新警告 + Sim 冒烟 `BEFORE=10 → AFTER=11`。**⚠ 真机待人工**：抓取落点与重复定位精度**须先标定手眼矩阵**（`tcpCalibration.handEyeMatrix` 现为占位值） |
| **P6** ✅ | `action_tree_concurrency_plan` 第二、三步（`QTreeWidget` 树化 + 单线程协作式调度） | 与 P5 改同一批文件 → 必须在其后；与相机链路正交，放最后让相机链先稳定 | ✅ **2026-09-20 已落地（TR-102）**：`QListWidget` → `QTreeWidget`、`m_currentActionIdx`（43 处）→ `m_currentNodeId`；分组详情页 + 新建分组按钮 + 冲突类型置灰；组内校验按**资源求交**并**新增数据依赖一类**；`actions` → `nodes` 改名 + `nodeType`（旧键仍可读、升级惰性）；`ExecuteNode` / `ExecuteGroup` / `RunScheduler`（**同时下发、统一汇合**，20 ms tick），失败归因 = 首个失败者、组内暂停 / 停止 / 单步均正确。Debug `[6/6]` / Release `[15/15]` `EXITCODE=0` 零警告 + Sim 冒烟 `11 → 12` + 离线自测 `test_node_tree_check` **70 项断言全通过**。**⚠ 真机待人工**：并发节拍对比、组内暂停 / 停止 / 急停、多轴干涉。**D8 / D9 节拍回收本批未做**（原建议同批） |

## 四、可并行边界

若同时开两条工作线（多人 / 多分支）：

| 工作线 | 阶段 | 文件范围 |
| :--- | :--- | :--- |
| **相机线** | P3 → P4 → P5 | `HAL/camera`、`HAL/core`、`UI/VisionTestPage`、`UI/ProcessPage`、`Logic/SequenceWorker` |
| **动作树线** | P6 | `UI/ProcessPage`、`Logic/SequenceWorker`、`Config/ProcessManager` |

**两条线在 P5 与 P6 之间冲突**（同一批文件），所以：**相机线走到 P4 时，动作树线可以并行推进 P6 的准备（数据模型与防呆校验设计），但 P6 的代码落地必须等 P5 完成。**

**单人开发时不要并行**——P6 第二步会重写 `ProcessPage` 的列表控件与 `m_currentActionIdx` 的全部调用点（`:252` / `:642` / `:651-658` / `:674-675` / `:794-798` / `:1201`），与 P5 的新动作类型叠加会让冲突难以定位。

## 五、不建议的顺序（反例）

| 反例 | 后果 |
| :--- | :--- |
| **先做 `action_tree`（树 + 并发）** | ① 编号机制单独定稿，与 `vision_guide` 的 `sourceActionId` 分叉；② 它重写 `ProcessPage`，之后 `vision_guide` 加动作类型要重写一遍；③ 相机链路仍未打通，节拍优化在「抓不到」的前提下没有意义 |
| **先做 `camera_integration_plan`** | 配置断链（C1/C2/C5）未修 → 相机**枚举不到真机设备**，S0~S4 只能对着 `SimCamera` 空跑，真机问题推迟到最贵的时候才暴露 |
| **先做 `vision_test_page_redesign_plan`** | 它是**纯 UI 重构**（25 项映射对账表说明重点在防功能丢失），但页面上的 11 个相机控件要绑 P3 的接口。先做就得绑旧 `CameraManager`，P3 落地后再改一次 |
| **把 P2 拆到两篇各自做** | 必然产生两套编号。这是本编排里**唯一不可妥协**的一条 |
| **P1 只做「分文件」不做 C1~C5** | `config_ownership_plan` 的结论就是**现在不分**；真正的缺陷是 C1（`SimAlgo::LoadConfig` 空实现）、C2（相机设备标识两键不相通）、C5（三套路径解析）与 §七（`save()` 非原子写）。**分文件不是目标** |

## 六、台账与文档同步约定

- **TR 编号按落地顺序分配，不按文档分配**。三篇文档都写了「待开 TR-097 起」，那只是**预留区间**——实际编号取决于谁先落地。
- **P0（J1 偏移）已收口，占 TR-095**——属**配置调整，无代码改动**（见 §七）。后续 D9（`smoothTime` 配置化）若落地会占新编号，**建议并入 P6 批次**，与 D8 加速度边界扫描同批做。
- 每阶段完成后同步：`TEST_RECORD.md` 追加记录 → 对应方案文档状态 `⏳` → `🟡 已实施待真机` → `🟢` → `doc/README.md` 状态列 → `AGENTS.md` 相关章节。
- **每阶段改完代码须 Debug + Release 双编译**（用户实际运行 `out\build\x64-Debug\CreamPuffRobot.exe`）。

## 七、P0 状态更新：J1 累积偏移（✅ 已缓解，不再阻塞）

`doc/joint_drift_analysis.md` / `TEST_RECORD.md` **TR-095**：**2026-09-18 真机收口**。

- **处置**：J1 `maxAccel` 200→80 °/s²、`maxSpeed` 30→25 °/s（提交 `4c95d14`，2026-09-16）。
- **结果**：连续 **247 轮（≈4 h）目测无偏移**；日志确认 1333 次下发全为 80 °/s²（对照 09-15 的 200 °/s²）。**用例 8.30 通过，机理确认为 J1 开环步进失步。**
- **对本文编排的影响**：**P0 从「阻塞项」降级为「已完成」**，实际起点是 **P1**。原排最前的理由（正确性问题、会污染 P6 的节拍对比与 P5 的定位精度评价）**已消除**。

**但有两条尾巴，不要当作已彻底闭环**：

1. **「目测无偏移」≠「残余失步为 0」。** 机制上残余失步**不会被自动纠正**（卡内位置寄存器是指令计数器；循环内不回零），只是可能已降到目视阈值以下。若计划**无人值守的长循环**（远超 247 轮），先做 `joint_drift_analysis.md` §6.2 的 **D6（定量读数）**，再决定是否需要 D10（轮次间定期回零）。
2. **降加速度是拿节拍换精度，与 P6 直接冲突。** P6（动作树并发）的全部目的是**压缩节拍**，而本次修复正是**花掉**节拍。两者必须合并考虑：
   - **D8（加速度边界扫描）**：找出无可见漂移的最高 `maxAccel`，把节拍买回一部分。**建议与 P6 同批做**——现在做等于对着一个即将重新调整的目标调参。
   - **D9（`smoothTime`）**：S 曲线平滑削减斜坡 jerk 尖峰，理论上允许在更高加速度下不丢步 → 是**同时保住节拍与精度**的杠杆。已从「治本方向」转为「节拍回收杠杆」，前置是查卡手册确认其合法范围（`3rdparty/bopai/` 内无 PDF 手册）。

**结论**：**P0 不再占用排期**；D6 可在 P1 之前顺手做（成本极低，只影响长循环决策），D8 / D9 并入 **P6**。
