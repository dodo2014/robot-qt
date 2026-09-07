# 全局安全位（Global Safe Position）实现方案

> 前置：5 按钮 + 状态机重构已完成（TR-073）。本功能为急停恢复链路补全「回零后 → 回安全位 → 恢复生产」。
> 4 项决策已确认：**关节角配置 / 回零后自动+手动按钮 / 复用 SequenceWorker / 配置页输入框+手动页示教钮**。

## 一、需求

回零完成后（含急停恢复后的重新回零）自动走到预配置的安全位：**先抬 Z 再水平移动**，避免从低位直奔方案第一步横扫桌面。另提供手动「回安全位」按钮与「示教安全位」。

## 二、已确认决策

| # | 决策 |
|---|---|
| 1 | 配置形态：**关节角 j1/j2/z/r**（与示教 joints 同坐标系，MoveAbs 直发，无 IK 歧义；Z 为电机坐标，回零后 0 向下为负） |
| 2 | 触发：`homeStateChanged(true)` **上升沿**自动回安全位 + 手动控制页「回安全位」按钮 |
| 3 | 执行：**复用 SequenceWorker**——构造临时方案（1 个 Move 动作、2 个 hasJoints 点：点1=原地抬Z、点2=安全位）走现有 `RunSequence→ExecuteMove`，**零新运动代码**，白得暂停/停止/门禁/与自动运行互斥 |
| 4 | UI：ConfigPage 运动学区 4 输入框 + 自动触发开关 checkbox；ManualControlPage「回安全位」+「示教安全位」两按钮 |

## 三、探查核实的关键事实

- 配置写入：`ConfigManager::set()` 自动 300ms 落盘（`ConfigManager.cpp:118-136`）；ConfigPage links 编辑模式可照抄（`ConfigPage.cpp:322-364`：QLineEdit + `dVal(path)` + editingFinished）。
- `homeStateChanged(bool)` **全工程唯一 emit**：`HardwareManager.cpp:736`（全绑定轴 homed 时），PollTick 主线程内。**电平语义**：手动页单轴重回零（其余轴 homed 保持）会再次 emit true → 自动触发必须做**上升沿**判定。
- `MoveAbs` 参数即逻辑关节角（`HardwareManager.cpp:379-457`）；`MoveToPoint` hasJoints 路径（`SequenceWorker.cpp:460-481`）有 **FK 一致性校验 ±0.5**（校验 pt.x/y/z/r 与 FK(joints)）→ 构造点必须用 `impl_->kin.Forward(joints)` 填 x/y/z/r，否则静默回退 IK。
- 急停不复位已完成轴的 homed（`EmergencyStop:843-867` 仅 AbortHoming"回零中"轴）→ 急停后**仅重新使能不触发** homeStateChanged，必须一键回零 → 与恢复链路一致，且正确（急停后舵机 TorqueOff 可能垂落，旧坐标不可信）。
- **隐患 1**：`RunSequence`（`:168-175`）**不清 `stepMode`**——单步会话残留 stepMode=true 时，安全位会话 1 个动作跑完会挂起在 `Paused(Step)` 等NextStep → **卡死**。`RunSingleAction:216` 有清，RunSequence 须补。
- **隐患 2**：ProcessPage `actionStarted`（`:915-922`）会按 index 选中自己列表的行 → 临时方案的动作 0/1 会误选中。需 `IsSafePosSession()` 屏蔽（复用 singleSession 模式）。
- `ManualControlPage` 无 worker 注入；顶栏三按钮为局部变量（`:133-169`），新按钮插在 `homeAllBtn`（`:162`）之后。
- `paramsChanged` 唯一消费者是 `ReloadFromConfig`（`MainWindow.cpp:155-157`）→ safePos 每次 `RunSafePos()` 现读 config，**不需要也不应 emit paramsChanged**。

## 四、实现方案（4 步）

### 步骤 1 `SequenceWorker` 核心（纯增量 + 2 处防御修复）

