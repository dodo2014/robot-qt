#include "SequenceWorker.h"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QMetaObject>

#include <atomic>
#include <cmath>
#include <vector>

#include "ConfigManager.h"
#include "Core/Kinematics.h"
#include "Core/CoordTransform.h"
#include "HAL/core/HardwareManager.h"
#include "HAL/core/AxisMap.h"
#include "spdlog/spdlog.h"

namespace {

// 在调用线程非主线程时，将 lambda 排队到主线程同步执行并返回结果。
// 保证所有 HardwareManager 硬件调用与 PollTick（主线程）串行，避免跨线程数据竞争。
// worker 线程调用会阻塞等待主线程执行完毕（主线程只短暂执行硬件操作，不会长时间卡 UI）。
template<typename F>
auto InMainThread(F&& f) -> decltype(f())
{
    if (QThread::currentThread() == QCoreApplication::instance()->thread())
        return f();
    using R = decltype(f());
    R result{};
    QMetaObject::invokeMethod(QCoreApplication::instance(),
        [&] { result = f(); },
        Qt::BlockingQueuedConnection);
    return result;
}

// 分步下发一条点位目标：先舵机（J2/R）再卡轴（J1/Z），避免机械干涉。
// 目标与读回当前值之差 ≤ ε 的轴**跳过下发**（S2c / D19 加固）：
//   · "目标 = 当前位置"本无运动意义，跳过无副作用；
//   · 关键收益：回安全位点1 只剩 Z 轴下发 → "抬 Z 阶段零水平运动"由代码保证，
//     不再依赖"舵机读回值与指令值一致"这一假设（减速停后 J2/R 读回可能有微差 δ）；
//   · 被跳过的轴不写 busy 时间戳 → IsAxisBusy 立即 false，WaitForAxes 不受影响。
// 软限位硬拦截不旁路：跳过前先过 IsWithinSoftLimits（与 MoveAbs 同源），
// 越界时**不跳过**，落到 MoveAbs 走完整的 WARN + 拒绝路径。
// **必须在主线程调用**（内部直接调 HardwareManager，与 PollTick 串行）。
bool DispatchPointMove(const Joints& joints, double v1, double v2, double vz, double vr)
{
    constexpr double kInPositionEps = 0.01;   // 0.01 mm（Z）/ 0.01°（J1/J2/R）
    auto& hw = HardwareManager::instance();
    struct Cmd { LogicalAxis axis; double target; double speed; const char* name; };
    const Cmd cmds[] = {
        { LogicalAxis::J2, joints.j2, v2, "J2" },
        { LogicalAxis::R,  joints.r,  vr, "R"  },
        { LogicalAxis::J1, joints.j1, v1, "J1" },
        { LogicalAxis::Z,  joints.z,  vz, "Z"  },
    };
    for (const auto& c : cmds) {
        if (std::fabs(c.target - hw.GetPosition(c.axis)) <= kInPositionEps
            && hw.IsWithinSoftLimits(c.axis, c.target)) {
            SPDLOG_INFO("[SequenceWorker] skip axis {} (in position)", c.name);
            continue;
        }
        if (!hw.MoveAbs(c.axis, c.target, c.speed)) return false;
    }
    return true;
}

} // namespace

class SequenceWorker::Impl
{
public:
    Kinematics     kin;
    CoordTransform coord;

    // 显式状态机（S2）。running 保留仅为兼容既有并发读写点，最终由 state 统一表达：
    //   state == Idle  ⇔ !running（正常/中断/故障清除后）
    //   state != Idle  ⇔ running（Running / Paused / Fault 都算"会话持有中"）
    std::atomic<WorkerState> state       {WorkerState::Idle};
    std::atomic<PauseReason> pauseReason {PauseReason::None};

    std::atomic<bool> running    {false};
    std::atomic<bool> cancel     {false};
    std::atomic<bool> paused     {false};   // 用户暂停请求（协作式，由 PauseGate 消费）
    std::atomic<bool> stepMode   {false};
    std::atomic<bool> stepGo     {false};
    std::atomic<bool> stepGatePending {false};   // 单步：正挂起等待 NextStep 放行
    std::atomic<bool> singleSession   {false};   // 单动作会话（RunSingleAction 入口）
    std::atomic<bool> safeSession     {false};   // 回安全位会话（RunSafePos 临时方案）

    // 暂停累计时长（ms）：Delay 等按"剩余时间"续算，暂停耗时不占用延时预算
    std::atomic<long long> pauseAccumMs {0};

    SchemeData scheme;
    int        currentIndex = -1;

    // ---- 循环生产（2026-09-14，见 doc/auto_run_loop_production.md）----
    // 注：命名 loopCfg 而非 loop——Impl::WaitForCancelOrTime / WaitForStep 内有
    // 局部变量 QEventLoop loop，同名会触发 C4458（声明隐藏类成员）。
    LoopConfig loopCfg;                 // 会话级配置副本（主线程组装 → worker 只读，不跨线程读 config）
    int        cycleIndex = 0;          // 当前轮号（1-based），仅供日志/进度

    // 安全位数值：主线程启动时快照（worker 线程绝不读 ConfigManager，json 读写非线程安全）。
    // safePoseValid = kinematics.safePos.enabled 的启动快照，兼作"已捕获"标记。
    bool   safePoseValid = false;
    double safeJ1 = 0.0;
    double safeJ2 = 0.0;
    double safeZ  = 0.0;
    double safeR  = 0.0;

    // 失败原因（D17 方案 A 单点化）：ExecuteAction 子树只写此字段，
    // emit errorOccurred 收敛到会话级 2 处出口（ExecuteOnce / StartSingleExecution）。
    // 仅 worker 线程内写/读，不跨线程共享 → 无需加锁。
    QString lastError;

    bool WaitForCancelOrTime(int ms)
    {
        QElapsedTimer t;
        t.start();
        QEventLoop loop;
        QTimer poll;
        poll.setInterval(20);
        QObject::connect(&poll, &QTimer::timeout, [&] {
            if (cancel.load() || t.elapsed() >= ms)
                loop.quit();
        });
        poll.start();
        loop.exec();
        poll.stop();
        return !cancel.load();
    }

    bool WaitForStep()
    {
        QEventLoop loop;
        QTimer poll;
        poll.setInterval(20);
        QObject::connect(&poll, &QTimer::timeout, [&] {
            if (cancel.load() || stepGo.load())
                loop.quit();
        });
        poll.start();
        loop.exec();
        poll.stop();
        stepGo.store(false);
        return !cancel.load();
    }
};

