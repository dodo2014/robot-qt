# UI 权限与状态机互锁（Interlock）需求与方案

> 状态：**已实施（TR-075，2026-09-07）**——以本节"实施定稿"与代码为准，下方原始方案（含 5.3/5.4/5.5）仅作评审过程存档。
> 创建：2026-09-07　作者：CodeBuddy + 用户评审
> 相关台账：TR-053 / TR-054 / TR-062 / TR-063 / TR-064 / TR-065 / TR-066 / **TR-075**

## ✅ 实施定稿（TR-075，与原方案的差异以此为准）

| 项 | 原方案 | 实施定稿 |
|---|---|---|
| 急停锁存归属 | AutoRunPage 自持 `m_estopPending`（5.4h） | **HardwareManager::estopPending_**（全局硬件级状态，Gemini Q6）；置位于 `EmergencyStop` 的 `emit enableStateChanged()` 之前；清除唯一条件=急停后全轴回零（MarkAxisHomed allHomed），先清后发专用信号 `estopCleared()` |
| 模式切换门禁 | IsExecutionActive()（含 Fault） | **仅 Running/Paused 拦截**（Fault 放行——无运动在执行，切手动脱困合法；清报警回自动页做） |
| 单步会话切模式 | 禁止切换并弹窗 | **Q12 形态**：切自动→强制 `AbortStepSession()`（Stop+ResetStepSession）后放行；切手动→放行（工艺页手动模式可达） |
| 导航白名单 | 仅自动运行（决策 2） | **{0 自动运行，3 视觉检测}**（Gemini Q9）；视觉页 `SetParamsLocked` 锁 9 个参数控件（4 检测参数+deviceId+相机类型/分辨率/帧率/算法），操作类控件（相机开关/采集/检测/视图/截图）保持可用 |
| 方案下拉 | `gate && !busy`（手动模式全灭） | **`!busy`**——手动模式与急停锁存期保留预选能力（对齐原评审稿 5.3 表格） |
| 标签高亮 | toggled lambda 内（5.5 称"回弹无需补偿"） | **迁入 `ApplyModeState`**（模式态唯一出口）——5.5 结论有误：拒绝回弹经 QSignalBlocker 屏蔽 toggled，高亮留在 lambda 会残留被拒模式色 |
| 手动页急停提示 | 明确不做（原第九节） | **已做**（Gemini 修正 2）：estop 时先 ResetAxisStates 再 SetHint 分步恢复指导（防被 RefreshSoftLimitHint 覆盖）；`estopCleared` 绿字提示；OnGlobalEnable 成功且 IsEStopPending 时提示"请执行一键回零" |
| 强制跳页 | `setChecked(true)` 触发 idClicked | **双写** `setChecked(true)+setCurrentIndex(0)`（程序化置位不发 idClicked） |

安全位（TR-074）影响已覆盖：门禁弹窗区分 `IsSafePosSession()` → "正在返回安全位，请稍候"（不诱导停止）；急停恢复链路含"等自动回安全位完成"步骤。**手测 7 条见 TEST_RECORD TR-075，待真机验证阶段统一执行。**

**2026-09-15 补充（TR-092，自动运行页提示迁至顶栏全局横幅）**：`AutoRunPage` 底部 `m_hintLabel` 已删除，`SetHint` 改为 `emit requestGlobalHint(text,color)`，提示统一由 MainWindow 顶栏「全局消息横幅」（`HintBanner`，定义在 `MainWindow.cpp`）承载；**手动页 `hintLabel_` 与 `RefreshSoftLimitHint` 未改动**，故本表「手动页急停提示」行的行为（先 ResetAxisStates 再 SetHint、防被 RefreshSoftLimitHint 覆盖）完全不变，只是该提示的显示位置仍留在手动页。**下文「四、现状核实」与 5.4 节中出现的 `m_hintLabel` 均为 2026-09-07 存档文本**，引用前请以本节与源码为准。

---

## ⚠ 2026-09-07 更新：按钮语义已重构为 5 按钮（先行落地，本文 5.3/5.4 节为旧 4 按钮版）

与用户/Gemini 评审后，**先行完成**了「5 按钮语义 + SequenceWorker 显式状态机」重构（架构指令 1/2），本文的互锁方案（2/2）建立在新基础之上。实施互锁前必须按以下变更重新推导 5.3/5.4：

