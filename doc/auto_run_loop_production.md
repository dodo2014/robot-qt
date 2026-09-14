# 自动运行循环生产（Loop Production）需求方案

> 状态：**已实施（代码完成，待真机验证）**——2026-09-14 定稿并当日实施完毕，台账 **TR-091**（🟡 代码完成待真机）。Debug+Release 双编译 EXITCODE=0 + Sim 冒烟 `SMOKE_INIT_COMPLETE=True`；**Sim 无法自动驱动 UI 起停**，8.18–8.26 / 10.21–10.26 需真机执行。
> 实施落点：`src/Logic/SequenceWorker.h/.cpp`（S1/S2/S2b/S2c）、`src/UI/AutoRunPage.h/.cpp`（S3/S4）、`src/main.cpp`（S8）；`MainWindow` / `ProcessPage` / `ManualControlPage` / `HardwareManager` **零改动**（S5 已确认）。**实施偏差一处**：`Impl` 成员命名为 `loopCfg` 而非本文档 §4.1 写的 `loop`（`Impl` 内已有局部 `QEventLoop loop`，同名触发 C4458），以 TR-091 为准。
> 修订：**2026-09-14 定稿**（对齐 09-09/09-10 落地现状 + 用户裁决）——① §一 行号全线校正、补 TR-083/084/088/090 四条新事实；② §七 代码片段符号修正（原文引用了两个不存在的函数）+ `errorOccurred` 单点化定稿（D17 方案 A）；③ 台账编号由 TR-080 改为 **TR-091** 起；④ 用例编号口径统一（见 §十）；⑤ 新增 **§4.4「严格先抬 Z」保证链**（D19）；⑥ 用户裁决：不做常驻提示条（D13）、循环期间一键回零不可达（D14）、术语统一「回安全位」（D15）、日志保留 7 天（D16）、**循环控件先按一行实现、布局待定**（D18，**TR-072 方案已废弃**）。
> 来源：真机验收阶段 8 用例 **8.4** —— 用户问「自动运行是单次运行？不是循环运行？」，核实为只跑单轮；用户裁决**需要连续循环生产**。
> 关联：`doc/test/real_machine_plan_phase3.md`（阶段 8/9/10）、`doc/auto_run_button_refactor.md`（TR-073）、`doc/global_safe_position.md`（TR-074）、`doc/ui_interlock_plan.md`（TR-075）、`doc/architecture.md`（§5 关键不变量：观察者指针一律 `QPointer`，TR-084）

---

## 一、现状核实（代码事实）

> **行号基准：2026-09-14**（`SequenceWorker.cpp` 802 行）。09-09 之后 TR-083/TR-088 追加代码使本文档初稿行号整体下移 4~7 行，下表已逐条校正。

| 事实 | 位置 |
|---|---|
| 唯一循环 = 单遍动作遍历：`for (int i = 0; i < actions.size(); ++i)` | `src/Logic/SequenceWorker.cpp:445-487`（函数 `ExecuteActions` `:442-489`） |
| 跑完由 `StartExecution` 发 `schemeFinished` + `SetState(Idle)`，**无回绕** | `SequenceWorker.cpp:396-409`（`schemeFinished` `:400`） |
| `RunSequence` 启动时无条件重置：cancel/stepGo/paused/stepGatePending/pauseAccumMs/safeSession/**currentIndex=-1**/singleSession；**无循环计数变量** | `SequenceWorker.cpp:150-190`（重置段 `:169-181`） |
| **`stepMode` 不在 `RunSequence` 清**——清除点在**会话结束**（`StartExecution` `:405`、`RunSingleAction` `:222`）；入口清会吃掉调用方标志（TR-076） | `SequenceWorker.cpp:174-177`、`:405`、`:222` |
| config.json 无 `loop/cycle/repeat` 键；三页 UI 无循环开关/次数输入 | 已 grep 确认（2026-09-14 复验） |
| `PickCycleController`（视觉抓取单周期）为**死代码**（仅 CMakeLists 引用），其 StartCycle 亦为单次 | `src/Logic/CMakeLists.txt:6,11` |
| 暂停 = 保留上下文原地续走（PauseGate + reissue 当前绝对目标 MoveAbs） | `SequenceWorker.cpp:629-664`（reissue 构造 `:586-593`、Resume 重发 `:657-660`） |
| 停止 = `StopImmediate` = cancel + `StopAllAxes` 减速停（保持使能），废弃上下文 | `SequenceWorker.cpp:308-314` |
| `ConfigManager` 无信号；`paramsChanged` 属 ConfigPage，唯一消费者是 `ReloadFromConfig`（重建 Kinematics） | `src/Config/ConfigManager.*`、`src/UI/MainWindow.cpp:165` |
| `ConfigManager::getValue` 非线程安全；worker 线程内**从不**读 config（读取全在主线程） | `SequenceWorker.cpp:111-148`、`:238-292` |
| `MoveAbs` 对所有轴强制软限位拦截（越界 return false，不夹紧） | `src/HAL/core/HardwareManager.cpp:419-424` |
| `ReloadFromConfig` 在 running 时跳过（防主线程写/worker 读竞争） | `SequenceWorker.cpp:113-118` |
| **（新增）一键回零会终止残留会话**：`PrepareHomingSession` 在 `HomeAxis` **之前**调用——`Paused` 态 `Stop()` 终止会话并放行回零，`Fault` 态不动锁存仅预告，`Running` 态拒绝回零 | `src/UI/ManualControlPage.cpp:513`、`:560`（TR-083） |
| **（新增）安全位会话完成态特判**：`AutoRunPage` 在 `actionStarted` 时快照 `IsSafePosSession()`，完成槽按标志分流显示「✅ 已回安全位」 | `src/UI/AutoRunPage.cpp:530-532`（TR-088） |
| **（新增）停止减速度系数**：每轴 `axes.<轴>.stopDecSmooth`（0=不下发保持卡默认 0.5），`MoveAbs/MoveJog` 前实时读 config + 按已下发值去重 | `src/HAL/motioncard/BoPaiCard.cpp:491-500`、`src/HAL/core/AxisConfigService.cpp:61-66`（TR-090；J1/Z 已置 1.0） |
| **（新增）观察者指针规范**：跨线程/长生命周期 QObject 的观察者一律 `QPointer`，禁用裸指针长期持有（新增信号 `cycleChanged` 的连接须遵此） | `doc/architecture.md` §5 关键不变量（TR-084） |