SequenceWorker::SequenceWorker(QObject* parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>())
{
    SPDLOG_INFO("[SequenceWorker] Created");
}

SequenceWorker::~SequenceWorker() = default;

void SequenceWorker::ReloadFromConfig()
{
    // 方案运行中禁止重载：主线程写 kin/coord 会与 worker 线程并发读竞争（UB）。
    // OnStartClicked 每次启动前已 ReloadFromConfig，运行中重载无价值，直接跳过。
    if (impl_->running.load()) {
        SPDLOG_WARN("[SequenceWorker] ReloadFromConfig skipped: scheme running");
        return;
    }
    auto& cfg = ConfigManager::instance();

    double l1 = cfg.getValue<double>("kinematics.links.l1", 138.83);
    double l2 = cfg.getValue<double>("kinematics.links.l2", 166.86);
    double z0 = cfg.getValue<double>("kinematics.links.z0", 0.0);
    double h1 = cfg.getValue<double>("kinematics.links.h1", 0.0);
    impl_->kin.SetParams(l1, l2, z0, h1);

    double tox = cfg.getValue<double>("tcpCalibration.toolOffsetX", 0.0);
    double toy = cfg.getValue<double>("tcpCalibration.toolOffsetY", 0.0);
    double toz = cfg.getValue<double>("tcpCalibration.toolOffsetZ", 0.0);
    impl_->kin.SetTCP(tox, toy, toz);

    auto m = cfg.getValue<std::vector<double>>("tcpCalibration.handEyeMatrix", std::vector<double>{});
    if (m.size() == 16) {
        std::array<double, 16> arr{};
        for (size_t i = 0; i < 16; ++i) arr[i] = m[i];
        impl_->coord.SetHandEyeMatrix(arr);
    }

    impl_->kin.SetJointLimits(
        cfg.getValue<double>("axes.Axis_J1.limitMin", -180.0), cfg.getValue<double>("axes.Axis_J1.limitMax", 180.0),
        cfg.getValue<double>("axes.Axis_J2.limitMin", -180.0), cfg.getValue<double>("axes.Axis_J2.limitMax", 180.0),
        cfg.getValue<double>("axes.Axis_Z.limitMin",  -1000.0), cfg.getValue<double>("axes.Axis_Z.limitMax", 1000.0),
        cfg.getValue<double>("axes.Axis_R.limitMin",  -180.0),  cfg.getValue<double>("axes.Axis_R.limitMax", 180.0));

    SPDLOG_INFO("[SequenceWorker] Kinematics reloaded: L1={:.2f} L2={:.2f} Z0={:.1f} H1={:.1f} "
                "TCP=({:.1f},{:.1f},{:.1f})",
                l1, l2, z0, h1, tox, toy, toz);
}

bool SequenceWorker::RunSequence(const SchemeData& scheme, const LoopConfig& loop)
{
    if (impl_->running.load()) {
        SPDLOG_WARN("[SequenceWorker] RunSequence rejected: already running");
        return false;
    }
    // 使能门禁：自动流程与手动界面共用同一安全门禁（先使能再运动）
    if (!InMainThread([] { return HardwareManager::instance().IsGlobalEnabled(); })) {
        SPDLOG_WARN("[SequenceWorker] RunSequence rejected: axes not enabled");
        emit errorOccurred(QStringLiteral("轴未使能，请先手动使能"));
        return false;
    }
    // 回零互锁：开环步进断电丢坐标，未回零禁止自动运行（第二道安全门禁）
    if (!InMainThread([] { return HardwareManager::instance().IsSystemHomed(); })) {
        SPDLOG_WARN("[SequenceWorker] RunSequence rejected: system not homed");
        emit errorOccurred(QStringLiteral("系统未回零，请先一键回零"));
        return false;
    }

    impl_->cancel.store(false);
    impl_->stepGo.store(false);
    impl_->paused.store(false);
    impl_->stepGatePending.store(false);
    impl_->pauseAccumMs.store(0);
    // stepMode **不在此清**：单步由 ProcessPage 先 SetStepMode(true) 再 RunSequence，
    // 此处清除会把刚设的单步标志抹掉 → 动作间不再挂起，方案一次跑完（2026-09-08 真机 8.13 实测）。
    // 清除点统一在会话结束时（StartExecution / RunSingleAction），既保证单步生效，
    // 又避免"单步残留 stepMode=true 使后续会话挂起 Paused(Step) 卡死"（TR-074 隐患 1 的原意）。
    impl_->safeSession.store(false);
    impl_->scheme = scheme;
    impl_->currentIndex = -1;
    impl_->singleSession.store(false);
    // 会话级循环状态：与本段其余项同列清空
    impl_->loopCfg = loop;
    impl_->cycleIndex = 0;
    impl_->lastError.clear();
    // 安全位数值快照（主线程）：循环间隔回安全位在 worker 线程执行，绝不能读 ConfigManager。
    // 未启用 → safePoseValid=false → 循环间隔回安全位判为故障（UI 侧另有启动门禁，此处兜底）。
    {
        auto& cfg = ConfigManager::instance();
        impl_->safePoseValid = cfg.getValue<bool>("kinematics.safePos.enabled", false);
        if (impl_->safePoseValid) {
            impl_->safeJ1 = cfg.getValue<double>("kinematics.safePos.j1", 0.0);
            impl_->safeJ2 = cfg.getValue<double>("kinematics.safePos.j2", 0.0);
            impl_->safeZ  = cfg.getValue<double>("kinematics.safePos.z",  0.0);
            impl_->safeR  = cfg.getValue<double>("kinematics.safePos.r",  0.0);
        }
    }

    SetState(WorkerState::Running);
    SPDLOG_INFO("[SequenceWorker] RunSequence: {} ({} actions) loop={} count={} safePos={} retry={}",
                scheme.schemeName.toStdString(), static_cast<int>(scheme.actions.size()),
                static_cast<int>(loop.mode), loop.count,
                loop.returnToSafePos ? "on" : "off", loop.retryCount);

    // 排队到 worker 线程执行（若已 moveToThread 则跨线程排队，否则当前线程事件循环执行）
    QMetaObject::invokeMethod(this, "StartExecution", Qt::QueuedConnection);
    return true;
}