| 变更 | 旧（本文所写） | 新（已落地，以代码为准） |
|---|---|---|
| 按钮组 | 4 按钮：`m_btnStart/m_btnReset/m_btnStop/m_btnInit`（复位=HomeAll） | **5 按钮**：`m_btnStartOrResume / m_btnPause / m_btnStop / m_btnAlarmClear / m_btnInit`（原"复位"实为 HomeAll，已归手动控制页；"清报警"仅清引擎 Fault 锁存） |
| `stateChanged` 签名 | `stateChanged(const QString&)` | **`stateChanged(WorkerState, PauseReason)`**（enum + Q_ENUM，跨线程 Queued 需元类型注册）；消费者仅 ProcessPage（已改） |
| 引擎状态 | `running` 布尔 | **`enum class WorkerState { Idle, Running, Paused, Fault }`**；`IsRunning()` = `state != Idle`（单步暂停时亦 true，语义零回归） |
| 停止语义 | `Stop()` 仅 cancel（跑完当前动作） | 新增 **`StopImmediate()`**（cancel + `StopAllAxes()` 减速停，保持使能）供自动页；`Stop()` 保持就近（单步/ShutdownWorker 不变）；`EmergencyStop()` 不变 |
| 暂停 | 无 | **`Pause()/Resume()` + `PauseGate(axes, reissue)`**：点级暂停+原地续走（Resume 重发当前绝对目标 MoveAbs；Delay 按剩余时间；回抽阶段不反向补料） |
| Fault | 无 | 动作失败且非 cancel → `Fault` 锁存（保留上下文），`ClearFault()` 回 Idle（不动硬件使能） |
| 按钮启用出口 | `UpdateControlsEnabled()`（4 钮矩阵） | 已落地同名函数（**5 钮矩阵**，见 `AutoRunPage.cpp`），互锁只需扩展 `gate = m_autoModeActive && !m_estopPending` 分支 |
| 急停锁存 | `m_estopPending` 放 AutoRunPage | **改放 `HardwareManager`（如 `IsEStopPending()`）**——急停是全局硬件状态，任何页面按钮都该受限（Gemini 复核意见 Q6）；提示文案可放手动页 m_hintLabel |
| 模式白名单 | 仅自动运行 | **自动运行 + 视觉检测**（生产需观察相机；视觉页只读，禁改参数）——Gemini 复核意见 Q9 |
| 单步会话切换 | 禁止切换并弹窗 | **切自动时强制 Stop() + ResetStepSession()**（不留脏数据）——Gemini 复核意见 Q12 |
| `IsExecutionActive` | private → public | 未做（本轮未动 ProcessPage.h），实施时需上移 |

### ⚠ 补充（2026-09-07 晚）：全局安全位（TR-074）对互锁的影响——实施时必须一并考虑

安全位已在互锁之前落地（`doc/global_safe_position.md`），对本方案的影响：

1. **"运行中禁止切模式"窗口被拉长**：`kinematics.safePos.enabled=true` 时，每次回零完成都会自动执行 `RunSafePos()`（worker 经 RunSequence 进入 Running，2 点运动时长）。此窗口内 `IsExecutionActive()`=true → 模式切换被拒。**门禁弹窗文案需涵盖此情形**——现文案"请先点击「停止」结束当前执行"可能诱导操作员把自动回安全位会话停掉；建议文案区分（安全位会话提示"正在返回安全位，请稍候"或允许等待）。
2. **急停恢复链路追加一步**：急停 → 切手动 → 使能 → 一键回零 → **等待自动回安全位完成（enabled=true 时）** → 切自动启动方案。恢复提示文案（若按本方案放 AutoRunPage hint）应包含"正在返回安全位"状态。
3. **`homeStateChanged(true)` 现有两个消费逻辑共存**：① MainWindow 上升沿（`lastHomed_` 判定）触发 `RunSafePos()`；② 本方案 5.4(d) 的 AutoRunPage 锁存清除（`m_estopPending=false`）。两者互不干扰（一个发运动指令、一个清 UI 锁存），但实施时注意连接顺序与触发时序（均在 PollTick 主线程内顺序执行）。
4. **`RunSequence` 已补清 `stepMode`**（2026-09-07）：本方案 5.4 的"暂停中切自动→强制 Stop+ResetStepSession"实现时，`RunSequence` 侧已自防单步残留卡死。
5. 第四节"现状核实"的 AutoRunPage 行号/4 按钮事实同样随重构漂移，以 `doc/auto_run_button_refactor.md` 与当前代码为准。