**不可破坏的既有语义**：暂停=续走、停止=废弃上下文、开环步进禁断脉冲（MC_Stop 减速斜坡）、MoveAbs 绝对坐标不累积误差、程序不自动使能、初始化不含回零。

---

## 二、功能形态

**三挡可配：`单轮 / 指定 N 次 / 无限`，默认「单轮」。**

- 生产语义：泡芙抓取是按盘/按箱批量作业，N 次是刚需；无限循环保留给连续流水线。
- **默认单轮 → 与今天行为逐字节一致，TR-073 零回归**（8.1–8.17 全部保持有效）。
- 无限循环需**二次确认弹窗**（见 §九）。

### UI 入口：AutoRunPage（方案下拉下方新增「循环」行）

```
[运行方案:  ▾ 抓取流程                    ]        ← 现有
[循环:      ▾ 单轮|指定次数|无限循环 ] [10]次  [x] 回安全位        循环 3/10
```

| 控件 | 成员 | 默认 | 说明 |
|---|---|---|---|
| 模式下拉 | `m_loopModeCombo` | 单轮（index 0） | 0=单轮 / 1=指定次数 / 2=无限 |
| 次数框 | `m_loopCountSpin` | 10（1..9999） | 仅「指定次数」时 visible |
| 回安全位 | `m_loopSafeChk` | **勾选 true** | 显示文本统一为「回安全位」（见 §五；勿与 config 总开关「启用安全位」混称——TR-082） |
| 进度标签 | `m_cycleLabel` | `单轮` / `循环 0/10` | 运行时 `循环 3/10`、`循环 3/∞` |

- ConfigPage **不新增**控件（单一数据源，避免双写冲突）。
- 循环控件与方案下拉**同规则**：`m_schemeCombo->setEnabled(!busy)`——即**只**按 `busy(Running/Paused)` 置灰，**与 `gate`（自动模式 + 无急停锁存）无关**（TR-075 起方案下拉已脱离 gate，手动模式保留预选能力）。置灰走 `UpdateControlsEnabled` 唯一出口，防"跑到第 5 轮改次数"的竞态。
- **重试次数不进 UI**（见 §七，仅 config 配置项，默认 0）。
- **布局：先按一行实现，最终布局待定**（2026-09-14 用户裁决）。循环控件（模式下拉 / 次数框 / 「回安全位」复选框 / 进度标签）**排在同一行**；**TR-072 的坐标面板方案已废弃**——用户已手动改过该面板，本文档不再引用。
- **硬约束（10 寸触控屏）**：目标屏为 **10 寸触摸屏**，且**窗口缩到最小尺寸时界面不得错乱**（不折行、不裁切、不重叠）；控件触控命中区按手指操作适配（下拉/复选框不建议小于 32px 高）。先出一行版本的实际效果，再由用户决定是否调整布局。

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
    bool ExecuteActionWithRetry(const ActionData& act, int idx);  // 动作级重试包装（内部转调 ExecuteAction）
    // Impl 新增：LoopConfig loop; int cycleIndex = 0; bool safePoseValid = false;
    //           double safeJ1/J2/Z/R;   // 主线程启动时快照