bool SequenceWorker::RunSingleAction(const SchemeData& scheme, int actionIndex)
{
    if (impl_->running.load()) {
        SPDLOG_WARN("[SequenceWorker] RunSingleAction rejected: already running");
        return false;
    }
    if (actionIndex < 0 || actionIndex >= static_cast<int>(scheme.actions.size())) {
        SPDLOG_WARN("[SequenceWorker] RunSingleAction rejected: index out of range");
        return false;
    }
    // 使能门禁：与 RunSequence 同一安全门禁（先使能再运动）
    if (!InMainThread([] { return HardwareManager::instance().IsGlobalEnabled(); })) {
        SPDLOG_WARN("[SequenceWorker] RunSingleAction rejected: axes not enabled");
        emit errorOccurred(QStringLiteral("轴未使能，请先手动使能"));
        return false;
    }
    // 回零互锁：与 RunSequence 同一安全门禁（未回零禁止绝对运动）
    if (!InMainThread([] { return HardwareManager::instance().IsSystemHomed(); })) {
        SPDLOG_WARN("[SequenceWorker] RunSingleAction rejected: system not homed");
        emit errorOccurred(QStringLiteral("系统未回零，请先一键回零"));
        return false;
    }

    impl_->cancel.store(false);
    impl_->stepGo.store(false);
    impl_->paused.store(false);
    impl_->stepGatePending.store(false);
    impl_->pauseAccumMs.store(0);
    impl_->scheme = scheme;
    impl_->currentIndex = actionIndex;
    impl_->stepMode.store(false);   // 单动作执行与单步会话互斥（UI 侧另清 m_stepActive）
    impl_->singleSession.store(true);
    // 单动作会话不走循环外壳：清掉循环上下文，防上次循环的 loop/计数残留
    impl_->loopCfg = LoopConfig{};
    impl_->cycleIndex = 0;
    impl_->lastError.clear();

    SetState(WorkerState::Running);   // 原「执行选中动作」文案由 IsSingleActionSession() 还原
    SPDLOG_INFO("[SequenceWorker] RunSingleAction: [{}] {}", actionIndex,
                scheme.actions[actionIndex].name.toStdString());

    // 排队到 worker 线程执行（同 RunSequence 模式）
    QMetaObject::invokeMethod(this, "StartSingleExecution",
                              Qt::QueuedConnection, Q_ARG(int, actionIndex));
    return true;
}

// 构造「回安全位」动作（2026-09-14 从 RunSafePos 抽出，两条路径共用同一构造）：
// 点1 = 原地抬 Z（J1/J2/R 保持当前值）→ 点2 = 水平走安全位。
// j1/j2/z/r = **安全位**关节角（调用方给定，worker 线程不读 config）；当前位在此实时读。
// 返回 false = 安全位未就绪（未启用/未捕获）→ 调用方判为故障，不静默跳过。
bool SequenceWorker::BuildSafePosActionFrom(ActionData& act, double j1, double j2, double z, double r)
{
    if (!impl_->safePoseValid) {
        SPDLOG_WARN("[SequenceWorker] BuildSafePosActionFrom: safe pos not captured");
        return false;
    }
    // "当前位"在构造时读（主线程直通）：触发点=回零位（安全）；手动触发=操作者所见位。
    // 构造→排队执行之间轴不会动（此刻无其它指令源），即使有 ms 级偏差，点1 也只是就近抬 Z。
    const double cj1 = InMainThread([] { return HardwareManager::instance().GetPosition(LogicalAxis::J1); });
    const double cj2 = InMainThread([] { return HardwareManager::instance().GetPosition(LogicalAxis::J2); });
    const double cr  = InMainThread([] { return HardwareManager::instance().GetPosition(LogicalAxis::R);  });

    // 必须用 worker 自己的 kin 求 FK 填 x/y/z/r——MoveToPoint 的 hasJoints 路径有 ±0.5
    // 一致性校验，两者同源必过；若填 0 会静默回退 IK（可能跳到另一逆解分支）。
    const Pose f1 = impl_->kin.Forward(Joints{ cj1, cj2, z, cr });
    const Pose f2 = impl_->kin.Forward(Joints{ j1, j2, z, r });

    act = ActionData{};                               // 调用方可能复用对象，先归零防陈旧字段
    PointData lift;                                   // 点1：原地抬 Z（j1/j2/r 不变）
    lift.name = QStringLiteral("抬Z");
    lift.hasJoints = true;
    lift.j1 = cj1; lift.j2 = cj2; lift.j3 = z; lift.j4 = cr;
    lift.x = f1.x; lift.y = f1.y; lift.z = f1.z; lift.r = f1.r;

    PointData go;                                     // 点2：水平走安全位
    go.name = QStringLiteral("安全位");
    go.hasJoints = true;
    go.j1 = j1; go.j2 = j2; go.j3 = z; go.j4 = r;
    go.x = f2.x; go.y = f2.y; go.z = f2.z; go.r = f2.r;

    act.name = QStringLiteral("回安全位");
    act.type = ActionType::Move;
    act.speedPercent = 50;                            // 中低速走位，安全位不需要快
    act.points = { lift, go };
    return true;
}

// 循环间隔内联回安全位（2026-09-14）：三不——不置 safeSession、不发 actionStarted/actionFinished、
// 不重入门禁。→ ProcessPage 动作列表选中行不被联动跳走（TR-074 的 IsSafePosSession 守卫仍只服务手动触发）；
// → AutoRunPage 的安全位会话特判（TR-088 用 OnActionStarted 快照）恒为 false → 完成时仍显示普通「✅ 完成」。
bool SequenceWorker::ReturnToSafePosInline()
{
    ActionData act;
    if (!BuildSafePosActionFrom(act, impl_->safeJ1, impl_->safeJ2, impl_->safeZ, impl_->safeR))
        return false;                     // 未启用/未捕获 → 判为故障（不静默跳过）
    return ExecuteMove(act);              // 白得：cancel 检查、PauseGate、reissue 续走、逐点日志
}

// 回安全位（2026-09-07）：先原地抬 Z（防横扫），再水平走安全位关节角。
// 复用 RunSequence → ExecuteMove 的 hasJoints 路径——零新运动代码，
// 白得暂停/停止/使能/回零门禁与自动运行互斥（同一 worker 的 running 门禁）。
bool SequenceWorker::RunSafePos()
{
    auto& cfg = ConfigManager::instance();
    // 未启用（默认 false）时不动——避免出厂 config（全 0）下回零后乱走
    if (!cfg.getValue<bool>("kinematics.safePos.enabled", false)) {
        SPDLOG_INFO("[SequenceWorker] RunSafePos skipped: not enabled");
        return false;
    }
    // 与循环路径共用同一份快照（BuildSafePosActionFrom 以 safePoseValid 为就绪判据）
    impl_->safeJ1 = cfg.getValue<double>("kinematics.safePos.j1", 0.0);
    impl_->safeJ2 = cfg.getValue<double>("kinematics.safePos.j2", 0.0);
    impl_->safeZ  = cfg.getValue<double>("kinematics.safePos.z",  0.0);
    impl_->safeR  = cfg.getValue<double>("kinematics.safePos.r",  0.0);
    impl_->safePoseValid = true;

    ActionData act;
    if (!BuildSafePosActionFrom(act, impl_->safeJ1, impl_->safeJ2, impl_->safeZ, impl_->safeR))
        return false;

    SchemeData scheme;
    scheme.schemeName = QStringLiteral("安全位");
    scheme.actions = { act };

    if (!RunSequence(scheme)) return false;
    // RunSequence 内部会清 safeSession（当作普通方案），这里在排队生效前置位：
    // actionStarted 由 worker 线程 Queued 发出，必晚于此赋值 → ProcessPage 守卫可靠
    impl_->safeSession.store(true);
    // 双保险：安全位会话绝不使用单步——若此前有单步残留（异常路径），这里强制清掉，
    // 否则 1 个动作跑完会挂起在 Paused(Step) 等 NextStep → 卡死（TR-074 隐患 1）
    impl_->stepMode.store(false);
    return true;
}