---

## 一、需求原文

基于 `MainWindow` 顶栏的【手动/自动】切换开关，以及系统的急停状态，实现互锁：

**1. 模式切换 (`MainWindow::OnModeToggled`)**
- 切【手动】：激活左侧导航栏所有页面按钮（`navGroup_->buttons()`）；将自动运行界面的 4 个操作按钮（启动、复位、停止、初始化）全部置灰 `setEnabled(false)`。
- 切【自动】：强制跳转到 `AutoRunPage`；将左侧导航中除"自动运行"以外全部置灰；恢复自动运行界面 4 个按钮状态（未回零时【启动】仍受硬件防线拦截）。

**2. 自动运行界面防呆 (`AutoRunPage`)**
- `SequenceWorker` 处于 Running 期间，`m_schemeCombo`（方案切换下拉框）必须置灰禁止操作，流程结束后恢复。

**3. 急停恢复链路强制规范**
- 触发 `EmergencyStop` 后：切断硬件 + 中断 `SequenceWorker`；自动运行界面所有控制按钮失效；
- **核心约束**：必须通过 UI 提示操作员"急停已触发，请切换至【手动】模式，重新执行使能与一键回零后方可恢复生产。"

---

## 二、需求评审：发现的问题

需求方向正确（模式互锁 + 防呆 + 急停恢复链路均必要），但存在 **1 个致命安全问题、1 个无法强制的核心约束、3 处未定义边界**。

### ① 【致命】切【手动】会把「停止」也置灰 → 运行中无法停机

- 切模式**不会停止**正在运行的方案。若自动运行中切到手动，按需求一.1「停止」被禁用 → **无法停止运行中的流程**，只能切回自动或拍急停。
- 更严重：`ManualControlPage` 的点动/Go **完全不接 worker**（TR-066 未实现），手动模式下若自动流程仍在跑，操作员可同时点动 → **双运动源撞机风险**。

### ② 【核心约束无法强制】「必须回零才能恢复」做不到

- `IsSystemHomed()` 急停后**不一定变 false**：`AbortHoming`（`HardwareManager.cpp:721-731`）只复位"正在回零中"的轴，已回零完成的轴机械没动、homed 保持 true（**TR-062 真机实测**）。
- 因此按需求一.2"恢复 4 个按钮（未回零启动受拦截）"，操作员**不回零、只要重新使能**就能让【启动】重新亮起 → 需求三的核心约束形同虚设。
- **必须加「急停待恢复」锁存标志**，且只有急停后真正跑完一次回零才清除。

### ③ 【边界未定义】自动模式锁哪些页

导航共 5 页：自动运行 / 手动控制 / 工艺流程 / 视觉检测 / 设备配置。需求"除自动运行外全部置灰"意味着**视觉检测页也锁**（生产时无法观察/调试相机，而自动运行的 Vision 动作依赖相机）、**工艺流程页也锁**（无法编辑方案或单步调试）。

### ④ 【会话残留】强制跳转会留下孤儿单步会话

工艺流程页单步暂停时（worker 挂在 `WaitForStep`、`m_stepActive=true`），切到自动被强制跳走 → **会话残留**（`ProcessPage` 无 `showEvent` 清理）。之后自动启动会被 running 门禁以"启动失败：已在运行中"拒绝，体验异常。

### ⑤ 【实现缺口】

| 缺口 | 现状 |
|---|---|
| 4 按钮无法批量禁用 | 仅 `m_btnStart` 是成员（`AutoRunPage.h:57`），复位/停止/初始化是循环内局部变量（`AutoRunPage.cpp:218-249`） |
| 急停槽与需求相反 | `OnEmergencyTriggered`（`cpp:392-399`）反而 `m_btnStart->setEnabled(true)` |
| 六处无条件恢复 | `cpp:344/375/395/446/455/465` 无条件 `setEnabled(true)`，可绕过互锁 |
| `homeStateChanged` 不判 running | `cpp:256-259` 运行中收到回零完成会放开启动按钮 |
| 复位/初始化无运行期门禁 | 运行中点复位 = HomeAll，直接操作硬件，可撞机 |
| AutoRunPage 不感知运行态 | 未接 `stateChanged`、未调 `IsRunning()` |