```

**retry 的接线点（初稿此处自相矛盾，已澄清）**：`ExecuteActionWithRetry` 是**唯一**在 `ExecuteOnce` 内被改写的调用点——`ExecuteOnce` 里原 `ExecuteAction(action, i)` 一处改为 `ExecuteActionWithRetry(action, i)`，其余（cancel 检查 / PauseGate / currentIndex / actionStarted / 失败→Fault 锁存 / 单步挂起）原样搬迁。`retryCount = 0`（默认）时该包装**直通** `ExecuteAction`，零额外行为 → 与今天逐字节一致。

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

- `ExecuteOnce()` = 现 `ExecuteActions` 主体**原样搬迁**（cancel 检查 / PauseGate / currentIndex / actionStarted / 失败→Fault 锁存 / 单步挂起），**仅动作调用点一处**改为 `ExecuteActionWithRetry(action, i)`（见 §4.1 末注），其余一行不改。
- `RunSequence` 增量：`impl_->loop = loop; cycleIndex = 0;` + **主线程捕获安全位数值**（`safePos.enabled` 为真时读 j1/j2/z/r 存 Impl），规避 worker 读 config。注意 `RunSequence` 现有重置段（`:169-181`）**不含** `stepMode`（TR-076），新增的 `loop`/`cycleIndex` 属"会话级状态"，与本段其余项同列清空即可。
- `RunSingleAction`（:191-232）与 `ClearFault`（:370-381）增量清 `loop` / `cycleIndex`（与既有清 `currentIndex`、`singleSession` 同列）。

### 4.3 间隔回安全位（内联，不走 RunSequence）

```cpp
// 三不：不置 safeSession、不发 actionStarted/actionFinished、不重入门禁
// → ProcessPage 动作列表选中行不会被联动跳走（TR-074 的 IsSafePosSession 守卫仍只服务手动触发）
// → AutoRunPage 的安全位会话特判也不会被误触发（TR-088 用 OnActionStarted 快照 IsSafePosSession()；
//   循环内联回安全位不发 actionStarted → 快照恒为 false，完成时仍显示普通「✅ 完成」，语义正确）
bool SequenceWorker::ReturnToSafePosInline()
{
    ActionData act;
    if (!BuildSafePosActionFrom(act, impl_->safeJ1, impl_->safeJ2, impl_->safeZ, impl_->safeR))
        return false;                    // 未启用/未捕获 → 判为故障（不静默跳过）
    return ExecuteMove(act);             // 白得：cancel 检查、PauseGate、reissue 续走、逐点日志
}
```

`BuildSafePosActionFrom` = 从 `RunSafePos`（`:238-292`）抽出的参数化公共构造：安全位数值由入参给（循环用主线程快照；`RunSafePos` 传 config 现值），**当前位仍用 `InMainThread(GetPosition)` 实时读** → 点1 始终是"原地抬 Z"。`RunSafePos()` 改为调用同一函数，逻辑零重复。

### 4.4 「严格先抬 Z 到位，再做水平移动」保证链（D19，2026-09-14 专项确认）

> 适用范围：**TR-074 手动/自动回安全位** 与 **循环间隔回安全位**（两者共用同一构造与执行路径）。用户要求"确认保证"，故逐环节核实并补一处加固。

**① 结构保证（已核实，点粒度严格串行）**

| 环节 | 事实 | 位置 |
|---|---|---|
| 点串行 | `ExecuteMove` 对 `action.points` **逐点循环**，每个点执行完（`MoveToPoint` 返回）才进下一个点 | `SequenceWorker.cpp:506-518` |
| 点内等待 | `MoveToPoint` 在**下发后阻塞等待全部参与轴到位**（超时 30s）才返回 | `:594`（`WaitForAxes`，`:604-627`） |
| 抬升量 | 点1 = `{J1/J2/R = 当前值, Z = safeZ}`；点2 = 安全位 `{sj1, sj2, sz, sr}` → **水平位移只可能发生在 Z = sz 这一高度上** | `:262-282` |

⇒ 点2 的水平 `MoveAbs` 下发**必然晚于点1 的全部参与轴判为到位**；且安全位动作的 Z 值在两点的 `j3/z` 字段上一致（点1 已抬到 `sz`），水平段不会从低位起扫。

**② 到位判定的性质（必须写明，不能含糊）**

`IsAxisBusy`（`:705-710`）判的是 `axisBusyUntilMs_` 时间戳，由 `MarkAxisBusy` 写入 `busyMs = 距离/速度×1000 + 200`（`:444`）——**即"预计运动时长 + 200ms 余量"，不是位置反馈确认**（开环步进无编码器；全项目所有 `MoveAbs` 共用此判定）。故严格表述是：**Z 预计到位 + 200ms 余量后，才下发水平移动**。

**③ 残余窗口 + 加固（本方案采纳）**

残余风险：点1 会把 J1/J2/R **重新下发为读回的当前值**。卡轴（J1/Z）读回的是指令脉冲位置 → 差值恒为 0；但 **J2/R 是舵机，读回的是上报角度**，减速停后可能与指令位置有微差 δ → 点1 会伴随 δ 量级的水平微动，而此时 Z 正在抬升——正是要防的场景（低位水平位移）。

**加固（纳入实施，见 S2c）**：`MoveToPoint` 在下发前，对**目标与读回当前值之差在 ε 内的轴跳过下发**（建议 ε = 0.01mm / 0.01°）。于是：

- 点1 的实际下发**只剩 Z 轴** → "抬 Z 阶段零水平运动"由代码保证，与读回值无关（不再依赖 δ≈0 的假设）；
- 被跳过的轴不写 busy 时间戳 → `IsAxisBusy` 立即返回 false，`WaitForAxes` 不受影响；
- ε 与既有 `hasJoints` 的 ±0.5 一致性校验量级无关，不影响其它动作的正常下发；对"目标=当前位置"的零位移指令本就无运动意义，跳过无副作用；
- 实施时该分支写 `SPDLOG_INFO`（形如 `skip axis X (in position)`），**不写 UI 日志**避免噪声——真机 8.20 即用该日志核验"点1 只下发 Z"。

**④ 未采纳的更重方案（留档）**：① 点1 与点2 之间插固定延时（如 200ms）——治不了"J1/J2/R 被重下发"这一根因，且白增节拍；② 把 Z 到位改为读编码器/限位确认——硬件无此反馈（开环），不可行。

---

## 五、循环间行为

### 5.1 是否回安全位 —— **回（用户已拍板，默认开）**

- **时机**：只在**进入下一轮之前**回；**第 1 轮之前不回**（操作员已把机器停在起始位，多走是干扰），**最后一轮之后不回**（保持停机位便于取料）。
- **理由**：结束位（放料位）直奔下一轮第一步起点是多轴同时 `MoveAbs`（`SequenceWorker.cpp:576-579` 依次下发 J2/R/J1/Z，无中间等待），会横扫桌面撞料/撞夹具——这正是 TR-074 存在的原因。回安全位把跨轮大位移拆成"抬 Z → 水平 → 下一轮第一段"。
- **未启用安全位（`kinematics.safePos.enabled=false`）时拒绝启动循环**并提示：「循环运行需先示教安全位（手动控制页『示教安全位』），或取消勾选『回安全位』」。与"未回零禁止启动"同构，且可用配置一键放行。
- **术语（TR-082）**：`kinematics.safePos.enabled` 是**总开关**——同时管「回零后自动回安全位」与手动页「回安全位」两个入口，且兼作"已示教过"标记；设备配置页显示名已改为**「启用安全位」**。循环行的复选框 `production.loop.returnToSafePos` 只决定**循环间隔**是否回，显示文本统一为**「回安全位」**。两者不可混称。

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
| 3 | **急停** | cancel + 断使能 → 下一检查点 interrupted 退出；`estopPending_` 锁存 → 必须"使能 + 回零"解锁。**恢复链按 TR-081 口径 = 使能→回零→手动回安全位→切自动**（急停不清已回零轴 `homed`，恢复回零无上升沿 → 不自动回安全位），循环场景同 10.23。**急停后禁止续跑循环**（坐标不可信） |
| 4 | **动作失败 → Fault** | 终止整个循环并锁存（不自动重试，retryCount=0 时）；「⛔ 错误」+ 仅「↺ 清报警」可点。理由：无人值守下自动重试可能重复抓取/撞料 |
| 5 | **模式互锁（循环期间禁止切手动）** | **保持禁止（TR-075 不开后门）**。缓解：① 自动模式白名单含视觉检测页，可切过去观察（只读）；② ■停止全程可点，停止后即可切手动；③ Fault 态放行切手动脱困（10.8 规则），故障停机不会把人锁死 |
| 6 | **单步 `RunSingleAction`** | 不受影响（走 `StartSingleExecution` → 直接 `ExecuteAction`，不经过循环外壳）；循环运行时被 `running` 门禁拒绝，与今天一致 |
| 7 | **方案切换 / 下拉** | 不受影响（`IsExecutionActive()` 为真 → 工艺页弹窗拒绝；AutoRunPage 下拉按 `!busy` 置灰） |
| 8 | **安全位会话（TR-074）** | `RunSafePos()` 走默认单轮参数，行为不变；内联回安全位不发 `actionStarted`（ProcessPage 列表不误选，AutoRunPage 的 TR-088 安全位特判也不会误触发）；循环期间手动「回安全位」仍被 `ManualControlPage.cpp:589`（state 分支 `:595-620`）拒绝 |
| 9 | **`ReloadFromConfig`** | 循环期间被拒（已有 WARN）→ 中途改 L1/TCP 不重建 Kinematics，无并发 UB |
| 10 | **UI 可达性** | 循环必在自动模式启动（按钮 gate），故**暂停/停止按钮全程可用**，不存在 phase3 §0-B「提示不可达」问题。循环控件本身按 `!busy` 置灰（同方案下拉，与 gate 无关） |
| 11 | **循环期间点「一键回零」（2026-09-14 用户裁决）** | **不可达，无需新增门禁**：一键回零按钮在**手动控制页**，而循环运行期间模式互锁禁止切回手动页（TR-075 导航白名单仅放行【自动运行】【视觉检测】，且运行期切模式弹窗+回弹）→ 按钮物理上点不到。TR-083 的 `PrepareHomingSession`（Paused 终止会话 / Running 拒绝回零）因此不落在循环路径上；仅 Fault 态可切手动脱困，而那时循环已终止 |

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
| 重试期间反馈 | **只写 `logMessage`，不发 `errorOccurred`**（避免 UI 误报"错误"、状态跳 ⛔）——依赖 `errorOccurred` 单点化，见下方 D17 |
| 重试耗尽 | 发 `errorOccurred` + `SetState(Fault)` → 终止整个循环 |
| 可中断性 | 重试等待**必须**用 `ExecuteDelay`（`:744-764`）同构的 `for(;;){ cancel → false; paused → PauseGate() }` 结构 → 暂停/停止/急停同样生效。**注意：`Impl::WaitForCancelOrTime`（`:68-83`）只处理 cancel、不处理 paused，不可用于此处** |
| UI | **本次不进 UI**（仅 config 手改），保持界面简洁；待真机验证确有需求再开口 |

```cpp
// 重试等待：与 ExecuteDelay 同构（可被暂停/停止/急停打断），暂停时长不计入间隔预算
bool SequenceWorker::WaitRetryDelay(int ms)
{
    QElapsedTimer t; t.start();
    long long pauseAccum = 0;
    for (;;) {
        if (impl_->cancel.load()) return false;
        if (impl_->paused.load()) {
            const long long before = t.elapsed() - pauseAccum;
            if (!impl_->PauseGate()) return false;
            pauseAccum = t.elapsed() - before;
            continue;
        }
        if (t.elapsed() - pauseAccum >= ms) return true;
        QThread::msleep(20);
    }
}