bool SequenceWorker::IsSafePosSession() const { return impl_->safeSession.load(); }

void SequenceWorker::Stop()
{
    if (!impl_->running.load()) return;
    impl_->cancel.store(true);
    SPDLOG_INFO("[SequenceWorker] Stop requested");
}

// 立即停止（自动运行页【停止】专用）：cancel + 全轴减速停（保持使能），废弃上下文。
// 决策 3：与 Stop() 的语义区分——单步调试（ProcessPage）与程序退出（ShutdownWorker）
// 仍走 Stop() 的"执行完当前动作再停"（TR-063 真机语义），只有自动页要"立即减速停"。
// 注意：不能"断脉冲/断使能"——开环步进惯性滑行会丢步，坐标系错乱（Gemini 复核确认）。
// 绝不发 interrupted：由执行循环检测 cancel 后单点发出（≤20ms + 一次 InMainThread）。
void SequenceWorker::StopImmediate()
{
    if (impl_->state.load() == WorkerState::Idle) return;
    impl_->cancel.store(true);
    InMainThread([] { return HardwareManager::instance().StopAllAxes(); });
    SPDLOG_INFO("[SequenceWorker] immediate stop requested (decel stop, keep enabled)");
}

void SequenceWorker::EmergencyStop()
{
    impl_->cancel.store(true);
    InMainThread([] { return HardwareManager::instance().EmergencyStop(); });
    SPDLOG_INFO("[SequenceWorker] Emergency stop requested");
}

void SequenceWorker::SetStepMode(bool enabled)
{
    impl_->stepMode.store(enabled);
    impl_->stepGo.store(false);
    SPDLOG_INFO("[SequenceWorker] Step mode {}", enabled ? "ON" : "OFF");
}

bool SequenceWorker::NextStep()
{
    if (!impl_->running.load() || !impl_->stepMode.load()) return false;
    impl_->stepGo.store(true);
    SPDLOG_INFO("[SequenceWorker] Next step released");
    return true;
}

// ---- 状态机（S2） ----

void SequenceWorker::SetState(WorkerState s, PauseReason r)
{
    impl_->state.store(s);
    impl_->pauseReason.store(r);
    // running 跟随 state：Idle 之外都算"会话持有中"，保持 IsRunning 既有语义
    // （单步暂停时 running 为 true，MainWindow::IsExecutionActive / ProcessPage 零回归）
    impl_->running.store(s != WorkerState::Idle);
    emit stateChanged(s, r);
    SPDLOG_INFO("[SequenceWorker] state -> {} reason {}",
                static_cast<int>(s), static_cast<int>(r));
}

bool SequenceWorker::Pause()
{
    if (impl_->state.load() != WorkerState::Running) return false;
    // 单步等待放行时拒绝：此时应点「继续/单步」放行，Pause 会掩盖真实意图
    if (impl_->stepGatePending.load()) return false;
    impl_->paused.store(true);
    SPDLOG_INFO("[SequenceWorker] pause requested (cooperative, takes effect at next gate)");
    return true;
}

bool SequenceWorker::Resume()
{
    if (impl_->state.load() != WorkerState::Paused) return false;
    if (impl_->stepGatePending.load()) {
        impl_->stepGo.store(true);      // 单步等待 → 等价于放行下一步
    } else {
        impl_->paused.store(false);     // 用户暂停 → 解除挂起（PauseGate 循环感知后重发并继续）
    }
    SPDLOG_INFO("[SequenceWorker] resume requested");
    return true;
}

void SequenceWorker::ClearFault()
{
    if (impl_->state.load() != WorkerState::Fault) return;
    impl_->cancel.store(false);
    impl_->paused.store(false);
    impl_->pauseAccumMs.store(0);
    impl_->currentIndex = -1;
    impl_->singleSession.store(false);
    impl_->safeSession.store(false);
    // 停止/故障 = 废弃上下文：循环计数一并作废（无断点续产——坐标不可信）
    impl_->loopCfg = LoopConfig{};
    impl_->cycleIndex = 0;
    impl_->lastError.clear();
    SetState(WorkerState::Idle);
    SPDLOG_INFO("[SequenceWorker] fault latch cleared (hardware enable NOT touched)");
}

bool SequenceWorker::IsRunning() const   { return impl_->state.load() != WorkerState::Idle; }
bool SequenceWorker::IsPaused() const    { return impl_->state.load() == WorkerState::Paused; }
bool SequenceWorker::IsStepMode() const  { return impl_->stepMode.load(); }

SequenceWorker::WorkerState  SequenceWorker::GetState() const        { return impl_->state.load(); }
SequenceWorker::PauseReason  SequenceWorker::GetPauseReason() const  { return impl_->pauseReason.load(); }
bool SequenceWorker::IsSingleActionSession() const { return impl_->singleSession.load(); }
bool SequenceWorker::IsStepGatePending() const     { return impl_->stepGatePending.load(); }

void SequenceWorker::StartExecution()
{
    if (ExecuteActions()) {
        SPDLOG_INFO("[SequenceWorker] Scheme finished: {}", impl_->scheme.schemeName.toStdString());
        emit schemeFinished();
    }
    impl_->safeSession.store(false);
    // 会话结束即清 stepMode：单步标志只在本次会话内有效，防止残留影响后续会话
    // （隐患 1 的清除点：原放在 RunSequence 开头会抹掉单步自身标志，见 :174 注释）
    impl_->stepMode.store(false);
    // 故障态保持锁存（待 ClearFault 清除），其余一律回空闲
    if (impl_->state.load() != WorkerState::Fault)
        SetState(WorkerState::Idle);
}