---

## 三、已确认的 4 项决策

| # | 决策 | 选择 |
|---|---|---|
| 1 | 自动流程【运行中】能否切模式 | **禁止切换**（弹窗 + 开关回弹）。保证【停止】始终可用，避免自动流程与手动点动并存 |
| 2 |【自动】模式页面白名单 | **仅「自动运行」**（其余 4 页全灰）。留常量便于后续放开 |
| 3 | 急停后"必须回零"如何强制 | **加「急停待恢复」锁存标志**，只有急停后真正跑完一次一键回零才清除 |
| 4 | 单步调试暂停中切【自动】 | **禁止切换并弹窗**（同 TR-064/065 切方案门禁，提示先点停止） |

---

## 四、现状核实（关键事实 + 代码位置）

### MainWindow
| 事实 | 位置 |
|---|---|
| 【手动/自动】开关已存在，但 `OnModeToggled` **只打日志** | `MainWindow.cpp:433`（创建）、`:440-451`（lambda）、`:618-622`（空实现） |
| `toggle` 是**局部变量**，需提升为成员才能程序化回弹 | `MainWindow.cpp:433` |
| 导航 5 页：0 自动运行 / 1 手动控制 / 2 工艺流程 / 3 视觉检测 / 4 设备配置 | `MainWindow.cpp:561-567`，`navGroup_` 见 `MainWindow.h:42` |
| `OnNavButtonClicked` 直接切页，**无任何校验/置灰** | `MainWindow.cpp:608-616` |
| 顶栏急停钮（圆形 60×60）无任何 `setEnabled`，**永不禁用** | `MainWindow.cpp:515-530` |
| MainWindow 自身**未 connect** `emergencyStopTriggered`（各页自理） | `MainWindow.cpp:513-514` |

### AutoRunPage
| 事实 | 位置 |
|---|---|
| 4 按钮在循环内创建，按 `text.contains("启动")` 匹配（**改文案会静默错配**） | `AutoRunPage.cpp:218-249` |
| 仅 `m_btnStart` 是成员 | `AutoRunPage.h:57` |
| `m_schemeCombo` **从不被禁用**，无 `currentIndexChanged` 连接 | `.h:50` / `cpp:195` |
| 未接 `stateChanged`、未调 `IsRunning()` | `SetSequenceWorker` `cpp:47-57` |
| `m_btnStart` 六处无条件恢复为 true | `cpp:344/375/395/446/455/465` |
| `homeStateChanged` 不判 running（既有缺陷） | `cpp:256-259` |
| 【启动】三层门禁：按钮级 `setEnabled(IsSystemHomed())` / 点击级查 `IsGlobalEnabled()` / Worker 级 `RunSequence` | `cpp:241` / `cpp:326-330` / `SequenceWorker.cpp:139-152` |
| 复位/初始化运行中始终可用且不校验运行态 | `cpp:355/380` |
| 提示控件可用：`m_hintLabel` / `m_statusLabel` / `m_logTextEdit` | `.h:54/48/51` |

### 状态 / 硬件层
| 事实 | 位置 |
|---|---|
| `ToggleSwitch::setChecked()` 值变化**必 emit `toggled`** | `ToggleSwitch.cpp:14-21` |
| `ToggleSwitch::paintEvent` **无 disabled 视觉分支** → `setEnabled(false)` 无反馈 | `ToggleSwitch.cpp:23-52` |
| `SequenceWorker::IsRunning()` 为原子量（跨线程可信真相源） | `SequenceWorker.cpp:240` |
| `Stop()` **仅置 cancel**，执行完当前动作才停（非就近刹车） | `SequenceWorker.cpp:211-216`，TR-063 实测 |
| `AbortHoming` 只复位"正在回零中"的轴 → 急停后 `IsSystemHomed()` 可能仍 true | `HardwareManager.cpp:721-731`，TR-062 |
| `homeStateChanged` **全局只 emit `true`**，仅在 `MarkAxisHomed` 全轴完成时 | `HardwareManager.cpp:716` |
| ManualControlPage 使能/回零按钮**从不禁用**（急停后恢复通道可用） | `ManualControlPage.cpp:138-151/438-497` |
| `ProcessPage::IsExecutionActive()` 现为 **private**，MainWindow 需调用 → 上移 public | `ProcessPage.h:54` |
| TR-066 跨页互斥**未实现**（仅引擎级 running 门禁 + D2 副作用） | 台账 TR-066 |