```cpp
// .h public 新增
bool RunSafePos();                // 回安全位：读 config → 构造临时方案 → RunSequence
bool IsSafePosSession() const;    // 供 ProcessPage 屏蔽临时方案的 UI 联动
// .h private 新增成员（Impl）
std::atomic<bool> safeSession {false};
```

```cpp
bool SequenceWorker::RunSafePos()
{
    auto& cfg = ConfigManager::instance();
    if (!cfg.getValue<bool>("kinematics.safePos.enabled", false)) return false;  // 未启用/未示教
    const double sj1 = cfg.getValue<double>("kinematics.safePos.j1", 0.0);
    // ... j2 / z / r 同理

    // "当前位"构造时读（触发点=回零位，安全；手动触发=操作者所见位）
    const double cj1 = InMainThread([] { return HardwareManager::instance().GetPosition(LogicalAxis::J1); });
    const double cj2 = InMainThread([] { return hw.GetPosition(LogicalAxis::J2); });
    const double cr  = InMainThread([] { return hw.GetPosition(LogicalAxis::R);  });

    // FK 同源填充 x/y/z/r —— 必须过 MoveToPoint 的 ±0.5 一致性校验，否则静默回退 IK
    const Pose f1 = impl_->kin.Forward(Joints{ cj1, cj2, sz, cr });
    const Pose f2 = impl_->kin.Forward(Joints{ sj1, sj2, sz, sr });

    PointData lift;  lift.name="抬Z";  lift.hasJoints=true;
    lift.j1=cj1; lift.j2=cj2; lift.j3=sz; lift.j4=cr;  lift.x=f1.x; ... // 点1：原地抬 Z
    PointData go;    go.name="安全位"; go.hasJoints=true;
    go.j1=sj1; go.j2=sj2; go.j3=sz; go.j4=sr;          go.x=f2.x; ...  // 点2：水平走安全位

    ActionData act;  act.name="回安全位"; act.type=ActionType::Move;
    act.speedPercent=50; act.points={lift, go};
    SchemeData scheme; scheme.schemeName="安全位"; scheme.actions={act};

    if (!RunSequence(scheme)) return false;
    impl_->safeSession.store(true);   // RunSequence 已排队，此赋值先于 StartExecution
    return true;
}
```
配套：
- `RunSequence` 补 `impl_->stepMode.store(false);`（**隐患 1 修复**，对齐 `RunSingleAction:216`）与 `safeSession.store(false)`（与 singleSession 同点）。
- `StartExecution` 结尾 / `StartSingleExecution` 结尾 / `ClearFault` 三处与 singleSession 一同清 `safeSession`。
- `IsSafePosSession()` 返回该原子量。

### 步骤 2 `MainWindow` 接线（上升沿 + 守卫）

```cpp
// MainWindow.h 成员：bool lastHomed_ = false;
// .cpp 构造（:158 之后）：
manualPage_->SetSequenceWorker(sequenceWorker_);
connect(&HardwareManager::instance(), &HardwareManager::homeStateChanged,
        this, [this](bool homed) {
    const bool rising = homed && !lastHomed_;
    lastHomed_ = homed;
    if (!rising) return;                                       // 电平重复（单轴重回零）不触发
    if (!ConfigManager::instance().getValue<bool>("kinematics.safePos.enabled", false)) return;
    if (!sequenceWorker_ || sequenceWorker_->GetState() != SequenceWorker::WorkerState::Idle) {
        SPDLOG_WARN("[MainWindow] auto safe-pos skipped: worker not idle"); return;
    }
    sequenceWorker_->RunSafePos();
});
```
守卫策略：回零完成时有其它会话在跑 → RunSequence 静默拒（WARN），不打扰，下次沿/手动按钮补。

### 步骤 3 `ManualControlPage`（注入 + 两按钮 + 两槽）