bool SequenceWorker::ExecuteActionWithRetry(const ActionData& act, int idx)
{
    if (impl_->loop.retryCount <= 0) return ExecuteAction(act, idx);   // 默认路径：直通，零开销、零行为变化
    const int maxTry = impl_->loop.retryCount + 1;
    const bool retryable = (act.type == ActionType::Vision || act.type == ActionType::Move);
    for (int attempt = 1; attempt <= maxTry; ++attempt) {
        if (ExecuteAction(act, idx)) return true;             // 成功（真实函数名，非 ExecuteActionOnce）
        if (impl_->cancel.load()) return false;               // 停止/急停：不重试
        if (!retryable || attempt == maxTry) break;           // 不可重试 或 已耗尽 → 上层 Fault
        emit logMessage(QStringLiteral("动作「%1」失败，%2ms 后重试（%3/%4）")
            .arg(act.name).arg(impl_->loop.retryDelayMs).arg(attempt).arg(maxTry - 1));
        if (!WaitRetryDelay(impl_->loop.retryDelayMs)) return false;   // 可被暂停/停止打断
    }
    return false;   // 本函数**不 emit**——失败原因留在 impl_->lastError，由上层单点发出（见下）
}
```

**统一 emit 点（D17 方案 A 的关键：`ExecuteAction` 子树内一处都不 emit）**

```cpp
// ExecuteOnce 失败分支（现 :460-470 的 else 段，唯一改动见 ↓ 标注）
} else {
    // 故障锁存：保留上下文（scheme / currentIndex），待 ClearFault 才回 Idle；
    // 期间 RunSequence 会因 state != Idle 被拒，UI 也能据此禁用「启动」
    SetState(WorkerState::Fault);
    // 【改】原为 emit errorOccurred(action.name)：改用 lastError（具体原因，含坐标/轴号），
    //       为空才回退动作名——修掉"具体原因被 action.name 覆盖"的既有瑕疵
    emit errorOccurred(impl_->lastError.isEmpty() ? action.name : impl_->lastError);
}

