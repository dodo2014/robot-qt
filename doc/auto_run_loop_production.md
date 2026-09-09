# 自动运行循环生产（Loop Production）需求方案

> 状态：**待审核**（2026-09-08 出稿，供用户与 Gemini 过一遍；**代码尚未实施**）
> 来源：真机验收阶段 8 用例 **8.4** —— 用户问「自动运行是单次运行？不是循环运行？」，核实为只跑单轮；用户裁决**需要连续循环生产**。
> 关联：`doc/test/real_machine_plan_phase3.md`（阶段 8/9/10）、`doc/auto_run_button_refactor.md`（TR-073）、`doc/global_safe_position.md`（TR-074）、`doc/ui_interlock_plan.md`（TR-075）

---

## 一、现状核实（代码事实）

| 事实 | 位置 |
|---|---|
| 唯一循环 = 单遍动作遍历：`for (int i = 0; i < actions.size(); ++i)` | `src/Logic/SequenceWorker.cpp:435-482` |
| 跑完由 `StartExecution` 发 `schemeFinished` + `SetState(Idle)`，**无回绕** | `SequenceWorker.cpp:392-402` |
| `RunSequence` 启动时无条件重置：cancel/paused/pauseAccumMs/stepMode/safeSession/**currentIndex=-1**/singleSession；**无循环计数变量** | `SequenceWorker.cpp:150-189` |
| config.json 无 `loop/cycle/repeat` 键；三页 UI 无循环开关/次数输入 | 已 grep 确认 |
| `PickCycleController`（视觉抓取单周期）为**死代码**（仅 CMakeLists 引用），其 StartCycle 亦为单次 | `src/Logic/CMakeLists.txt:6,11` |
| 暂停 = 保留上下文原地续走（PauseGate + reissue 当前绝对目标 MoveAbs） | `SequenceWorker.cpp:622-657`、`:650-653` |
| 停止 = `StopImmediate` = cancel + `StopAllAxes` 减速停（保持使能），废弃上下文 | `SequenceWorker.cpp:304-310` |
| `ConfigManager` 无信号；`paramsChanged` 属 ConfigPage，唯一消费者是 `ReloadFromConfig`（重建 Kinematics） | `src/Config/ConfigManager.*`、`src/UI/MainWindow.cpp:165` |
| `ConfigManager::getValue` 非线程安全；worker 线程内**从不**读 config（读取全在主线程） | `SequenceWorker.cpp:111-148`、`237-288` |
| `MoveAbs` 对所有轴强制软限位拦截（越界 return false，不夹紧） | `src/HAL/core/HardwareManager.cpp:418-422` |
| `ReloadFromConfig` 在 running 时跳过（防主线程写/worker 读竞争） | `SequenceWorker.cpp:113-118` |

**不可破坏的既有语义**：暂停=续走、停止=废弃上下文、开环步进禁断脉冲（MC_Stop 减速斜坡）、MoveAbs 绝对坐标不累积误差、程序不自动使能、初始化不含回零。

---

## 二、功能形态

**三挡可配：`单轮 / 指定 N 次 / 无限`，默认「单轮」。**

- 生产语义：泡芙抓取是按盘/按箱批量作业，N 次是刚需；无限循环保留给连续流水线。
- **默认单轮 → 与今天行为逐字节一致，TR-073 零回归**（8.1–8.17 全部保持有效）。
- 无限循环需**二次确认弹窗**（见 §八）。

### UI 入口：AutoRunPage（方案下拉下方新增「循环」行）

```
[运行方案:  ▾ 抓取流程                    ]        ← 现有
[循环:      ▾ 单轮|指定次数|无限循环 ] [10]次  [x] 循环间隔回安全位   循环 3/10
```

| 控件 | 成员 | 默认 | 说明 |
|---|---|---|---|
| 模式下拉 | `m_loopModeCombo` | 单轮（index 0） | 0=单轮 / 1=指定次数 / 2=无限 |
| 次数框 | `m_loopCountSpin` | 10（1..9999） | 仅「指定次数」时 visible |
| 间隔回安全位 | `m_loopSafeChk` | **勾选 true** | 见 §五 |
| 进度标签 | `m_cycleLabel` | `单轮` / `循环 0/10` | 运行时 `循环 3/10`、`循环 3/∞` |