- `.h`：`class SequenceWorker;` 前置声明、`void SetSequenceWorker(SequenceWorker*)`、`SequenceWorker* worker_ = nullptr;`、槽 `OnGoSafePos` / `OnTeachSafePos`。
- `btnRow` 内 `homeAllBtn`（`:162`）后插「回安全位」「示教安全位」两按钮（**保持局部变量**，与既有三钮风格一致；门禁在槽内前置校验 + RunSequence 硬门禁双保险）。
- `OnGoSafePos`：worker 非 Idle 提示；未回零提示；`RunSafePos()` 成功提示"正在回安全位（先抬Z再水平移动）"。
- `OnTeachSafePos`：**必须已回零**（否则位姿无意义）+ 四轴非 busy → 逐轴 `GetPosition` → `cfg.set("kinematics.safePos.j1/j2/z/r", ...)` + `cfg.set("kinematics.safePos.enabled", true)`（示教过即启用）→ 提示记录值。

### 步骤 4 `ConfigPage` + `ProcessPage` 守卫

- `CreateTab2Kinematics()`（`ConfigPage.cpp:322-364`）links Row 后照抄一组：4 个 QLineEdit（`kinematics.safePos.j1/j2/z/r`，默认 0，标签"安全位 J1(°)/J2(°)/Z(mm)/R(°)"）+ **1 个 QCheckBox「回零后自动回安全位」绑 `kinematics.safePos.enabled`**（显式开关，避免"填了值却没生效"的困惑；示教按钮也会置 true）。
- **不 emit paramsChanged**（safePos 现读 config，无内存缓存）。
- `ProcessPage.cpp:915` 附近 actionStarted lambda **首行**加 `if (m_worker->IsSafePosSession()) return;`（**隐患 2 修复**：防临时方案误选列表行/误禁单步按钮）。

## 五、急停恢复链路（验证通过）

急停 → 切手动 → 使能 → 一键回零 → `homeStateChanged(true)` 上升沿 → 自动回安全位 → 切自动启动方案。
- 急停时 worker 在跑 → cancel → interrupted → **回 Idle 非 Fault**，链路畅通。
- 边界：急停前已因非 cancel 失败进 Fault → 回零完成时 RunSequence 被 Fault 门禁拒，自动回安全位跳过（可接受：故障未清不该自动走位，先在自动页清报警）。

## 六、文件清单

| 文件 | 改动 |
|---|---|
| `src/Logic/SequenceWorker.h/.cpp` | `RunSafePos`/`IsSafePosSession`/`safeSession`；`RunSequence` 补清 stepMode |
| `src/UI/MainWindow.h/.cpp` | `lastHomed_`、`manualPage_->SetSequenceWorker`、homeStateChanged 上升沿接线 |
| `src/UI/ManualControlPage.h/.cpp` | worker 注入、两按钮两槽 |
| `src/UI/ConfigPage.cpp` | 安全位编辑组（4 输入框 + enabled checkbox，不 emit paramsChanged） |
| `src/UI/ProcessPage.cpp` | actionStarted 首行 `IsSafePosSession()` 屏蔽（一行） |
| `config/config.json` | key 由 ConfigManager 自动创建，无需手改 |
| `TEST_RECORD.md` | 记账（追加前重读最大编号；现为 TR-073 → 新记录 TR-074） |

## 七、验证

- Debug+Release 双编译 + Sim 冒烟（`Initialize complete`）。
- Sim 需先确认回零能完成（SimCard/SimServo HomeAxis 路径）→ 使能→回零→观察安全位 2 点移动（Sim 的 MarkAxisBusy ≥1s 提供观察窗口）。
- 手测：① 未配置（enabled=false）时回零无动作；② 示教安全位→回零→自动走「抬Z→安全位」；③ 手动「回安全位」按钮；④ 单步会话中途停止→立即回零→安全位会话正常完成（**stepMode 残留回归**）；⑤ ProcessPage 停留时触发回零，列表选中行不变；⑥ 单轴重回零不触发；⑦ 急停→使能→回零→自动回安全位全链路；⑧ enabled 取消勾选后回零无动作。
