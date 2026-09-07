# 自动运行页 5 按钮 + SequenceWorker 显式状态机重构

> 范围：**架构指令 1/2**。本次**不做**模式互锁（那是下一步），**不做**安全位/初始位。
> 前置文档：`doc/ui_interlock_plan.md`（下一步互锁用，本次需同步其 4 按钮矩阵与信号签名）

## 一、需求

`AutoRunPage` 4 按钮 → **5 按钮**：
1. `[▶ 启动/继续]`(绿) 启动新流程 / 从暂停处继续
2. `[⏸ 暂停]`(黄) → `Pause()`，减速停止，保持上下文
3. `[■ 停止]`(红) → `StopImmediate()`，结束流程、废弃上下文、退回就绪
4. `[↺ 清报警]`(灰) 清除引擎故障锁存（取代原"复位"）
5. `[⟳ 初始化]`(蓝) 整机初始化

`SequenceWorker` 引入 `enum class WorkerState { Idle, Running, Paused, Fault };` + `Pause()/Resume()`。

## 二、已确认的 4 项决策

| # | 决策 |
|---|---|
| 1 | **点级暂停 + 原地续走**：减速停当前轴→挂起→Resume 只重发**当前那条**绝对目标 `MoveAbs` 续走；Delay 按剩余时间；Extrude 不反向补料；Vision 不重采图 |
| 2 | **【初始化】只 `Initialize()` + 分级引导**去手动页使能/回零（符合"程序不自动使能"原则）。回零入口在 `ManualControlPage.cpp:148` 本就存在，未丢失 |
| 3 | **仅自动页立即停**：`Stop()` 保持 cancel（就近，单步/退出路径不变）；新增 `StopImmediate()`（减速停）供自动页调用 |
| 4 | 安全位/初始位**本次不做** |

**决策 1 的决定性理由**（否掉"重试整个动作"）：`ExecuteExtrude` 分"挤出→回抽"两段，回抽阶段暂停后重试整个动作会先把挤出轴**反向补回满量程**（多挤料）再回抽；且 Delay 会重新完整计时、Vision 重新采图、多点 Move 会重发已完成点（可能选到另一逆解分支跳变）。

## 三、关键事实（探查核实，含与 Gemini 判断的出入）

- **底层"停止"已走 `MC_Stop` 减速斜坡**（`BoPaiCard.cpp:327/340`），**不是断脉冲** → 不丢步。用户观察到的"继续运动一点"= 减速距离（J1 30°/s、decel 跟随 maxAccel 50°/s² → 约 9°）。**不必也不能改成断脉冲**（开环会丢步）。
- `HardwareManager::StopAxis`(`cpp:624-648`)：卡轴 MC_Stop + 舵机 `Stop()`(CMD24 停+锁位) + 清 busy + `AbortHoming`，**不断使能**。**`AbortHoming` 仅复位"正在回零中"的轴**（`cpp:721-731`）→ 运行中减速停**不会**把 `IsSystemHomed()` 打回 false，Resume 后 MoveAbs 不被回零互锁拒绝。**这是方案成立的前提，须写进注释**。
- 全部挂起点收敛于 3 个等待函数 + 1 个循环头：`WaitForAxes`(`cpp:420`)、`Impl::WaitForCancelOrTime`(`cpp:55`)、`Impl::WaitForStep`(`cpp:72`)、`ExecuteMove` 点循环头(`cpp:339`)。`WaitForStep` 是现成的"可取消挂起门闩"模式，Pause 同构。
- `IsPaused()` 现语义 `stepMode && running`，**全项目无调用**（`cpp:241`）→ 可安全改为 `state==Paused`。
- `stateChanged` 仅 **ProcessPage.cpp:858-880 一个消费者**（4 个中文字符串比较）；AutoRunPage 未接。MainWindow 不接任何 worker 信号。
- 死代码：`SequenceWorker.h:88-89` 声明 `CancelSleep`/`WaitForStepGate` 无定义。
- 无 Fault 持久态/清除报警概念；卡轴报警靠 `lastAlarm_` 拒绝 `MoveAbs`(`cpp:394`) → 动作失败 → `errorOccurred`，已能自然进 Fault，**无需额外订阅 `axisAlarm`**。
- 卡轴清报警实际靠"断使能+重新使能"（`EnableAxis` 内 `MC_ClrSts+MC_AxisOn`）；`IAxisServo::ClearAlarm` 只清本地串 → **【清报警】不碰硬件使能**。
- 回零入口：`ManualControlPage.cpp:148`【一键回零】+ `:443`【全部使能】。