- ConfigPage **不新增**控件（单一数据源，避免双写冲突）。
- 循环控件与方案下拉同规则：`busy(Running/Paused)` 时置灰（走 `UpdateControlsEnabled` 唯一出口），防"跑到第 5 轮改次数"的竞态。
- **重试次数不进 UI**（见 §七，仅 config 配置项，默认 0）。

---

## 三、config 字段设计

| 键 | 类型 | 默认 | 读 | 写 |
|---|---|---|---|---|
| `production.loop.mode` | int（0/1/2） | `0` | AutoRunPage 构造初始化下拉 | 下拉 `currentIndexChanged` |
| `production.loop.count` | int（1..9999） | `10` | 构造初始化 SpinBox | SpinBox `valueChanged` |
| `production.loop.returnToSafePos` | bool | `true` | 构造初始化 + **启动前安全门禁** | 复选框 `toggled` |
| `production.loop.retryCount` | int（0..9） | `0` | 启动前读入 LoopConfig | 暂不进 UI（config 手改） |
| `production.loop.retryDelayMs` | int | `500` | 重试间隔 | 暂不进 UI（config 手改） |

- 写入走 `ConfigManager::instance().set(...)`（300ms 防抖落盘），与 `safePos` 同构（现读现用、无内存缓存镜像）。
- **不 emit `paramsChanged`**：其唯一消费者是 `ReloadFromConfig`（重建 Kinematics），循环参数与运动学无关，emit 只会造成无谓重建。
- `config/config.json` **不需手改**：首次改动控件时 `set()` 自动创建 `production` 节点；缺失时走 `getValue` 默认值，旧 config 零迁移。

---

## 四、执行流程改造

**在 `ExecuteActions` 层加"会话内循环外壳"**：现主体改名 `ExecuteOnce`（原样搬迁），新增 `ExecuteActions` 作为循环外壳。

> 为何不放 `StartExecution` 外层或 `RunSequence`（主线程）层：前者需重复门禁/状态置位且拆散 `schemeFinished` 语义；后者会阻塞主线程，与既有线程模型冲突。放会话内 → cancel / PauseGate / Fault / schemeFinished 全部复用既有单点，改动面最小。

### 4.1 头文件增量

```cpp
// 循环生产配置（值语义）。主线程组装 → 随 RunSequence 进 worker 线程；
// worker 线程只读副本，绝不跨线程读 ConfigManager（json 读写非线程安全）。
struct LoopConfig
{
    enum class Mode { Single, Count, Infinite };
    Mode mode            = Mode::Single;
    int  count           = 1;      // Mode::Count 生效，>=1
    bool returnToSafePos = true;   // 进入下一轮前先回安全位（防横扫桌面）
    int  retryCount      = 0;      // 0=不重试（默认/今天行为）；N=动作级重试 N 次
    int  retryDelayMs    = 500;
};

// 默认参数 → 既有调用点（AutoRunPage/ProcessPage/RunSafePos）零改动、行为零变化
bool RunSequence(const SchemeData& scheme, const LoopConfig& loop = LoopConfig{});

signals:
    void cycleChanged(int index, int total);   // index 从 1 开始；total < 0 表示无限

private:
    bool ExecuteActions();               // 【新】循环外壳
    bool ExecuteOnce();                  // 【改】原 ExecuteActions 主体（单轮，逐动作）
    bool BuildSafePosActionFrom(ActionData& act, double j1, double j2, double z, double r);
    bool ReturnToSafePosInline();        // 循环间隔内联回安全位
    bool ExecuteActionWithRetry(const ActionData& act, int idx);  // 动作级重试包装
    // Impl 新增：LoopConfig loop; int cycleIndex = 0; bool safePoseValid = false;
    //           double safeJ1/J2/Z/R;   // 主线程启动时快照
```

### 4.2 循环外壳骨架