---

## 五、实现方案

### 5.1 `MainWindow.h`
```cpp
class ToggleSwitch;          // 新增前置声明（与既有 AutoRunPage* 等风格一致）

private:
    void ApplyModeState(bool autoMode);      // 应用模式态的唯一出口（导航白名单+强制跳页+下发）
    bool IsExecutionActive() const;          // 门禁：worker running || 单步会话
    void RevertModeToggle(bool backToAuto);  // 开关回弹（必须阻断信号，见 5.5）
    ToggleSwitch* modeToggle_ = nullptr;     // 顶栏【手动/自动】开关（提升为成员）
```

### 5.2 `MainWindow.cpp`
文件作用域常量：
```cpp
constexpr int kPageAutoRun = 0;                    // 与 stack_ 页序严格一致
const QVector<int> kAutoModeNavWhitelist = { kPageAutoRun };   // 决策 2：后续放开只改此处
```
`:433` 后加 `modeToggle_ = toggle;`。

**`OnModeToggled`（替换 `:618-622`）**：
```cpp
void MainWindow::OnModeToggled(bool autoMode)
{
    // 门禁（决策 1/4）：执行进行中禁止切换模式
    // 理由：① 保证【停止】始终可点；② 避免自动流程与手动点动并存；
    //       ③ 避免单步会话被强制跳走留下残留
    if (IsExecutionActive()) {
        SPDLOG_WARN("[MainWindow] 模式切换被拒：执行进行中 autoMode={}", autoMode);
        QMessageBox::warning(this, QStringLiteral("禁止切换模式"),
            QStringLiteral("当前有流程正在执行，禁止切换【手动 / 自动】模式。\n"
                           "请先点击「停止」结束当前执行，再切换模式。"));
        RevertModeToggle(!autoMode);
        return;
    }
    ApplyModeState(autoMode);
}

bool MainWindow::IsExecutionActive() const
{
    if (sequenceWorker_ && sequenceWorker_->IsRunning()) return true;
    return processPage_ && processPage_->IsExecutionActive();
}

void MainWindow::ApplyModeState(bool autoMode)
{
    // ① 导航按白名单启停（决策 2）
    for (QAbstractButton* b : navGroup_->buttons())
        b->setEnabled(!autoMode || kAutoModeNavWhitelist.contains(navGroup_->id(b)));

    // ② 自动模式强制跳「自动运行」页
    if (autoMode) {
        if (auto* b0 = navGroup_->button(kPageAutoRun)) b0->setChecked(true); // 触发 idClicked→切页
        stack_->setCurrentIndex(kPageAutoRun);                                // 已在 0 页时兜底
    }

    // ③ 下发到自动运行页（直接调接口，理由见 5.4）
    autoRunPage_->SetAutoModeActive(autoMode);
}
```
构造函数 worker 注入后加 `ApplyModeState(modeToggle_->isChecked());` 对齐初值。
新增 include：`<QMessageBox>` `<QSignalBlocker>` `<QAbstractButton>`。
**不改**：顶栏急停钮（`:515-530`）永不禁用——安全底线。

### 5.3 `AutoRunPage.h`
```cpp
public:
    // 模式互锁：由 MainWindow 顶栏【手动/自动】开关调用
    void SetAutoModeActive(bool active);

private:
    void UpdateControlsEnabled();                                   // 互锁唯一出口
    bool IsBusy() const { return m_worker && m_worker->IsRunning(); }

    QPushButton* m_btnStart = nullptr;   // 既有
    QPushButton* m_btnReset = nullptr;   // 新增
    QPushButton* m_btnStop  = nullptr;   // 新增
    QPushButton* m_btnInit  = nullptr;   // 新增
    bool m_autoModeActive = false;       // 默认手动（与 toggle 初值 false 一致）
    bool m_estopPending   = false;       // 急停待恢复锁存（决策 3）
```