## 四、实现方案（分 6 步，每步 Debug+Release 编译 + Sim 冒烟）

### S1 `HardwareManager` 新增（纯增量）
```cpp
// 减速停止：卡轴 MC_Stop 斜坡 + 舵机停+锁位；不断使能、舵机不松力（区别于 EmergencyStop）
bool StopAxes(const QVector<LogicalAxis>& axes);   // 逐个调既有 StopAxis（清 busy + AbortHoming）
bool StopAllAxes();                                 // 对 AxisMap 已绑定轴逐个调
```
> **不要**直接用 `motionCard_->StopAll()`：不覆盖舵机、不清 `axisBusyUntilMs_`、不发 `axisMoveFinished` → `WaitForAxes` 会误判"已到位"或空等 30s。

### S2 `SequenceWorker` 状态机骨架
```cpp
// SequenceWorker.h（Q_ENUM 必须在类内：跨线程 Queued 需元类型注册，否则运行时报 Cannot queue arguments）
enum class WorkerState { Idle, Running, Paused, Fault };
Q_ENUM(WorkerState)
enum class PauseReason { None, User, Step };
Q_ENUM(PauseReason)

void Pause();                 // Running→(协作式)Paused；stepGatePending 时拒绝（防误放行重现 D2 竞态）
bool Resume();                // Paused→Running；stepGatePending 时等价于放行一步
void Stop();                  // 保持现状：仅 cancel（就近，跑完当前动作）—— 单步/ShutdownWorker 用
void StopImmediate();         // 新增：cancel + StopAllAxes()（减速停，保持使能）—— 自动页【停止】用
void ClearFault();            // Fault→Idle，仅清引擎锁存，不动硬件使能
void EmergencyStop();         // 不变

WorkerState GetState() const;      PauseReason GetPauseReason() const;
bool IsSingleActionSession() const;  bool IsStepGatePending() const;
bool IsRunning() const;  // == (state != Idle)，保持既有调用点语义（单步暂停时亦 true）→ 零回归
bool IsPaused() const;   // == (state == Paused)
bool IsStepMode() const;

signals:
    void stateChanged(SequenceWorker::WorkerState, SequenceWorker::PauseReason);  // 替换 QString 版
```
`SetState()` 统一出口；3 处重复的"清 running + emit 空闲"收敛到 `SetState(Idle)`。删死代码 `h:88-89`。
**进 Fault 条件**：动作失败且非 cancel（即今日 `emit errorOccurred` 的分支），保留上下文；正常/中断 → `Idle`。

### S3 `ProcessPage.cpp:858-880` 改 enum 消费者（**高风险，先做**）
判定表（唯一真相，不加 `StepPaused`——生命周期与 `Paused` 完全重叠）：

| 情形 | state | reason | stepMode | stepGatePending |
|---|---|---|---|---|
| 连续运行·执行中 | Running | None | false | false |
| 单步·执行中 | Running | None | true | false |
| 单步·等待 NextStep | Paused | **Step** | true | **true** |
| 用户暂停 | Paused | **User** | – | false |
| 单动作会话 | Running | None | false(内部清) | false | **IsSingleActionSession=true** |
| 故障 | Fault | None | – | false |

单步按钮可点的**权威条件**：`state==Paused && reason==Step`（比字符串更强，且守 D2）。`Running` 分支按 `IsSingleActionSession()` 还原"● 执行选中动作"文案。