```cpp
bool SequenceWorker::ExecuteActions()
{
    const LoopConfig loop = impl_->loop;                       // 值快照
    const int total = (loop.mode == LoopConfig::Mode::Count) ? qMax(1, loop.count) : -1;

    for (int cycle = 1; ; ++cycle) {
        if (impl_->cancel.load()) {                            // ① 轮边界 cancel
            emit interrupted(QStringLiteral("用户停止"));
            return false;
        }
        if (!PauseGate()) {                                    // ② 轮边界挂起（暂停/停止必生效）
            emit interrupted(QStringLiteral("用户停止"));
            return false;
        }
        // ③ 循环间隔回安全位（第 1 轮之前不动 → 向后兼容；最后一轮之后不回）
        if (cycle > 1 && loop.returnToSafePos) {
            emit logMessage(QStringLiteral("循环间隔：回安全位（先抬 Z 再水平）"));
            if (!ReturnToSafePosInline()) {
                if (impl_->cancel.load()) emit interrupted(QStringLiteral("用户停止"));
                else { SetState(WorkerState::Fault);
                       emit errorOccurred(QStringLiteral("循环间隔回安全位失败")); }
                return false;
            }
        }

        impl_->cycleIndex = cycle;
        emit cycleChanged(cycle, total);                       // ④ UI 进度
        emit logMessage(QStringLiteral("=== 循环 %1/%2 开始 ===")
            .arg(cycle).arg(total > 0 ? QString::number(total) : QStringLiteral("∞")));

        if (!ExecuteOnce()) return false;                      // ⑤ 单轮（失败原因已在内部单点发出）

        emit logMessage(QStringLiteral("=== 循环 %1/%2 完成 ===")
            .arg(cycle).arg(total > 0 ? QString::number(total) : QStringLiteral("∞")));
        if (loop.mode == LoopConfig::Mode::Single) break;
        if (total > 0 && cycle >= total) break;
    }
    return true;
}
```

- `ExecuteOnce()` = 现 `ExecuteActions` 主体**原样搬迁**（cancel 检查 / PauseGate / currentIndex / actionStarted / 失败→Fault 锁存 / 单步挂起），一行不改。
- `RunSequence` 增量：`impl_->loop = loop; cycleIndex = 0;` + **主线程捕获安全位数值**（`safePos.enabled` 为真时读 j1/j2/z/r 存 Impl），规避 worker 读 config。
- `RunSingleAction`（:191-232）与 `ClearFault`（:370-381）增量清 `loop` / `cycleIndex`（与既有清 `currentIndex`、`singleSession` 同列）。

### 4.3 间隔回安全位（内联，不走 RunSequence）

```cpp
// 三不：不置 safeSession、不发 actionStarted/actionFinished、不重入门禁
// → ProcessPage 动作列表选中行不会被联动跳走（TR-074 的 IsSafePosSession 守卫仍只服务手动触发）
bool SequenceWorker::ReturnToSafePosInline()
{
    ActionData act;
    if (!BuildSafePosActionFrom(act, impl_->safeJ1, impl_->safeJ2, impl_->safeZ, impl_->safeR))
        return false;                    // 未启用/未捕获 → 判为故障（不静默跳过）
    return ExecuteMove(act);             // 白得：cancel 检查、PauseGate、reissue 续走、逐点日志
}
```

`BuildSafePosActionFrom` = 从 `RunSafePos`（:237-288）抽出的参数化公共构造：安全位数值由入参给（循环用主线程快照；`RunSafePos` 传 config 现值），**当前位仍用 `InMainThread(GetPosition)` 实时读** → 点1 始终是"原地抬 Z"。`RunSafePos()` 改为调用同一函数，逻辑零重复。

---

## 五、循环间行为

### 5.1 是否回安全位 —— **回（用户已拍板，默认开）**

- **时机**：只在**进入下一轮之前**回；**第 1 轮之前不回**（操作员已把机器停在起始位，多走是干扰），**最后一轮之后不回**（保持停机位便于取料）。
- **理由**：结束位（放料位）直奔下一轮第一步起点是多轴同时 `MoveAbs`（`SequenceWorker.cpp:569-572` 依次下发 J2/R/J1/Z，无中间等待），会横扫桌面撞料/撞夹具——这正是 TR-074 存在的原因。回安全位把跨轮大位移拆成"抬 Z → 水平 → 下一轮第一段"。
- **未示教安全位（`safePos.enabled=false`）时拒绝启动循环**并提示：「循环运行需先示教安全位（手动控制页『示教安全位』），或取消勾选『循环间隔回安全位』」。与"未回零禁止启动"同构，且可用配置一键放行。

### 5.2 计数与进度