// StartSingleExecution 兜底分支（现 :424-430）同样改写为上面这一句（单动作会话共用 ExecuteAction）
```

**D17 已定稿：`errorOccurred` 单点化（方案 A，2026-09-14 用户拍板）**

问题：`ExecuteAction` 及其子函数内部有 **8 处** `emit errorOccurred`（`:501/559/598/686/718/737/776/799`），失败瞬间即发；而 `AutoRunPage::OnError` 会把状态标签置「⛔ 错误」+ 红字、`ProcessPage` 会置「✖ 出错」并 `ResetStepSession` → **重试还没开始，UI 已经报故障**，与上表「重试期间不发 `errorOccurred`」直接冲突。故本次一并做**失败原因单点化**：

| 步 | 内容 |
|---|---|
| ① | `Impl` 新增 `QString lastError`（仅 worker 线程内写/读，不跨线程共享，无需加锁） |
| ② | `ExecuteAction` 子树 8 处 `emit errorOccurred(...)` 改为 `impl_->lastError = ...;` 后 `return false`（字符串原样保留，含坐标/轴号） |
| ③ | 每次进入动作前（`ExecuteOnce` 循环内 `emit actionStarted` 之前）清空 `lastError`，防上一动作的陈旧原因被重发 |
| ④ | **统一 emit 点收敛为 2 处**（均在 `ExecuteAction` 之外，按会话类型各一）：`ExecuteOnce` 失败分支（`:467`）、`StartSingleExecution` 兜底分支（`:430`）——两处均发 `impl_->lastError`，为空时回退 `action.name`。**`ExecuteActionWithRetry` 本身不 emit**（耗尽后把原因留给上层） |

- **`retryCount = 0`（默认）行为零变化**：仍是"失败 → 发一次 `errorOccurred` + `SetState(Fault)`"，文案与今天一致；`retryCount > 0` 时重试期间只写 `logMessage`，**仅在耗尽后**才发 `errorOccurred` → 上表规则天然成立。
- **顺带修掉一处既有小瑕疵**：今天失败会发**两条** `errorOccurred`（内部具体原因 + 外层 `action.name`），`OnError` 里 `m_lastError` 被后者覆盖 → 清报警提示丢失具体原因；单点化后只发具体原因（`lastError` 非空即用）。
- **影响面**：`SequenceWorker.cpp` 内 8 处改写 + 2 处统一 emit 点；`AutoRunPage` / `ProcessPage` **零改动**（信号语义不变，仅条数与文案来源变化）。
- **回归验证点**：任一动作失败 → UI 显示**具体失败原因**（不再被 `action.name` 覆盖）；`retryCount=0` 下与今天逐字节一致。实施见 §十一 **S2b**。

---

## 八、信号与 UI 反馈

**`schemeFinished` 只在「全部循环完成」时发一次**（每轮结束发新信号 `cycleChanged`）。

理由：① `AutoRunPage::OnSchemeFinished` 会置「✅ 完成」、`ProcessPage:903` 会置终态并 `ResetStepSession`——每轮都发会导致**机器还在动、UI 却显示"完成"**，操作员可能据此伸手取料；②「方案执行完成」的历史语义 = 会话结束，循环只是会话内部重复，不应改变会话级信号；③ 无限循环下 `schemeFinished` 永不发（只能被停止 → interrupted），语义自洽。

| 反馈 | 载体 | 内容 |
|---|---|---|
| 计数 | `cycleChanged(int,int)` → `m_cycleLabel` | `循环 3/10` / `循环 3/∞` |
| 日志 | `logMessage`（既有 `OnLogMessage`，1000 行上限已防膨胀） | `=== 循环 3/10 开始 ===` / `完成 ===` / `循环间隔：回安全位（先抬 Z 再水平）` |
| 终态 | 既有 `schemeFinished` / `interrupted` / `errorOccurred` | 「✅ 完成」/「⏸ 中断」/「⛔ 错误」，**文案与 8.17/8.5/8.7 完全一致** |

---

## 九、安全与风险

| 风险 | 现有防护 | 本方案新增门禁 |
|---|---|---|
| 撞软限位 | `MoveAbs` 内全轴硬拦截（`HardwareManager.cpp:419-424`）→ 越界即失败 → Fault → 循环自动终止 | 无需新增（已是硬保护） |
| 无限循环误触 | — | **二次确认弹窗**（标题「确认无限循环」：急停在手边 / 工作区无人员肢体 / 软限位已核对）。**不做常驻提示条**（2026-09-14 用户裁决）——`m_hintLabel` 是单槽覆盖式且无优先级守卫，常驻文案必被其它 `SetHint` 吞掉（TR-087 同类实测）；循环进度改由 `m_cycleLabel` 常显即可 |
| 跨轮横扫 | TR-074 安全位 | **启动门禁**：未**启用**安全位 + 勾选「回安全位」→ 拒绝启动循环（§五.1） |
| 循环期间改参数 | 循环控件 + 方案下拉按 `!busy` 置灰；`ReloadFromConfig` 被拒 | 安全位数值**启动时快照**（中途不生效） |
| 视觉失败 | 失败即 Fault（retryCount=0） | 不自动重试；无人值守前须确认供料稳定（重试口子见 §七 / D17） |
| 停止/暂停滑行距离 | `stopDecSmooth` 每轴可配（TR-090，J1/Z 已置 1.0），⏸/■ 停距随之变化 | 用例 8.21/8.22 的观察值随标定档位变化，验收以**当前 config 档位**为准 |
| 连续运行发热丢步 | 无 | 次数上限 9999；不建议无限连续运行超过班次；无温度反馈前不做自动降速（缺硬件依据） |
| 日志膨胀 | `m_logTextEdit` 1000 行上限（`AutoRunPage.cpp:177`） | 每轮仅 2~3 行，长循环不爆内存 |

**运维项（2026-09-14 已结案并落地）**：`log/` 下日志为**按日期分文件**（曾有 35 个 `creampuff_YYYY-MM-DD.log`，最早 `2026-07-27`），无需按大小轮转；**保留策略定为 7 天**（用户裁决）。✅ **已实现（S8）**：`src/main.cpp` 启动时 `PurgeOldLogs(logDir, 7)`——只匹配 `^creampuff_YYYY-MM-DD\.log$`、只删**日期早于「今天 − 7 天」**的文件；`creampuff.log`（sink 的当日写入基名）、`crash.txt`、其它文件一律不碰，删除条数写日志。**首次运行实测**：删 29 个过期文件（cutoff 2026-09-07），余 09-07 ~ 09-14 共 7 个日期文件 + `crash.txt`。注：spdlog `daily_file_sink` 的 `max_files=30` 参数**实测未生效**（否则不会积到 35 个），故此项必须自己实现。

**明确不做（避免过度设计）**：循环间隔额外延时配置（用方案里的 Delay 动作即可）、断点续产、产量统计与班次、每轮重载 config、循环内自动重试（本次只留配置口子）、完成后自动回安全位、循环期间开互锁后门、重试次数进 UI、循环运行常驻提示条。

---

## 十、回归用例（**已同步进 `doc/test/real_machine_plan_phase3.md`，2026-09-14**）

> 落地位置：phase3 **阶段 8 表后**（`#### 循环生产批次（TR-091，⏳ 待功能实施后执行）`）+ **阶段 10 表后**同名子块；phase3 的 §四 执行顺序、§五 验收标准第 6 条、§六 风险第 15 条、§七 测试记录、§八 工时、§九 回填方式已同步。