void SequenceWorker::StartSingleExecution(int index)
{
    // 防御：调用链（RunSingleAction 已校验）保证合法，此处兜底防越界 UB
    if (index < 0 || index >= static_cast<int>(impl_->scheme.actions.size())) {
        SPDLOG_WARN("[SequenceWorker] StartSingleExecution: index out of range {}", index);
        SetState(WorkerState::Idle);
        return;
    }
    const auto& action = impl_->scheme.actions[index];
    impl_->lastError.clear();          // 防上一动作的陈旧原因被重发（D17 方案 A）
    emit actionStarted(index, action.name);
    SPDLOG_INFO("[SequenceWorker] Single action {}: {}", index, action.name.toStdString());

    bool ok = ExecuteAction(action, index);
    if (!ok) {
        // ExecuteAction 子树**不再 emit**（D17 方案 A 单点化），失败原因写入 Impl::lastError；
        // 此处是单动作会话的唯一出口。lastError 为空（如 MoveAbs 静默失败）才回退动作名。
        if (impl_->cancel.load())
            emit interrupted(QStringLiteral("用户停止"));
        else
            emit errorOccurred(impl_->lastError.isEmpty() ? action.name : impl_->lastError);
    } else {
        emit actionFinished(index, action.name);
    }
    emit singleActionFinished(index);
    impl_->singleSession.store(false);
    impl_->safeSession.store(false);
    // 故障态保持锁存（待 ClearFault 清除），其余一律回空闲
    if (impl_->state.load() != WorkerState::Fault)
        SetState(WorkerState::Idle);
}

// 循环外壳（2026-09-14）：按 LoopConfig 重复 ExecuteOnce。
// 为何放在会话内（而非 StartExecution 外层或 RunSequence 层）：前者需重复门禁/状态置位且拆散
// schemeFinished 语义；后者会阻塞主线程，与既有线程模型冲突。放会话内 → cancel / PauseGate /
// Fault / schemeFinished 全部复用既有单点，改动面最小。
// schemeFinished 只在**全部循环完成**时由 StartExecution 发一次（每轮发 cycleChanged）——
// 否则机器还在动、UI 已显示「✅ 完成」，操作员可能据此伸手取料（见方案 §八）。
bool SequenceWorker::ExecuteActions()
{
    const LoopConfig loop = impl_->loopCfg;   // 值快照：循环期间不受外部影响
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
        // ③ 循环间隔回安全位：第 1 轮之前不回（操作员已把机器停在起始位），
        //    最后一轮之后不回（保持停机位便于取料）。结束位直奔下一轮起点会横扫桌面撞料（TR-074）。
        if (cycle > 1 && loop.returnToSafePos) {
            emit logMessage(QStringLiteral("循环间隔：回安全位（先抬 Z 再水平）"));
            if (!ReturnToSafePosInline()) {
                if (impl_->cancel.load()) {
                    emit interrupted(QStringLiteral("用户停止"));
                } else {
                    SetState(WorkerState::Fault);
                    emit errorOccurred(QStringLiteral("循环间隔回安全位失败"));
                }
                return false;
            }
        }

        impl_->cycleIndex = cycle;
        emit cycleChanged(cycle, total);                       // ④ UI 进度
        emit logMessage(QStringLiteral("=== 循环 %1/%2 开始 ===")
                            .arg(cycle)
                            .arg(total > 0 ? QString::number(total) : QStringLiteral("∞")));

        if (!ExecuteOnce()) return false;                      // ⑤ 单轮（失败原因已在内部单点发出）

        emit logMessage(QStringLiteral("=== 循环 %1/%2 完成 ===")
                            .arg(cycle)
                            .arg(total > 0 ? QString::number(total) : QStringLiteral("∞")));
        if (loop.mode == LoopConfig::Mode::Single) break;
        if (total > 0 && cycle >= total) break;
    }
    return true;
}

// 单轮执行：原 ExecuteActions 主体原样搬迁，**仅动作调用点一处**改为 ExecuteActionWithRetry
// （其余 cancel 检查 / PauseGate / currentIndex / actionStarted / 失败→Fault 锁存 / 单步挂起一行不改）。
bool SequenceWorker::ExecuteOnce()
{
    const auto& actions = impl_->scheme.actions;
    for (int i = 0; i < actions.size(); ++i) {
        if (impl_->cancel.load()) {
            emit interrupted(QStringLiteral("用户停止"));
            return false;
        }
        // 动作边界挂起点（无轴参与 → 只挂起）：单步/无硬件动作的暂停在此生效
        if (!PauseGate()) {
            emit interrupted(QStringLiteral("用户停止"));
            return false;
        }
        impl_->currentIndex = i;
        const auto& action = actions[i];
        impl_->lastError.clear();      // 防上一动作的陈旧原因被重发（D17 方案 A）
        emit actionStarted(i, action.name);
        SPDLOG_INFO("[SequenceWorker] Action {}: {}", i, action.name.toStdString());

        if (!ExecuteActionWithRetry(action, i)) {
            if (impl_->cancel.load()) {
                emit interrupted(QStringLiteral("用户停止"));
            } else {
                // 故障锁存：保留上下文（scheme / currentIndex），待 ClearFault 才回 Idle；
                // 期间 RunSequence 会因 state != Idle 被拒，UI 也能据此禁用「启动」
                SetState(WorkerState::Fault);
                // 【D17 方案 A】原为 emit errorOccurred(action.name)：改用 lastError（具体原因，
                // 含坐标/轴号），为空才回退动作名——顺带修掉"具体原因被 action.name 覆盖"的既有瑕疵
                emit errorOccurred(impl_->lastError.isEmpty() ? action.name : impl_->lastError);
            }
            return false;
        }

        emit actionFinished(i, action.name);

        // 单步模式：每个动作完成后挂起，等待 NextStep()
        if (impl_->stepMode.load()) {
            impl_->stepGatePending.store(true);
            SetState(WorkerState::Paused, PauseReason::Step);
            SPDLOG_INFO("[SequenceWorker] Step mode: pausing after action {}", i);
            const bool go = impl_->WaitForStep();
            impl_->stepGatePending.store(false);
            if (!go) {
                emit interrupted(QStringLiteral("用户停止"));
                return false;
            }
            SetState(WorkerState::Running);
        }
    }
    return true;
}