- `Impl::cycleIndex`（1-based）+ `Impl::loop`（值拷贝）。
- **暂停后恢复：计数保留**（`Pause`/`Resume` 不触碰 cycleIndex，零改动达成）。
- **停止 / 急停 / Fault：计数废弃**（走 RunSequence 重置 / ClearFault 重置），符合"停止=废弃上下文"。
- 计数**不持久化**（无断点续产：断电/停止后坐标不可信）。
- UI：`cycleChanged` → `m_cycleLabel` 显示 `循环 3/10` / `循环 3/∞`；终态**不清空**（便于取证），下次启动前重置。
- **不改动**「今日产量」LCD（当前为静态 mock，联动会误导；产量统计另开需求）。

---

## 六、与既有语义的交互（逐条结论）

| # | 场景 | 结论 |
|---|---|---|
| 1 | **■ 停止**（用户已拍板） | **立即减速停 + 终止整个循环**，不跑完当前轮；发 `interrupted("用户停止")`，使能保持。理由：既有语义已定为"立即"+废弃上下文；若跑完当前轮，停止按钮最长要等一个完整节拍，等于削弱急停前置手段，且"正在安全停止…"文案会变成谎言 |
| 2 | **⏸ 暂停** | 原地挂起（最近 gate 减速停、保持使能），继续后从当前动作续走；**计数不丢**，`m_cycleLabel` 保持不变（零改动） |
| 3 | **急停** | cancel + 断使能 → 下一检查点 interrupted 退出；`estopPending_` 锁存 → 必须"使能 + 回零"解锁（10.13→10.17 恢复链路不受影响）。**急停后禁止续跑循环**（坐标不可信） |
| 4 | **动作失败 → Fault** | 终止整个循环并锁存（不自动重试，retryCount=0 时）；「⛔ 错误」+ 仅「↺ 清报警」可点。理由：无人值守下自动重试可能重复抓取/撞料 |
| 5 | **模式互锁（循环期间禁止切手动）** | **保持禁止（TR-075 不开后门）**。缓解：① 自动模式白名单含视觉检测页，可切过去观察（只读）；② ■停止全程可点，停止后即可切手动；③ Fault 态放行切手动脱困（10.8 规则），故障停机不会把人锁死 |
| 6 | **单步 `RunSingleAction`** | 不受影响（走 `StartSingleExecution` → 直接 `ExecuteAction`，不经过循环外壳）；循环运行时被 `running` 门禁拒绝，与今天一致 |
| 7 | **方案切换 / 下拉** | 不受影响（`IsExecutionActive()` 为真 → 工艺页弹窗拒绝；AutoRunPage 下拉按 `!busy` 置灰） |
| 8 | **安全位会话（TR-074）** | `RunSafePos()` 走默认单轮参数，行为不变；内联回安全位不发 `actionStarted`（ProcessPage 列表不误选）。循环期间手动「回安全位」仍被 `ManualControlPage:537` 拒绝（state != Idle） |
| 9 | **`ReloadFromConfig`** | 循环期间被拒（已有 WARN）→ 中途改 L1/TCP 不重建 Kinematics，无并发 UB |
| 10 | **UI 可达性** | 循环必在自动模式启动（按钮 gate），故**暂停/停止按钮全程可用**，不存在 phase3 §0-B「提示不可达」问题 |

**刻意收敛的边界**：`MainWindow.cpp`、`ProcessPage.cpp`、`ManualControlPage.cpp`、`ConfigPage.cpp`、`HardwareManager.*` **全部零改动**。

---

## 七、重试口子设计（用户要求：留口子，默认不重试）

- `retryCount = 0`（**默认**）= 今天行为：任一动作失败 → `errorOccurred` + `SetState(Fault)` + 终止循环。
- `retryCount = N > 0`：**动作级重试**（非整轮重跑——整轮会重复已完成的前序动作如已抓取，风险更大）。

| 项 | 规则 |
|---|---|
| 可重试动作 | **Vision**（未检出 → 重新采图定位）与 **Move** |
| 不可重试 | **Extrude / Gripper / 回抽 / Delay** —— 重复出料、重复夹取危险，直接 Fault |
| 重试间隔 | `retryDelayMs`（默认 500ms）等物料稳定 |
| 重试期间反馈 | **只写 `logMessage`，不发 `errorOccurred`**（避免 UI 误报"错误"、状态跳 ⛔） |
| 重试耗尽 | 发 `errorOccurred` + `SetState(Fault)` → 终止整个循环 |
| 可中断性 | 重试间隔复用 Delay 类等待的 cancel / PauseGate 通道 → 暂停/停止/急停同样生效 |
| UI | **本次不进 UI**（仅 config 手改），保持界面简洁；待真机验证确有需求再开口 |