> **编号口径（2026-09-14 统一）**：本组用例**直接追加进 phase3 现有编号体系**——阶段 8 接 8.18–8.26、阶段 10 接 10.21–10.26。phase3 实测最大为 **8.17 / 10.20**（已核实），故追加**无冲突**。
> ⚠ **需同步回改的既有记录**：`TEST_RECORD.md` TR-089、`AGENTS.md`（阶段收口里程碑段）、`doc/worklog/2026-09-09.md`、`2026-09-10.md` 四处曾把本组用例记成「独立编号 10.1–10.26 / 10.1–10.23，与 phase3 的 10.x **撞号**」——该表述与本文档实际编号不符，已于 2026-09-14 一并更正为「追加进 phase3：8.18–8.26 / 10.21–10.26，无撞号」。

**阶段 8 追加 8.18–8.26：**

| # | 动作 | 期望 |
|---|---|---|
| 8.18 | **零回归核心**：不碰循环控件，直接启动 | 与 8.17 完全一致：「✅ 完成」出现**一次**；无 `=== 循环` 日志；`m_cycleLabel` 显示「单轮」 |
| 8.19 | 循环=指定次数 N=3 启动 | 日志 `=== 循环 1/3 开始 ===`…`=== 循环 3/3 完成 ===`；标签依次 `循环 1/3`→`2/3`→`3/3`；**「✅ 完成」只出现一次** |
| 8.20 | ★已示教安全位 → N=2 → 观察第 1 轮结束瞬间 | 日志「循环间隔：回安全位…」+「移动 → 点 1 (抬Z)」；**点1 日志出现 `skip axis J1/J2/R (in position)`（S2c 加固生效）→ 点1 实际只下发 Z**；**第 1 段只有 Z 动、J1/J2/R 零位移**，第 2 段才动 J1/J2/R；速度≈50%；**工艺流程页列表选中行不跳到第 0/1 行**。**同一核验同步在手动页「回安全位」（TR-074 9.6）执行**——两条路径共用构造与执行链（§4.4） |
| 8.21 | ★循环运行中（水平段）点「■ 停止」 | 状态「⏹ 停止中…」→「⏸ 中断」，日志「中断: 用户停止」；**不再进入下一轮**；使能保持 |
| 8.22 | ★循环中记下标签值 X → 点「⏸ 暂停」→「▶ 启动/继续」 | 从暂停处原地续走（J1 单调趋近、无反向回摆）；**标签仍为 X**（计数不丢） |
| 8.23 | ⚠前置（不可达点，同 8.7）循环 N=3 启动 | 第 1 轮即 IK 失败 → 「⛔ 错误」；**不进入第 2 轮**；清报警后可重启（计数复位为 1） |
| 8.24 | 循环运行中/暂停中查看循环控件 | 模式下拉、次数框、安全位复选框**全灰**（与方案下拉同规则）；运行中修改无效 |
| 8.25 | ⚠前置：安全位未**启用**（`safePos.enabled=false`）→ 循环 N=2 + 勾选「回安全位」→ 启动 | **拒绝启动，无运动**；提示「循环运行需先示教安全位…或取消勾选『回安全位』」（黄） |
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