### S4 挂起门闩 + reissue（决策 1 落地）
```cpp
enum class SleepOutcome { Ok, Canceled, Paused };
SleepOutcome SleepTick(int ms);   // 每 20ms 醒，paused/cancel 立即返回

bool WaitForAxes(const QVector<LogicalAxis>& axes, int timeoutMs,
                 std::function<bool()> reissue = nullptr);
bool PauseGate(const QVector<LogicalAxis>& axes = {},
               std::function<bool()> reissue = nullptr);  // 返回 false=被 cancel 终止
```
`PauseGate` 内：`SetState(Paused, User)` → `InMainThread{ StopAxes(axes) }`（减速停+清 busy）→ `QEventLoop`+20ms 轮询等 `!paused || cancel` → 若 cancel 返回 false → `InMainThread{ reissue(); }`（重发**当前那条**绝对目标）→ `SetState(Running)`。
- `Move/Extrude/Gripper`：把当前段的 `MoveAbs` 包成 reissue 传入。
- `Delay`：`WaitForCancelOrTime` 改为同构（累计 `pauseAccumMs`，按剩余时间判），无 reissue。
- `ExecuteMove` 点循环头 + `ExecuteActions` 循环头：加无参 `PauseGate()`（只挂起不停硬件），保证暂停一定生效。
- `WaitForAxes` 恢复后 `t.restart()`——暂停时长不占 30s 超时预算。
- **顺序约束**：cancel/paused 检查必须在 `allDone` 判定**之前**。

### S5 `StopImmediate()`（决策 3）
```cpp
void SequenceWorker::StopImmediate()
{
    if (impl_->state.load() == WorkerState::Idle) return;
    impl_->cancel.store(true);
    InMainThread([] { HardwareManager::instance().StopAllAxes(); });   // 减速停，保持使能
}
```
- 不新增 `interrupted` emit：仍由执行循环检测 cancel 后单点发出（≤20ms 退出）。
- `Stop()` 不变 → **ProcessPage 单步停止（`cpp:184`）与 `MainWindow::ShutdownWorker`（`cpp:187`）零改动、零回归**。

### S6 `AutoRunPage` 5 按钮 + 槽
- 按下标分派（**不再** `text.contains` —— 改文案会静默错配）：`m_btnStartOrResume/m_btnPause/m_btnStop/m_btnAlarmClear/m_btnInit`。
- 新增 `QPushButton:disabled` 样式（既有缺失，置灰无视觉反馈）；字号 12px + tooltip；若太挤备选 3+2 两行。
- 新增 `SetHint(text,color)` 收敛 8 处重复样式；**删除** `cpp:241/258/344/375/395/446/455/465` 全部散点 `setEnabled` → 统一 `UpdateControlsEnabled()`。
- 接线：新增 `stateChanged`→`OnWorkerStateChanged`；`homeStateChanged`/`enableStateChanged`→`UpdateControlsEnabled()`。
- 槽要点：
  - `OnStartOrResumeClicked`：`Paused`→`Resume()`；`Fault`→提示先清报警；`Idle`→三层门禁不变 + `RunSequence`（**不在此改按钮**，等 `stateChanged(Running)`）
  - `OnPauseClicked`：仅 Running 可暂停；`stepGatePending` 时提示"点启动/继续即可放行下一步"
  - `OnStopClicked`：`StopImmediate()`；若引擎本就 Idle 则就地 `UpdateControlsEnabled()`（Stop 不发信号，防永久置灰）
  - `OnAlarmClearClicked`：Running/Paused 时拒绝（先停止）；Fault→`ClearFault()` + 提示"若驱动器仍报警请到手动页断使能后重新使能"；Idle→"当前无故障锁存"
  - `OnInitClicked`：非 Idle 拒绝；`Initialize()` 后按 未连接/未使能/未回零/就绪 给分级引导
- 保留 `RequestHomeAll()` 私有函数（不占按钮位）供后续物理按钮转接。

**按钮启用矩阵**（`gate = 自动模式 ∧ 非急停待恢复`，下一步互锁接入；本次 `gate=true` 行为不变）