```cpp
bool SequenceWorker::ExecuteActionWithRetry(const ActionData& act, int idx)
{
    const int maxTry = impl_->loop.retryCount + 1;
    const bool retryable = (act.type == ActionType::Vision || act.type == ActionType::Move);
    for (int attempt = 1; attempt <= maxTry; ++attempt) {
        if (ExecuteActionOnce(act, idx)) return true;         // 成功
        if (impl_->cancel.load()) return false;               // 停止/急停：不重试
        if (!retryable || attempt == maxTry) return false;    // 不可重试 或 已耗尽 → 上层 Fault
        emit logMessage(QStringLiteral("动作「%1」失败，%2ms 后重试（%3/%4）")
            .arg(act.name).arg(impl_->loop.retryDelayMs).arg(attempt).arg(maxTry - 1));
        if (!WaitWithGate(impl_->loop.retryDelayMs)) return false;   // 可被暂停/停止打断
    }
    return false;
}
```

---

## 八、信号与 UI 反馈

**`schemeFinished` 只在「全部循环完成」时发一次**（每轮结束发新信号 `cycleChanged`）。

理由：① `AutoRunPage::OnSchemeFinished` 会置「✅ 完成」、`ProcessPage:899` 会置终态并 `ResetStepSession`——每轮都发会导致**机器还在动、UI 却显示"完成"**，操作员可能据此伸手取料；②「方案执行完成」的历史语义 = 会话结束，循环只是会话内部重复，不应改变会话级信号；③ 无限循环下 `schemeFinished` 永不发（只能被停止 → interrupted），语义自洽。

| 反馈 | 载体 | 内容 |
|---|---|---|
| 计数 | `cycleChanged(int,int)` → `m_cycleLabel` | `循环 3/10` / `循环 3/∞` |
| 日志 | `logMessage`（既有 `OnLogMessage`，1000 行上限已防膨胀） | `=== 循环 3/10 开始 ===` / `完成 ===` / `循环间隔：回安全位（先抬 Z 再水平）` |
| 终态 | 既有 `schemeFinished` / `interrupted` / `errorOccurred` | 「✅ 完成」/「⏸ 中断」/「⛔ 错误」，**文案与 8.17/8.5/8.7 完全一致** |

---

## 九、安全与风险

| 风险 | 现有防护 | 本方案新增门禁 |
|---|---|---|
| 撞软限位 | `MoveAbs` 内全轴硬拦截（`HardwareManager.cpp:418`）→ 越界即失败 → Fault → 循环自动终止 | 无需新增（已是硬保护） |
| 无限循环误触 | — | **二次确认弹窗**（标题「确认无限循环」：急停在手边 / 工作区无人员肢体 / 软限位已核对）；提示条常驻「循环运行中，停止请点 ■」 |
| 跨轮横扫 | TR-074 安全位 | **启动门禁**：未示教安全位 + 勾选回安全位 → 拒绝启动循环（§五.1） |
| 循环期间改参数 | 循环控件 + 方案下拉置灰；`ReloadFromConfig` 被拒 | 安全位数值**启动时快照**（中途不生效） |
| 视觉失败 | 失败即 Fault（retryCount=0） | 不自动重试；无人值守前须确认供料稳定 |
| 连续运行发热丢步 | 无 | 次数上限 9999；不建议无限连续运行超过班次；无温度反馈前不做自动降速（缺硬件依据） |
| 日志膨胀 | `m_logTextEdit` 1000 行上限 | 每轮仅 2~3 行，长循环不爆内存 |

**待确认运维项**：`log/creampuff.log` 的轮转策略（若 24h 无限连续运行，需确认日志按日期/大小轮转，否则磁盘风险）——**这是真机长循环前唯一需补确认的项**。