### 5.4 `AutoRunPage.cpp`（**重点，待 Gemini 复核**）

**(a) 按钮改为命名成员 + 下标赋值**（`:218-249`）
```cpp
// 数组序：0 启动 / 1 复位 / 2 停止 / 3 初始化
for (int i = 0; i < btns.size(); ++i) {
    auto* btn = new QPushButton(btns[i].text);
    /* 样式、光标、尺寸保持原样 */
    switch (i) {
    case 0: m_btnStart = btn; connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnStartClicked); break;
    case 1: m_btnReset = btn; connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnResetClicked); break;
    case 2: m_btnStop  = btn; connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnStopClicked);  break;
    default:m_btnInit  = btn; connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnInitClicked);  break;
    }
}
```
> 为何不用 `QList`/`QButtonGroup`：四个按钮启用规则**各不相同**（启动受 homed 约束、停止运行期必须可用、复位/初始化运行期禁用），命名成员最直白；容器需下标访问、语义靠注释维持易错。

**(b) 互锁唯一出口 `UpdateControlsEnabled()`**
```cpp
void AutoRunPage::UpdateControlsEnabled()
{
    if (!m_btnStart || !m_btnReset || !m_btnStop || !m_btnInit) return;
    const bool busy  = IsBusy();                                 // SequenceWorker::IsRunning()（原子）
    const bool homed = HardwareManager::instance().IsSystemHomed();

    // 需求 2：运行期（含"单步暂停"）禁止切换方案
    m_schemeCombo->setEnabled(!busy);

    // 非自动模式 或 急停待恢复 → 四个按钮全灭
    if (!m_autoModeActive || m_estopPending) {
        m_btnStart->setEnabled(false); m_btnReset->setEnabled(false);
        m_btnStop ->setEnabled(false); m_btnInit ->setEnabled(false);
        return;
    }

    m_btnStart->setEnabled(!busy && homed);   // 未回零仍灰（需求一.2 括号说明）
    m_btnReset->setEnabled(!busy);            // 运行期 HomeAll 会撞机
    m_btnInit ->setEnabled(!busy);            // 运行期重初始化会打断硬件
    m_btnStop ->setEnabled(true);             // 【停止】运行期必须始终可用
}
```

**按钮启用矩阵**

| 状态 | 启动 | 复位 | 停止 | 初始化 | 方案下拉 |
|---|---|---|---|---|---|
| 手动模式（空闲） | ✗ | ✗ | ✗ | ✗ | ✓（可预选） |
| 自动 · 空闲 · 已回零 | ✓ | ✓ | ✓ | ✓ | ✓ |
| 自动 · 空闲 · 未回零 | ✗ | ✓ | ✓ | ✓ | ✓ |
| 自动 · 运行中 | ✗ | ✗ | **✓** | ✗ | ✗（需求 2） |
| 急停待恢复（任意模式） | ✗ | ✗ | ✗ | ✗ | ✓ |

> **关于「停止」在非自动模式/急停下也置灰的安全性论证**：
> - 切模式已被 MainWindow 门禁挡在运行之外（决策 1），不存在"运行中切到手动导致停不了"；
> - 急停后硬件已断使能、worker 已中断，无执行需要停；
> - 真有残留执行，兜底通道是**顶栏急停（永不禁用）**。
> 此为需求一.1 的原文要求（4 个按钮全部置灰），予以保留。

**(c) 运行态判定：以 `IsRunning()` 为真相，`stateChanged` 仅作刷新触发**
```cpp
// SetSequenceWorker 中新增
connect(m_worker, &SequenceWorker::stateChanged,
        this, [this](const QString&) { UpdateControlsEnabled(); });
```
> 为何不解析 `stateChanged` 文案："单步暂停"期间流程未结束但 `IsRunning()==true`，靠字符串集合穷举易漏判；且结束的 `"空闲"` 从 worker 线程 queued 到达，而 `IsRunning()` 读到的必是当前值。

**(d) 修正 `homeStateChanged`（`:256-259`，既有缺陷：不判 running）**
```cpp
connect(&HardwareManager::instance(), &HardwareManager::homeStateChanged,
        this, [this](bool homed) {
    if (homed) m_estopPending = false;   // homeStateChanged 全局只 emit true，且只在全轴 MarkAxisHomed 完成时
    UpdateControlsEnabled();             // 统一出口，不再直接 setEnabled(homed)
});
```