> **不设「循环期间一键回零」用例（2026-09-14 用户裁决）**：一键回零按钮在手动控制页，循环期间模式互锁禁止切回手动页（TR-075）→ 该操作**物理不可达**，无需门禁也无需用例。TR-083 的 `PrepareHomingSession`（Paused 终止会话 / Running 拒绝回零）因此不落在循环路径上（详见 §六 #11）。
> **10.23 的恢复链已按 TR-081 口径写明**（使能→回零→**手动**回安全位→切自动），与 phase3 阶段 10 同源；引用时注意本文档 10.x 与 phase3 10.x 为同一编号体系（非"独立编号"）。

> phase3 第七节「测试记录」增一行「循环生产（8.18-8.26 / 10.21-10.26）」；工时 +0.5 天（实现 0.3 + 真机 0.2）。

---

## 十一、实施步骤（每步 Debug+Release 编译 + Sim 冒烟）

| 步 | 内容 | 涉及文件 | 验证点 |
|---|---|---|---|
| S0 | ~~本方案审核定稿~~ → **2026-09-14 已定稿**（状态行已更新；D1–D19 全部落定） | 本文档 | — |
| S1 ✅ | `LoopConfig`、`cycleChanged` 信号、`RunSequence` 默认参数、Impl 成员、私有方法**声明**（不实现） | `src/Logic/SequenceWorker.h` | 默认参数 → 既有 3 个调用点零改动，编译必过 —— **已完成**（`ProcessPage.cpp:1078` 单参调用零改动编译通过） |
| S2 ✅ | `ExecuteActions`→`ExecuteOnce`；新增循环外壳；抽 `BuildSafePosActionFrom`；`ReturnToSafePosInline`；`RunSequence` 存 loop + 捕获 safePos；`RunSingleAction`/`ClearFault` 清 loop | `src/Logic/SequenceWorker.cpp` | **单轮零回归冒烟**：Sim 跑一遍 →「✅ 完成」一次、无循环日志（等价 8.18）—— **已完成**（Sim 冒烟 `Initialize complete` 通过；8.18 的 UI 侧核验待真机） |
| S2b ✅ | **`errorOccurred` 单点化**（D17 方案 A）：`Impl::lastError`（+ 每动作前清空）+ `ExecuteAction` 子树 8 处 emit 改写为赋值 + 2 处统一 emit 点（`ExecuteOnce` 失败分支 / `StartSingleExecution` 兜底） | `src/Logic/SequenceWorker.cpp` | 任一动作失败 → UI 显示**具体原因**（不再被 `action.name` 覆盖）；`retryCount=0` 下行为与今天一致（等价 8.7/8.23）—— **已完成**（全文件 `emit errorOccurred` 现仅 7 处：2 处门禁 + 2 处会话级出口 + 循环间隔安全位失败 + 注释 2 处） |
| S2c ✅ | **「严格先抬 Z」加固**（D19）：下发前对"目标与读回当前值之差 ≤ ε（0.01mm/0.01°）"的轴跳过下发 + `SPDLOG_INFO` 记录；`reissue` 同步按同规则跳过 | `src/Logic/SequenceWorker.cpp` | 回安全位/循环间隔的**点1 只下发 Z**（日志 `skip axis J1/J2/R (in position)`）；普通移动行为零变化（等价 8.1/8.2/8.20）—— **已完成**（实现为匿名 namespace 函数 `DispatchPointMove()`，4 轴一次 `InMainThread` 内联下发；跳过前仍过 `IsWithinSoftLimits`，不旁路软限位硬拦截） |
| S3 ✅ | 循环控件行 + `m_cycleLabel` + `OnCycleChanged` + 启动组装 LoopConfig + 两项门禁（未启用安全位拒绝 / 无限循环二次确认）+ `UpdateControlsEnabled` 内按 `!busy` 置灰（同方案下拉，与 `gate` 无关） | `src/UI/AutoRunPage.h/.cpp` | Sim：单轮=现状；N=3 计数与终态正确；运行中控件置灰 —— **已完成**（控件一行排布：`循环:` + 模式下拉 100×32 + 次数框 78×32 + 「回安全位」复选框 + 进度标签右对齐；`MainWindow` 最小尺寸 1200×700 下右侧面板约 462px，本行最小需求约 400px） |
| S4 ✅ | 持久化（`production.loop.*` 读写，不 emit paramsChanged） | `src/UI/AutoRunPage.cpp` | 重启后控件保持上次选择；config.json 自动出现 `production` 节点 —— **已完成**（初始化用 `QSignalBlocker` 屏蔽三控件，避免旧 config 首次启动被回写平白生成 `production` 节点） |
| S5 ✅ | 确认 `MainWindow` 零改动；跑 10.2/10.6/10.13 子集 | — | 无回归 —— **已完成**：`MainWindow`/`ProcessPage`/`ManualControlPage`/`HardwareManager` 确实零改动（10.2/10.6/10.13 子集待真机） |
| S6 🟡 | Sim 全量冒烟（8.18-8.26 / 10.21-10.26 可仿真子集） | — | Sim 无相机：视觉相关跳过 —— **部分完成**：Sim 启动冒烟通过（SimCard/SimServo/SimAlgo/SimCamera 全正常 + `Initialize complete`）；**Sim 无法自动驱动 UI 起停**（沙箱内 `Start-Process` 被拦），计数/停止/暂停等交互用例需真机或人工点界面 |
| S7 ✅ | 文档回填。**2026-09-14 已完成**：phase3 阶段 8/10 追加用例（8.18–8.26 / 10.21–10.26）+ 编号口径更正 4 处（`TEST_RECORD.md` TR-089 / `AGENTS.md` 里程碑段 / `worklog 2026-09-09.md` / `worklog 2026-09-10.md`）+ 实施后追加 **TR-091** + `AGENTS.md` 记新需求 + `doc/architecture.md` 记 `cycleChanged`/`LoopConfig`/`errorOccurred` 单点化 + `doc/compile_guide.md` 补沙箱编译失败排查 | 7 个 md | 引用一律 `TR-###`；编号口径与 §十 一致 |
| S8 ✅ | 日志保留：启动时清理 7 天前的 `log/creampuff_*.log`（§九 运维项；可独立提交） | `src/main.cpp` | 只删过期日期文件，当天/7 天内不动 —— **已完成并实测生效**：本次冒烟即删 29 个过期日期文件（cutoff 2026-09-07），`crash.txt` 与 7 天内文件未动，日志 `[Main] Log retention: removed 29 file(s) older than 7 days (cutoff 2026-09-07)` |