**明确不做（避免过度设计）**：循环间隔额外延时配置（用方案里的 Delay 动作即可）、断点续产、产量统计与班次、每轮重载 config、循环内自动重试（本次只留配置口子）、完成后自动回安全位、循环期间开互锁后门、重试次数进 UI。

---

## 十、回归用例（实施后同步进 `doc/test/real_machine_plan_phase3.md`）

**阶段 8 追加 8.18–8.26：**

| # | 动作 | 期望 |
|---|---|---|
| 8.18 | **零回归核心**：不碰循环控件，直接启动 | 与 8.17 完全一致：「✅ 完成」出现**一次**；无 `=== 循环` 日志；`m_cycleLabel` 显示「单轮」 |
| 8.19 | 循环=指定次数 N=3 启动 | 日志 `=== 循环 1/3 开始 ===`…`=== 循环 3/3 完成 ===`；标签依次 `循环 1/3`→`2/3`→`3/3`；**「✅ 完成」只出现一次** |
| 8.20 | ★已示教安全位 → N=2 → 观察第 1 轮结束瞬间 | 日志「循环间隔：回安全位…」+「移动 → 点 1 (抬Z)」；**第 1 段只有 Z 动**（J1/J2/R 不变 ±0.1°），第 2 段才动 J1/J2/R；速度≈50%；**工艺流程页列表选中行不跳到第 0/1 行** |
| 8.21 | ★循环运行中（水平段）点「■ 停止」 | 状态「⏹ 停止中…」→「⏸ 中断」，日志「中断: 用户停止」；**不再进入下一轮**；使能保持 |
| 8.22 | ★循环中记下标签值 X → 点「⏸ 暂停」→「▶ 启动/继续」 | 从暂停处原地续走（J1 单调趋近、无反向回摆）；**标签仍为 X**（计数不丢） |
| 8.23 | ⚠前置（不可达点，同 8.7）循环 N=3 启动 | 第 1 轮即 IK 失败 → 「⛔ 错误」；**不进入第 2 轮**；清报警后可重启（计数复位为 1） |
| 8.24 | 循环运行中/暂停中查看循环控件 | 模式下拉、次数框、安全位复选框**全灰**（与方案下拉同规则）；运行中修改无效 |
| 8.25 | ⚠前置：safePos 未启用 → 循环 N=2 + 勾选回安全位 → 启动 | **拒绝启动，无运动**；提示「循环运行需先示教安全位…或取消勾选『循环间隔回安全位』」（黄） |
| 8.26 | 循环=无限循环 → 点启动 | 弹二次确认框；**取消**→不启动；**是**→标签「循环 1/∞」+ 日志 `=== 循环 1/∞ 开始 ===`；点「■ 停止」可终止 →「⏸ 中断」；**无 schemeFinished** |

**阶段 10 追加 10.21–10.26：**

| # | 动作 | 期望 |
|---|---|---|
| 10.21 | ★循环运行中点开关切【手动】 | 弹窗 + 回弹（**与 10.6 同一文案**）；日志 `模式切换被拒：执行中 autoMode=0 state=1`；连点两次各弹一次 |
| 10.22 | 循环运行中切「🎥 视觉检测」→ 切回自动页 | 可切（白名单内）、参数只读；**切回后标签继续计数**（上下文不丢） |
| 10.23 | ★循环运行中点顶栏【急停】 | 断使能 + 中断；五钮全灰、状态「⛔ 急停」；其后 10.14→10.17 恢复链（使能→回零→手动回安全位→切自动可启动，TR-081）**仍成立** |
| 10.24 | 循环运行期间到工艺流程页点「单步执行」/「执行选中动作」 | 被拒（`rejected: already running`）；方案切换/删除弹窗拒绝 |
| 10.25 | ★循环运行期间到设备配置页改 L1 保存 | 日志 `[SequenceWorker] ReloadFromConfig skipped: scheme running`；循环不受影响、不卡死 |
| 10.26 | 循环全部完成（Idle）后切【手动】 | 放行无弹窗（与 10.8/10.9 一致） |

> phase3 第七节「测试记录」增一行「循环生产（8.18-8.26 / 10.21-10.26）」；工时 +0.5 天（实现 0.3 + 真机 0.2）。

---

## 十一、实施步骤（每步 Debug+Release 编译 + Sim 冒烟）