// 重试等待（2026-09-14）：与 ExecuteDelay 同构（for(;;) + PauseGate），暂停/停止/急停均生效，
// 暂停时长不计入间隔预算。**不可用 Impl::WaitForCancelOrTime**——它只处理 cancel、不处理 paused。
bool SequenceWorker::WaitRetryDelay(int ms)
{
    if (ms <= 0) return !impl_->cancel.load();
    QElapsedTimer t;
    t.start();
    long long pauseAccum = 0;
    for (;;) {
        if (impl_->cancel.load()) return false;
        if (impl_->paused.load()) {
            const long long effectiveBeforePause = t.elapsed() - pauseAccum;
            if (!PauseGate()) return false;
            pauseAccum = t.elapsed() - effectiveBeforePause;
            continue;
        }
        if (t.elapsed() - pauseAccum >= ms) return true;
        QThread::msleep(20);
    }
}

// 动作级重试包装（2026-09-14）。retryCount=0（默认）时**直通** ExecuteAction → 零额外行为，
// 与今天逐字节一致。retryCount=N>0 时：仅 Vision（重新采图定位）与 Move（重新下发目标）可重试；
// Extrude / Gripper / 回抽 / Delay 重复执行有重复出料、重复夹取风险 → 直接失败。
// 本函数**不 emit errorOccurred**：重试期间只写 logMessage，失败原因留在 lastError 由会话级出口发出。
bool SequenceWorker::ExecuteActionWithRetry(const ActionData& action, int index)
{
    if (impl_->loopCfg.retryCount <= 0)
        return ExecuteAction(action, index);

    const int maxTry = impl_->loopCfg.retryCount + 1;
    const bool retryable = (action.type == ActionType::Vision || action.type == ActionType::Move);

    for (int attempt = 1; attempt <= maxTry; ++attempt) {
        impl_->lastError.clear();
        if (ExecuteAction(action, index)) return true;
        if (impl_->cancel.load()) return false;             // 停止/急停：不重试
        if (!retryable || attempt == maxTry) break;         // 不可重试 或 已耗尽 → 上层 Fault
        emit logMessage(QStringLiteral("动作「%1」失败，%2ms 后重试（%3/%4）")
                            .arg(action.name)
                            .arg(impl_->loopCfg.retryDelayMs)
                            .arg(attempt)
                            .arg(maxTry - 1));
        if (!WaitRetryDelay(impl_->loopCfg.retryDelayMs)) return false;   // 可被暂停/停止打断
    }
    return false;   // 失败原因留在 lastError，由会话级出口单点发出
}

// 动作分派。**本函数及其子树一律不 emit errorOccurred**（D17 方案 A）：
// 失败原因写入 Impl::lastError，由会话级唯一出口发出（ExecuteOnce / StartSingleExecution）。
// 目的：重试（retryCount>0）期间不误报故障，UI 状态不跳「⛔ 错误」。
bool SequenceWorker::ExecuteAction(const ActionData& action, int index)
{
    switch (action.type) {
    case ActionType::Move:    return ExecuteMove(action);
    case ActionType::Vision:  return ExecuteVision(action);
    case ActionType::Extrude: return ExecuteExtrude(action);
    case ActionType::Delay:   return ExecuteDelay(action);
    case ActionType::Gripper: return ExecuteGripper(action);
    default:
        SPDLOG_WARN("[SequenceWorker] Unknown action type {}", static_cast<int>(action.type));
        impl_->lastError = QStringLiteral("未知动作类型 (index %1)").arg(index);
        return false;
    }
}

bool SequenceWorker::ExecuteMove(const ActionData& action)
{
    const double speedScale = qBound(0.01, action.speedPercent / 100.0, 1.0);
    for (int p = 0; p < action.points.size(); ++p) {
        if (impl_->cancel.load()) return false;
        // 点边界挂起点（无轴参与 → 只挂起不下发停止）：保证暂停一定生效
        if (!PauseGate()) return false;
        emit logMessage(QStringLiteral("移动 → 点 %1 (%2)").arg(p + 1).arg(action.points[p].name));
        if (!MoveToPoint(action.points[p], speedScale))
            return false;
    }
    return true;
}

bool SequenceWorker::MoveToPoint(const PointData& pt, double speedScale)
{
    auto& hw = HardwareManager::instance();

    const double curJ2 = InMainThread([&] { return hw.GetPosition(LogicalAxis::J2); });

    Pose target{ pt.x, pt.y, pt.z, pt.r };
    // R 目标 ±180 wrap 归一化（必须在 IK 之前）：旧方案/边界示教可能存 wrap 值（-179.9 ≡ 180.1），
    // Kinematics::ValidateJoints 按 [rMin,rMax] 拒绝 wrap 值会报"目标点不可达"
    target.r = InMainThread([&] { return hw.NormalizeRotationAngle(LogicalAxis::R, target.r); });

    Joints joints;
    bool usedTeach = false;
    if (pt.hasJoints) {
        // 示教关节角路径：j1/j2=逻辑关节角、j3=Z 电机坐标（MoveAbs 同坐标系）、j4=R 角，
        // 直接下发，确定性复现示教姿态（无 IK 双解歧义）
        Joints j{ pt.j1, pt.j2, pt.j3, pt.j4 };
        j.r = InMainThread([&] { return hw.NormalizeRotationAngle(LogicalAxis::R, j.r); });
        // FK 一致性校验（±0.5mm / ±0.5°）：joints 与 coord 不匹配（json 手改未走 UI 作废、
        // 旧版硬编码全 0 占位残留等）时回退 IK，绝不按错误关节角运动。
        // 例：旧版 save 对所有点硬编码写 joints=0.0，fill_start(85,92,22) 若采信 J(0,0,0)
        // 会走到 (358.69,0,165)——校验失败即回退，位置仍正确
        const Pose fk = impl_->kin.Forward(j);
        double dr = std::fabs(fk.r - pt.r);
        while (dr > 180.0) dr = std::fabs(dr - 360.0);   // ±180 回绕感知的角度差
        if (std::fabs(fk.x - pt.x) <= 0.5 && std::fabs(fk.y - pt.y) <= 0.5 &&
            std::fabs(fk.z - pt.z) <= 0.5 && dr <= 0.5) {
            joints = j;
            usedTeach = true;
        } else {
            SPDLOG_WARN("[SequenceWorker] joints/coord mismatch at '{}' "
                        "(FK {:.2f},{:.2f},{:.2f} vs coord {:.2f},{:.2f},{:.2f}), fallback to IK",
                        pt.name.toStdString(), fk.x, fk.y, fk.z, pt.x, pt.y, pt.z);
        }
    }
    if (!usedTeach) {
        if (!impl_->kin.InverseSmart(target, joints, curJ2)) {
            SPDLOG_WARN("[SequenceWorker] IK failed for point ({:.1f}, {:.1f}, {:.1f}) r={:.1f}",
                        pt.x, pt.y, pt.z, pt.r);
            impl_->lastError = QStringLiteral("目标点不可达：(%1, %2, %3)").arg(pt.x).arg(pt.y).arg(pt.z);
            return false;
        }
        SPDLOG_INFO("[SequenceWorker] IK → J({:.2f}, {:.2f}, {:.2f}, {:.2f})",
                    joints.j1, joints.j2, joints.z, joints.r);
    } else {
        SPDLOG_INFO("[SequenceWorker] Teach joints → J({:.2f}, {:.2f}, {:.2f}, {:.2f})",
                    joints.j1, joints.j2, joints.z, joints.r);
    }

    // 各轴速度：maxSpeed × speedPercent
    const double v1 = InMainThread([&] { return hw.GetMaxSpeed(LogicalAxis::J1); }) * speedScale;
    const double v2 = InMainThread([&] { return hw.GetMaxSpeed(LogicalAxis::J2); }) * speedScale;
    const double vz = InMainThread([&] { return hw.GetMaxSpeed(LogicalAxis::Z); }) * speedScale;
    const double vr = InMainThread([&] { return hw.GetMaxSpeed(LogicalAxis::R); }) * speedScale;

    // 分步移动：先舵机（J2/R）再卡轴（J1/Z），避免机械干涉。
    // 已在位轴（差 ≤ 0.01）跳过下发 —— 见 DispatchPointMove 注释（S2c / D19 加固）。
    if (!InMainThread([&] { return DispatchPointMove(joints, v1, v2, vz, vr); })) return false;

    // 等待全部轴到位（IsAxisBusy 时间戳 + 轮询；超时 30s 兜底）
    QVector<LogicalAxis> axes{ LogicalAxis::J1, LogicalAxis::J2, LogicalAxis::Z, LogicalAxis::R };
    // 暂停恢复用：重发**当前点**的绝对目标。MoveAbs 是绝对坐标，
    // 从半路续走到目标不会累积误差，也不会回退到上一个点。
    // （在 PauseGate 内已切到主线程执行，无需再包 InMainThread）
    auto reissue = [joints, v1, v2, vz, vr]() {
        // 与首次下发同规则（含在位轴跳过）：恢复瞬间不把已在位轴重下发
        return DispatchPointMove(joints, v1, v2, vz, vr);
    };
    if (!WaitForAxes(axes, 30000, reissue)) {
        if (impl_->cancel.load()) return false;
        SPDLOG_WARN("[SequenceWorker] WaitForAxes timeout at point ({:.1f}, {:.1f}, {:.1f})",
                    pt.x, pt.y, pt.z);
        impl_->lastError = QStringLiteral("移动到位超时");
        return false;
    }
    return true;
}