**预估工时**：实现 0.3 天 + 真机用例 0.2 天（S8 日志清理为独立小改，不计入本项）。

**台账编号（2026-09-14 更正）**：`real_machine_plan_phase3.md` 原预分配 TR-076/077/078 = 阶段 8/9/10，因执行过程中的回归缺陷先占用 TR-076/077，阶段 8 记账 TR-078，其后 TR-079~**TR-090** 已被 09-08~09-10 的阶段 9/10 验证与缺陷全部用尽（TR-080 = 回安全位误报「有任务执行中」，**注意与本文档初稿的"TR-080 起"冲突，勿混**）。本循环需求**单开新号：TR-091 起**；追加前**重读 `TEST_RECORD.md` 取当前最大号 +1**（并行会话抢号教训），禁止重排/复用。

---

## 十二、决策清单（**2026-09-14 全部落定**）

> 用户指令「其他内容直接定稿」→ 下表 **D1–D19 全部生效**（原标「采纳推荐」者一并定稿），本文档状态同步改为「已定稿，待实施」。

| # | 决策点 | 结论 |
|---|---|---|
| D1 | 循环形态 | **单轮 / 指定 N 次 / 无限 三挡，默认单轮**（✅ 采纳推荐） |
| D2 | 循环间隔回安全位 | **默认开；未启用安全位（`safePos.enabled=false`）时拒绝启动循环**（✅ **用户已拍板**） |
| D3 | 无限循环二次确认弹窗 | **要**（标题「确认无限循环」） |
| D4 | 动作失败处理 | **默认不重试（retryCount=0）；保留配置口子 N**（✅ **用户已拍板，并要求留口子**） |
| D5 | `schemeFinished` 时机 | **全部循环完成后只发一次**；每轮发 `cycleChanged` |
| D6 | ■ 停止 | **立即终止整个循环**（✅ **用户已拍板**） |
| D7 | 循环期间切手动 | **保持禁止（TR-075 不开后门）**；缓解 = 视觉页观察 / 先停止 |
| D8 | 全部循环完成后回安全位 | **不回**（保持与今天一致的停机位，便于取料） |
| D9 | TR 编号 | **本需求 TR-091 起**（2026-09-14 更正：TR-080~TR-090 已被阶段 9/10 验证与缺陷用尽，当前最大 TR-090） |
| D10 | 循环参数持久化 | **是**（仅持久化 UI 选择，不参与门禁） |
| D11 | 次数上限 | **1..9999**（SpinBox 限） |
| D12 | 死代码 `PickCycleController` | **本次不动**（避免与循环功能混在一条提交） |
| **D13** | 循环运行常驻提示条 | **不做**（2026-09-14 用户裁决；理由见 §九——单槽提示必被覆盖，进度由 `m_cycleLabel` 承载） |
| **D14** | 循环期间一键回零 | **不可达，不加门禁、不设用例**（2026-09-14 用户裁决；互锁禁止切回手动页，见 §六 #11） |
| **D15** | 术语「回安全位」 | 循环行复选框显示文本统一为**「回安全位」**；config 总开关显示名沿用 TR-082 的**「启用安全位」**，两者不混称 |
| **D16** | 日志保留 | **7 天**（2026-09-14 用户裁决；按日期分文件已存在，需补清理实现，见 §九 / S8） |
| **D17** | 重试期间 `errorOccurred` 抑制 | **方案 A：`errorOccurred` 单点化**（2026-09-14 用户拍板）——`ExecuteAction` 子树 8 处 emit 改为写 `Impl::lastError`，emit 收敛到 2 处会话级出口；`retryCount=0` 行为零变化，重试期间不误报（见 §七 / **S2b**） |
| **D18** | 循环行布局 | **先按一行实现，最终布局待定**（2026-09-14 用户裁决）——循环控件同一行排布；**TR-072 原方案已废弃**（用户已手动改过坐标面板，本文档不再引用）；硬约束 = 10 寸触控屏 + **窗口缩到最小尺寸时界面不错乱**（见 §二） |
| **D19** | 回安全位「严格先抬 Z 到位再水平移动」 | **结构上已保证**（点粒度串行 + `WaitForAxes`，见 §4.4）；**新增加固 = `MoveToPoint` 对 ε 内在位轴跳过下发** → 点1 只下发 Z（消除舵机读回微差导致的低位水平微动）；残余为到位时间估算窗口（全项目共用，+200ms 余量）。真机核验见 8.20 |

**待确认（无阻塞）**：真机验证时循环间隔回安全位的两段走位是否与 9.6 完全一致（同一代码路径，预期一致）。~~`log/creampuff.log` 轮转策略~~ → **已结案：按日期分文件 + 保留 7 天（D16）**。