| 步 | 内容 | 涉及文件 | 验证点 |
|---|---|---|---|
| S0 | 本方案审核定稿（D1–D12） | 本文档 | — |
| S1 | `LoopConfig`、`cycleChanged` 信号、`RunSequence` 默认参数、Impl 成员、私有方法**声明**（不实现） | `src/Logic/SequenceWorker.h` | 默认参数 → 既有 3 个调用点零改动，编译必过 |
| S2 | `ExecuteActions`→`ExecuteOnce`；新增循环外壳；抽 `BuildSafePosActionFrom`；`ReturnToSafePosInline`；`RunSequence` 存 loop + 捕获 safePos；`RunSingleAction`/`ClearFault` 清 loop | `src/Logic/SequenceWorker.cpp` | **单轮零回归冒烟**：Sim 跑一遍 →「✅ 完成」一次、无循环日志（等价 8.18） |
| S3 | 循环控件行 + `m_cycleLabel` + `OnCycleChanged` + 启动组装 LoopConfig + 两项门禁（安全位未配置拒绝 / 无限循环二次确认）+ `UpdateControlsEnabled` 置灰 | `src/UI/AutoRunPage.h/.cpp` | Sim：单轮=现状；N=3 计数与终态正确；运行中控件置灰 |
| S4 | 持久化（`production.loop.*` 读写，不 emit paramsChanged） | `src/UI/AutoRunPage.cpp` | 重启后控件保持上次选择；config.json 自动出现 `production` 节点 |
| S5 | 确认 `MainWindow` 零改动；跑 10.2/10.6/10.13 子集 | — | 无回归 |
| S6 | Sim 全量冒烟（8.18-8.26 / 10.21-10.26 可仿真子集） | — | Sim 无相机：视觉相关跳过 |
| S7 | 文档回填：phase3 阶段 8/10 追加用例；`TEST_RECORD.md` 追加新 TR；`AGENTS.md` 记新需求 | 3 个 md | 引用一律 `TR-###` |

**预估工时**：实现 0.3 天 + 真机用例 0.2 天。

**台账编号**：`real_machine_plan_phase3.md` 原预分配 TR-076/077/078 = 阶段 8/9/10，**因执行过程中的两个回归缺陷（TR-076 单步失效、TR-077 执行选中动作破坏会话）先占用 TR-076/077，阶段 8 已记账 TR-078**（缺陷顺延 TR-079 起）。本循环需求**单开新号**（建议 **TR-080** 起）；追加前**重读 `TEST_RECORD.md` 取当前最大号 +1**（并行会话抢号教训），禁止重排/复用。

---

## 十二、决策清单（待用户 / Gemini 审核）

| # | 决策点 | 结论 |
|---|---|---|
| D1 | 循环形态 | **单轮 / 指定 N 次 / 无限 三挡，默认单轮**（✅ 采纳推荐） |
| D2 | 循环间隔回安全位 | **默认开；未示教安全位时拒绝启动循环**（✅ **用户已拍板**） |
| D3 | 无限循环二次确认弹窗 | **要**（标题「确认无限循环」） |
| D4 | 动作失败处理 | **默认不重试（retryCount=0）；保留配置口子 N**（✅ **用户已拍板，并要求留口子**） |
| D5 | `schemeFinished` 时机 | **全部循环完成后只发一次**；每轮发 `cycleChanged` |
| D6 | ■ 停止 | **立即终止整个循环**（✅ **用户已拍板**） |
| D7 | 循环期间切手动 | **保持禁止（TR-075 不开后门）**；缓解 = 视觉页观察 / 先停止 |
| D8 | 全部循环完成后回安全位 | **不回**（保持与今天一致的停机位，便于取料） |
| D9 | TR 编号 | **本需求 TR-080 起**（当前最大 TR-079：阶段 8 验证 TR-078、9.4 缺陷 TR-079 已入账） |
| D10 | 循环参数持久化 | **是**（仅持久化 UI 选择，不参与门禁） |
| D11 | 次数上限 | **1..9999**（SpinBox 限） |
| D12 | 死代码 `PickCycleController` | **本次不动**（避免与循环功能混在一条提交） |

**待确认（无阻塞）**：真机验证时循环间隔回安全位的两段走位是否与 9.6 完全一致（同一代码路径，预期一致）；`log/creampuff.log` 轮转策略（§九）。