bool SequenceWorker::WaitForAxes(const QVector<LogicalAxis>& axes, int timeoutMs,
                                 std::function<bool()> reissue)
{
    auto& hw = HardwareManager::instance();
    QElapsedTimer t;
    t.start();
    for (;;) {
        // 顺序约束：cancel/paused 检查必须在 allDone 判定**之前**——
        // 否则减速停清 busy 后会被误判成"已到位"，导致动作假成功
        if (impl_->cancel.load()) return false;
        if (impl_->paused.load()) {
            if (!PauseGate(axes, reissue)) return false;   // 暂停中被 Stop/急停 → 终止动作
            t.restart();                                    // 暂停时长不占超时预算
            continue;
        }
        bool allDone = true;
        for (auto a : axes) {
            if (InMainThread([&] { return hw.IsAxisBusy(a); })) { allDone = false; break; }
        }
        if (allDone) return true;
        if (timeoutMs > 0 && t.elapsed() > timeoutMs) return false;
        QThread::msleep(20);
    }
}

bool SequenceWorker::PauseGate(const QVector<LogicalAxis>& axes,
                               std::function<bool()> reissue)
{
    if (!impl_->paused.load()) return true;

    SetState(WorkerState::Paused, PauseReason::User);

    // 减速停当前参与轴：卡轴 MC_Stop 斜坡（BoPaiCard.cpp:327，非断脉冲 → 开环不丢步）、
    // 舵机 Stop() 停+保持锁力（XRServo.cpp:468）。**不断使能**（区别于 EmergencyStop）。
    // 注：StopAxes 内部 AbortHoming 仅复位"正在回零中"的轴（HardwareManager.cpp:721），
    //     已回零的轴 homed 保持 true → 恢复后 MoveAbs 不会被回零互锁拒绝。
    if (!axes.isEmpty())
        InMainThread([&] { return HardwareManager::instance().StopAxes(axes); });

    // 与 WaitForStep 同构的挂起门闩（QEventLoop + 20ms 轮询）
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(20);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        if (impl_->cancel.load() || !impl_->paused.load())
            loop.quit();
    });
    poll.start();
    loop.exec();
    poll.stop();

    if (impl_->cancel.load()) return false;   // 暂停中被 Stop / 急停 → 终止当前动作

    // Resume：重发**当前那条**绝对目标 MoveAbs，从半路续走到目标。
    // 决策要点：不重试整个动作——挤出/回抽分段动作重试会把挤出轴反向补回满量程（多挤料），
    // 且 Delay 会重新完整计时、Vision 会重新采图、多点点位会重发已完成点（可能跳到另一逆解分支）。
    if (reissue) InMainThread([&] { return reissue(); });

    SetState(WorkerState::Running);
    return true;
}

bool SequenceWorker::ExecuteVision(const ActionData& action)
{
    auto& hw = HardwareManager::instance();

    auto* camera = InMainThread([&] { return hw.camera(); });
    auto* algo   = InMainThread([&] { return hw.algorithm(); });
    if (!camera || !algo) {
        SPDLOG_WARN("[SequenceWorker] Vision: camera/algo not available, simulate");
        emit logMessage(QStringLiteral("视觉：无相机/算法，模拟延时"));
        return impl_->WaitForCancelOrTime(500);
    }

    emit logMessage(QStringLiteral("视觉：采帧中 (%1)...").arg(action.visionType));
    CameraFrame frame = InMainThread([&] { return camera->CaptureFrame(); });

    emit logMessage(QStringLiteral("视觉：检测中 (阈值 %1)...").arg(action.threshold));
    auto results = InMainThread([&] { return algo->Detect(frame); });

    if (results.empty()) {
        SPDLOG_WARN("[SequenceWorker] Vision: no target detected");
        impl_->lastError = QStringLiteral("视觉未检出目标");
        return false;
    }

    const auto& best = results.front();
    Pose robot = impl_->coord.CameraToRobot(best.x, best.y, best.z);
    SPDLOG_INFO("[SequenceWorker] Vision target: cam({:.1f},{:.1f},{:.1f}) conf={:.2f} → robot({:.1f},{:.1f},{:.1f})",
                best.x, best.y, best.z, best.confidence, robot.x, robot.y, robot.z);
    emit logMessage(QStringLiteral("视觉定位：基座 (%1, %2, %3) 置信度 %4")
                        .arg(robot.x, 0, 'f', 1)
                        .arg(robot.y, 0, 'f', 1)
                        .arg(robot.z, 0, 'f', 1)
                        .arg(best.confidence, 0, 'f', 2));
    return true;
}