**(e) 六处无条件恢复全部改为调 `UpdateControlsEnabled()`**（`cpp:344/375/395/446/455/465`），并删除 `:241` 的直接 `setEnabled`。

**(f) `OnStopClicked` 不立即恢复按钮**
```cpp
void AutoRunPage::OnStopClicked()
{
    if (m_worker) m_worker->Stop();
    // Stop() 只置 cancel，当前动作执行完才真正退出（TR-063），running 仍 true：
    // 不立即恢复按钮，等 interrupted / schemeFinished / stateChanged("空闲") 到达后由统一出口恢复。
    if (!IsBusy()) UpdateControlsEnabled();   // 未真正运行时 Stop() 不发信号，就地刷新防永久置灰
    else SetHint(QStringLiteral("正在安全停止…"), QStringLiteral("#8fd4ff"));
}
```

**(g) `OnResetClicked` / `OnInitClicked` 开头加运行期兜底**
```cpp
if (IsBusy()) { SetHint(QStringLiteral("流程执行中，禁止复位（请先停止）"), QStringLiteral("#f7c948")); return; }
```

**(h) 急停槽改造 + 锁存状态机（`OnEmergencyTriggered` `:392-399`）**
```cpp
void AutoRunPage::OnEmergencyTriggered()
{
    // 决策 3：急停后必须加锁存，且只有真正跑完一次回零才清除。
    // 不能用 IsSystemHomed() 兜底：AbortHoming 只复位"正在回零中"的轴，
    // 已回零完成的轴 homed 保持 true（TR-062 实测）→ 急停后 IsSystemHomed() 可能仍 true。
    m_estopPending = true;
    UpdateControlsEnabled();          // 原代码此处反而 setEnabled(true)，与需求相反

    m_statusLabel->setText(QStringLiteral("⛔ 急停"));
    m_statusLabel->setStyleSheet(/* 红色 28px，同既有 */);
    m_hintLabel->setText(QStringLiteral(
        "急停已触发，请切换至【手动】模式，重新执行使能与一键回零后方可恢复生产。"));
    m_hintLabel->setStyleSheet(/* 红色 #ff5e6b */);
    m_logTextEdit->append(/* 追加日志 */);
}
```
**状态机**
```
上电 ──emergencyStopTriggered(HardwareManager.cpp:845)──▶ ESTOP_PENDING
        · m_estopPending = true
        · 四按钮全灭 + 红字常驻提示 + 状态「⛔ 急停」
             │
             │ homeStateChanged(true)  （MarkAxisHomed 全轴完成，HardwareManager.cpp:716）
             ▼
      m_estopPending = false ──▶ 按 模式 ∧ running ∧ homed 恢复
```
**锁存归属：AutoRunPage 自持**。理由：它是唯一消费方；置位信号（`emergencyStopTriggered` `cpp:253`）与清除信号（`homeStateChanged` `cpp:256`）本页已全部连接，闭环在页内完成、零新增连接。若 MainWindow 也持一份会形成两处清除条件 → 状态漂移风险。

**已知残留（写注释）**：某轴已 homed 且未参与本次回零时，其它轴完成即判 allHomed 会提前解锁（机械位置未变，可接受，本次不处理）。

### 5.5 关键陷阱：开关回弹的信号递归（必须处理）

`ToggleSwitch::setChecked()` 值变化即 `emit toggled`（`ToggleSwitch.cpp:14-21`）。拒绝切换后直接 `setChecked(旧值)` 会形成"拒绝→回弹→再拒绝"**无限递归**（门禁条件在递归期间不变，永不终止，且每次嵌套一个模态 `QMessageBox`）。

```cpp
void MainWindow::RevertModeToggle(bool backToAuto)
{
    // QSignalBlocker：RAII，构造时 blockSignals(true)，析构自动恢复原屏蔽态
    const QSignalBlocker blocker(modeToggle_);
    modeToggle_->setChecked(backToAuto);
}
```
- **不可用** `if (autoMode == 旧值) return;` 式守卫：会吞掉回弹，留下 UI 与逻辑不一致。
- **不可用** `setEnabled(false)`：`ToggleSwitch` 的 `paintEvent` 无 disabled 视觉分支，点了没反应也无解释。
- 被一并屏蔽的标签高亮 lambda（`:440`）无需补偿——回弹后颜色本就该是原模式。