| 状态 | ▶启动/继续 | ⏸暂停 | ■停止 | ↺清报警 | ⟳初始化 | 方案下拉 |
|---|---|---|---|---|---|---|
| Idle·未回零 | ✗ | ✗ | ✗ | ✓ | ✓ | ✓ |
| Idle·已回零 | ✓¹ | ✗ | ✗ | ✓ | ✓ | ✓ |
| **Running** | ✗ | **✓** | **✓** | ✗ | ✗ | ✗ |
| **Paused(User)** | **✓继续** | ✗ | **✓** | ✗ | ✗ | ✗ |
| **Paused(Step)** | **✓放行下一步** | ✗ | **✓** | ✗ | ✗ | ✗ |
| **Fault** | ✗ | ✗ | ✗ | **✓** | ✓ | ✓ |

¹ 未使能仍可点，槽内提示"请先使能"（保留既有交互，不静默置灰）。

## 五、文件清单

| 文件 | 改动 |
|---|---|
| `src/HAL/core/HardwareManager.h/.cpp` | 新增 `StopAxes`/`StopAllAxes`（纯增量） |
| `src/Logic/SequenceWorker.h/.cpp` | enum+Q_ENUM、`SetState`、`Pause/Resume/ClearFault`、`StopImmediate`、`PauseGate`+reissue、`stateChanged` 换 enum、删死代码 |
| `src/UI/ProcessPage.cpp` | `:858-880` 改 enum 消费者 |
| `src/UI/AutoRunPage.h/.cpp` | 5 按钮 + 5 槽 + `UpdateControlsEnabled` + `OnWorkerStateChanged` + `SetHint` |
| `src/UI/MainWindow.cpp` | **无需改动** |
| `doc/ui_interlock_plan.md` | 同步 4→5 按钮矩阵与 `stateChanged` 签名（下一步实施前必读） |
| `TEST_RECORD.md` | 记账（追加前**重读取当前最大 TR 编号**，防并行会话撞号；TR-071 现存两条重复待修） |

## 六、风险与对策

| 风险 | 对策 |
|---|---|
| Q_ENUM 缺失 → Queued 连接运行时报 "Cannot queue arguments" | enum 必须在 `SequenceWorker` 类内 + `Q_ENUM`；`AutoRunPage.h` 需 `#include "SequenceWorker.h"` |
| **ProcessPage 单步回归（D2 历史 bug：连跑两个动作）** | S3 后必测连续 5 次单步一一对应；按钮仅"单步暂停"期可点 |
| Pause/Step 双挂起源竞态 | `Pause()` 在 `stepGatePending` 时拒绝；`Resume()` 在该态转 `stepGo` |
| `StopAxis`→`AbortHoming` 误清 homed | 已核实仅复位"正在回零中"轴；PauseGate/StopImmediate 处写死注释，S4 验证 `IsSystemHomed()` 保持 true |
| 清 busy 造成"假到位" | cancel/paused 检查必须在 allDone 之前；StopImmediate 先置 cancel 再停硬件 |
| 急停/暂停中关程序挂死 | `PauseGate` 挂起循环检测 cancel 可退出；`ShutdownWorker` 走 `Stop()`（不变） |

## 七、验证

- Debug+Release 双编译（`AGENTS.md:26`）；Sim 冒烟判定 = 日志含 `Initialize complete`（`AGENTS.md:28`）。
- Sim 可测性：`HardwareManager::MoveAbs` 仍 `MarkAxisBusy(≥1000ms)`，移动等待窗口 ≥1s；**建议造含 10s 延时 + 2 个移动点的测试方案**（Delay 最易稳定触发暂停）。
- 手测：① 运行中暂停→继续→走到**当前点目标**（不回上一点）无误差累积；② 多点第 1 点中途暂停→到点1→点2；③ **Extrude 回抽阶段暂停→继续不反向补料**；④ Delay 10s 第 8s 暂停→剩余约 2s；⑤ 运行中【停止】≤1s 减速停 + interrupted + 回 Idle；⑥ 造失败→Fault→清报警→可重启；⑦ ProcessPage 单步 5 次 + 单动作（回归）；⑧ 急停回归；⑨ 暂停中关程序不挂死。