bool SequenceWorker::ExecuteExtrude(const ActionData& action)
{
    auto& hw = HardwareManager::instance();

    if (action.extrudeAmount > 0.0) {
        emit logMessage(QStringLiteral("挤出 %1 mm").arg(action.extrudeAmount));
        if (!InMainThread([&] { return hw.MoveAbs(LogicalAxis::Extruder, action.extrudeAmount, action.extrudeSpeed); }))
            return false;
        QVector<LogicalAxis> axes{ LogicalAxis::Extruder };
        // 续走只重发"挤出段"目标（绝对量），不会重复挤出
        auto reissue = [this, action]() {
            return HardwareManager::instance().MoveAbs(
                LogicalAxis::Extruder, action.extrudeAmount, action.extrudeSpeed);
        };
        if (!WaitForAxes(axes, 10000, reissue)) {
            if (impl_->cancel.load()) return false;
            impl_->lastError = QStringLiteral("挤出到位超时");
            return false;
        }
    }
    if (action.suckBackAmount > 0.0) {
        emit logMessage(QStringLiteral("回抽 %1 mm").arg(action.suckBackAmount));
        const double target = action.extrudeAmount - action.suckBackAmount;
        if (!InMainThread([&] { return hw.MoveAbs(LogicalAxis::Extruder, target, action.suckBackSpeed); }))
            return false;
        QVector<LogicalAxis> axes{ LogicalAxis::Extruder };
        // 续走只重发"回抽段"目标（绝对量 target），**绝不重发挤出段**——
        // 否则会把挤出轴反向补回满量程再回抽，等于多挤一份料
        const double suckTarget = target;
        auto reissue = [this, suckTarget, action]() {
            return HardwareManager::instance().MoveAbs(
                LogicalAxis::Extruder, suckTarget, action.suckBackSpeed);
        };
        if (!WaitForAxes(axes, 10000, reissue)) {
            if (impl_->cancel.load()) return false;
            impl_->lastError = QStringLiteral("回抽到位超时");
            return false;
        }
    }
    return true;
}

bool SequenceWorker::ExecuteDelay(const ActionData& action)
{
    emit logMessage(QStringLiteral("延时 %1 ms").arg(action.delayMs));
    // 暂停感知：暂停时长不计入延时预算——按"剩余时间"续算，而不是恢复后重新完整计时
    // （10s 延时跑到第 8s 被暂停，恢复后只应再等约 2s）
    QElapsedTimer t;
    t.start();
    long long pauseAccum = 0;
    for (;;) {
        if (impl_->cancel.load()) return false;
        if (impl_->paused.load()) {
            const long long effectiveBeforePause = t.elapsed() - pauseAccum;
            if (!PauseGate()) return false;          // 暂停中被 Stop / 急停 → 终止
            pauseAccum = t.elapsed() - effectiveBeforePause;   // 把暂停段计入 offset
            continue;
        }
        if (t.elapsed() - pauseAccum >= action.delayMs) return true;
        QThread::msleep(20);
    }
}

bool SequenceWorker::ExecuteGripper(const ActionData& action)
{
    auto& hw = HardwareManager::instance();
    // 行程 = 轴5 绝对目标坐标（mm），语义与手动控制页轴5 Go 一致：0 = 夹紧、负值 = 松开
    const double lo = InMainThread([&] { return hw.GetLimitMin(LogicalAxis::Gripper); });
    const double hi = InMainThread([&] { return hw.GetLimitMax(LogicalAxis::Gripper); });
    const double target = action.gripperTarget;
    // 软限位硬性拦截（与手动页 Go / MoveAbs 内部同源）：越界拒绝，绝不静默改写目标
    if (!InMainThread([&] { return hw.IsWithinSoftLimits(LogicalAxis::Gripper, target); })) {
        SPDLOG_WARN("[SequenceWorker] Gripper target {:.2f} out of soft limits [{:.2f}, {:.2f}]",
                    target, lo, hi);
        impl_->lastError = QStringLiteral("夹爪目标行程 %1 mm 超出轴5软限位（%2 ~ %3 mm）")
                               .arg(QString::number(target, 'f', 2),
                                    QString::number(lo, 'f', 2), QString::number(hi, 'f', 2));
        return false;
    }
    // 速度映射到轴5：maxSpeed × speedPercent（与 MoveToPoint 同源），MoveAbs 内部再按卡上限截断
    const double speedScale = qBound(0.01, action.speedPercent / 100.0, 1.0);
    const double speed = InMainThread([&] { return hw.GetMaxSpeed(LogicalAxis::Gripper); }) * speedScale;
    emit logMessage(QStringLiteral("夹爪%1 → 目标 %2 mm（速度 %3 mm/s）")
                        .arg(action.isGripperOpen ? QStringLiteral("张开") : QStringLiteral("闭合"),
                             QString::number(target, 'f', 2), QString::number(speed, 'f', 2)));
    if (!InMainThread([&] { return hw.MoveAbs(LogicalAxis::Gripper, target, speed); }))
        return false;
    // 到位等待：按 MoveAbs 里 MarkAxisBusy 估算的耗时动态兜底——
    // 低速（如 10% ≈ 0.2mm/s 走 3mm 需 15s）时固定 10s 会误判超时；下限 3000 防 est=0 无限等待
    const int estMs = InMainThread([&] { return hw.GetAxisBusyMs(LogicalAxis::Gripper); });
    QVector<LogicalAxis> axes{ LogicalAxis::Gripper };
    // 续走只重发当前目标（绝对行程），不重复走已完成段
    auto reissue = [this, target, speed]() {
        return HardwareManager::instance().MoveAbs(LogicalAxis::Gripper, target, speed);
    };
    if (!WaitForAxes(axes, qMax(3000, static_cast<int>(estMs * 1.2) + 3000), reissue)) {
        if (impl_->cancel.load()) return false;
        impl_->lastError = QStringLiteral("夹爪到位超时");
        return false;
    }
    return true;
}