### 5.6 `ProcessPage.h`
`IsExecutionActive()`（`:54`）从 private **上移到 public**（仅移动，不改判定逻辑）。

---

## 六、台账编号（须先处理撞号）

现状：**TR-071 有两条重复且均已提交**
- `:81` 并行会话的「示教记录关节解 + 确定性执行」
- `:82` 本会话的「AutoRunPage 坐标面板窄宽度 R 折行」

处理：实施前 `grep -rn "TR-07[12]" --include=*.md` 确认引用面；**默认改本会话的坐标面板记录为 TR-072**（其引用仅在本会话 `doc/worklog/2026-09-04.md` 内），保留并行会话的示教记录为 TR-071；则本次互锁新记录取 **TR-073**。

---

## 七、待人工审核 / 待 Gemini 复核清单（**重点**）

### A. 自动运行界面按钮实现逻辑（请重点过一遍）
1. **按钮启用矩阵**（5.4 表格）是否符合预期？尤其"自动·运行中"时【停止】唯一可点，是否正确？
2. **【停止】在手动模式/急停待恢复下也置灰**——是否接受？（方案 5.4 已给安全论证：切模式门禁 + 顶栏急停兜底）
3. **方案下拉 `m_schemeCombo` 在手动模式下仍可操作**（仅运行期禁用）——是否应改为"非自动模式也禁用"？
4. **复位/初始化运行期禁用**——是否应更严格（例如"已回零状态下禁用复位"）？
5. **运行态用 `IsRunning()` 而非解析 `stateChanged` 文案**——是否认同？（"单步暂停"期间 IsRunning 仍 true）
6. **急停锁存放 AutoRunPage 自持 vs 提到 MainWindow 统一管理**——哪种更合理？
7. **急停恢复提示放在 AutoRunPage 页内**（操作员若停在手动控制页看不到）——是否需要在 MainWindow 顶栏加常驻红色 banner？（方案因"状态双份"风险未纳入，如需可改为 AutoRunPage 发信号、MainWindow 只显示）

### B. 模式互锁
8. **运行中禁止切模式**（决策 1）——是否满足现场操作习惯？是否改为"允许切但停止保持可用"？
9. **自动模式白名单仅自动运行**（决策 2）——视觉检测页被锁导致生产中无法观察相机，是否可接受？
10. **开关回弹用 `QSignalBlocker`**——实现是否正确可靠？有无更好做法？

### C. 边界与残留
11. **急停锁存提前解锁**（某轴已 homed 未参与本次回零 → 其它轴完成即判 allHomed）——是否需要更严格？
12. **手动模式下单步会话切页仍残留 `m_stepActive`**（既有问题）——本次不处理是否可接受？
13. **TR-066 跨页互斥**（ManualControlPage 点动与自动流程并存）本次明确不做——是否需纳入本次范围？

---

## 八、验证清单

- Debug + Release 双编译（`AGENTS.md:26`）；Sim 冒烟通过判定 = 日志含 `Initialize complete`（`AGENTS.md:28`）。
- 冒烟需覆盖"急停 → 手动页使能 → 一键回零 → 切自动 → 启动恢复"；注意 Sim 舵机轴回零需能走到 `HardwareManager.cpp:1221` 才发 `homeStateChanged(true)`，否则锁存不清除。
- 手测 7 条：
  1. 手动→自动：导航仅"自动运行"可点、强制跳页、四按钮按 homed 恢复
  2. 自动→手动：导航全开、四按钮全灰
  3. **运行中连点开关：弹窗 + 回弹，且不出现第二次弹窗**（递归回归）
  4. 单步暂停中切模式：弹窗 + 回弹
  5. 运行中：下拉/复位/初始化灰、【停止】可点
  6. 运行中停止：按钮在动作真正结束后才恢复
  7. 急停 → 全灰 + 红字提示 → 使能回零 → 切自动 → 启动恢复

---

## 九、本次明确不做（避免范围膨胀）

- TR-066 跨页互斥（ManualControlPage 点动的模式门禁）
- 配置文件化的导航白名单机制
- MainWindow 顶栏急停提示 banner
- ManualControlPage 的急停提示文案补充
