#include "SequenceWorker.h"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QHash>

#include <atomic>
#include <cmath>
#include <vector>

#include "ConfigManager.h"
#include "Core/Kinematics.h"
#include "Core/CoordTransform.h"
#include "HAL/core/HardwareManager.h"
#include "HAL/core/AxisMap.h"
#include "spdlog/spdlog.h"

// 跨线程 Queued 传递 `detectionReady` 的实参注册（P3 S6 决策 6）。
// 文件级静态初始化 → 在 `AutoRunPage::SetSequenceWorker` 建立连接之前就已生效。
// 同款做法见 `CameraCaptureWorker.cpp` 的 `qRegisterMetaType<CameraFramePtr>`。
namespace {
const int g_detectionMetaRegistered = []() {
    qRegisterMetaType<SequenceWorker::DetectionResult>("SequenceWorker::DetectionResult");
    return 0;
}();
}  // namespace

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
//   · 被跳过的轴不写 busy 时间戳 → IsAxisBusy 立即 false，到位轮询不受影响。
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

    // 会话视觉结果表（P5，doc/vision_guide_action_plan.md §5.4）：key = **识别动作的 id**。
    // · 写入：识别分支末尾（StepRunner 的 case ActionType::Vision；只取 results.front()，与既有"最佳目标"语义一致）；
    // · 清空：**每次会话开始**（RunSequence / RunSingleAction）——防上次会话的残留结果
    //   被本次误用（与 TR-091「循环外壳在会话内」同类问题）；
    // · 循环生产：每轮识别覆盖同 id 的结果，语义正确（每轮重新识别）；
    // · 只在 worker 线程读写（UI 侧靠 visionResultReady 信号取副本）→ 无需加锁。
    QHash<int, Pose> visionResults_;

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
    // 会话视觉结果表清空（P5 §5.4）：上次会话识别出的坐标对本次无效——相机眼在手外，
    // 但物料已被取走/换位，复用旧坐标会把动作引到空处。
    impl_->visionResults_.clear();
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
    SPDLOG_INFO("[SequenceWorker] RunSequence: {} ({} nodes) loop={} count={} safePos={} retry={}",
                scheme.schemeName.toStdString(), static_cast<int>(scheme.nodes.size()),
                static_cast<int>(loop.mode), loop.count,
                loop.returnToSafePos ? "on" : "off", loop.retryCount);

    // 排队到 worker 线程执行（若已 moveToThread 则跨线程排队，否则当前线程事件循环执行）
    QMetaObject::invokeMethod(this, "StartExecution", Qt::QueuedConnection);
    return true;
}

bool SequenceWorker::RunSingleAction(const SchemeData& scheme, int nodeId)
{
    if (impl_->running.load()) {
        SPDLOG_WARN("[SequenceWorker] RunSingleAction rejected: already running");
        return false;
    }
    // P6：按**节点编号**定位（不再是扁平行号）——编号持久化且不随插入/删除漂移，
    // 分组同样占编号，故这里既能执行单个动作，也能执行整个分组。
    if (!ProcessManager::findNode(scheme.nodes, nodeId)) {
        SPDLOG_WARN("[SequenceWorker] RunSingleAction rejected: node #{} not found", nodeId);
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
    impl_->currentIndex = nodeId;    // 语义：当前节点编号（P6）
    impl_->stepMode.store(false);   // 单动作执行与单步会话互斥（UI 侧另清 m_stepActive）
    impl_->singleSession.store(true);
    // 单动作会话不走循环外壳：清掉循环上下文，防上次循环的 loop/计数残留
    impl_->loopCfg = LoopConfig{};
    impl_->cycleIndex = 0;
    impl_->lastError.clear();
    // 同 RunSequence：单动作会话也是新会话，视觉结果表清空。
    // 直接单跑一条「视觉引导」必然取不到结果 → 报错且不运动（§4.3 执行期校验），这是预期行为。
    impl_->visionResults_.clear();

    SetState(WorkerState::Running);   // 原「执行选中动作」文案由 IsSingleActionSession() 还原
    const ActionData* node = ProcessManager::findNode(scheme.nodes, nodeId);
    SPDLOG_INFO("[SequenceWorker] RunSingleAction: node #{} {}", nodeId,
                node ? node->name.toStdString() : std::string("(missing)"));

    // 排队到 worker 线程执行（同 RunSequence 模式）
    QMetaObject::invokeMethod(this, "StartSingleExecution",
                              Qt::QueuedConnection, Q_ARG(int, nodeId));
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

    // 必须用 worker 自己的 kin 求 FK 填 x/y/z/r——RunnerState 的 hasJoints 路径有 ±0.5
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
    // 单元素调度器（P6：原 ExecuteMove 的阻塞式实现已并入 RunnerState 状态机）
    return ExecuteAction(act, -1);        // 白得：cancel 检查、暂停、reissue 续走、逐点日志
}

// 回安全位（2026-09-07）：先原地抬 Z（防横扫），再水平走安全位关节角。
// 复用 RunSequence → ExecuteAction 的 hasJoints 路径——零新运动代码，
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
    scheme.nodes = { act };

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

void SequenceWorker::StartSingleExecution(int nodeId)
{
    // P6：按节点编号定位（调用链 RunSingleAction 已校验，此处兜底防悬垂编号）
    const ActionData* found = ProcessManager::findNode(impl_->scheme.nodes, nodeId);
    if (!found) {
        SPDLOG_WARN("[SequenceWorker] StartSingleExecution: node #{} not found", nodeId);
        SetState(WorkerState::Idle);
        return;
    }
    const ActionData action = *found;   // 值拷贝：与主线程后续编辑解耦
    impl_->lastError.clear();           // 防上一节点的陈旧原因被重发（D17 方案 A）
    emit actionStarted(nodeId, action.name);
    SPDLOG_INFO("[SequenceWorker] Single node #{}: {}", nodeId, action.name.toStdString());

    bool ok = ExecuteNode(action, nodeId);
    if (!ok) {
        // ExecuteNode 子树**不再 emit**（D17 方案 A 单点化），失败原因写入 Impl::lastError；
        // 此处是单节点会话的唯一出口。lastError 为空（如 MoveAbs 静默失败）才回退节点名。
        if (impl_->cancel.load())
            emit interrupted(QStringLiteral("用户停止"));
        else
            emit errorOccurred(impl_->lastError.isEmpty() ? action.name : impl_->lastError);
    } else {
        emit actionFinished(nodeId, action.name);
    }
    emit singleActionFinished(nodeId);
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
    // total 语义：> 0 = 有限轮数（`Single` 即 1 轮），-1 = 无限循环。
    // ⚠ `Single` **不可给 -1**：下游（本轮日志文案、`AutoRunPage::OnCycleChanged`）只看得到
    // total，无法区分 `Single` 与 `Infinite`，给 -1 会把「单轮」渲染成「循环 1/∞」——
    // 操作员会误以为在无限循环，实际跑完 1 轮就结束。
    const int total = (loop.mode == LoopConfig::Mode::Infinite)
                          ? -1
                          : (loop.mode == LoopConfig::Mode::Single ? 1 : qMax(1, loop.count));

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

// 单轮执行：原 ExecuteActions 主体原样搬迁，**仅两处**改动——
// ① 遍历 `scheme.nodes`（两级树的**一级节点**）而非旧的扁平 `actions`；
// ② 调用点改为 ExecuteNodeWithRetry（节点级重试，分组按 §Q3 重试整组）。
// 其余 cancel 检查 / PauseGate / currentIndex / actionStarted / 失败→Fault 锁存 / 单步挂起一行不改。
// ⚠ 单步挂起**只在一级节点边界**：分组的「一步」= 整个分组，组内一律不调用 WaitForStep
//   （否则 N 个子状态机会争抢同一个 stepGo 原子量，放行顺序不确定，§九 第 5 项）。
bool SequenceWorker::ExecuteOnce()
{
    const auto& nodes = impl_->scheme.nodes;
    for (int i = 0; i < nodes.size(); ++i) {
        if (impl_->cancel.load()) {
            emit interrupted(QStringLiteral("用户停止"));
            return false;
        }
        // 节点边界挂起点（无轴参与 → 只挂起）：单步/无硬件动作的暂停在此生效
        if (!PauseGate()) {
            emit interrupted(QStringLiteral("用户停止"));
            return false;
        }
        const auto& node = nodes[i];
        impl_->currentIndex = node.id;
        impl_->lastError.clear();      // 防上一节点的陈旧原因被重发（D17 方案 A）
        // 信号携带**节点编号**（P6 起不再是扁平行号）；分组与动作共用同一对信号，
        // 接收方按编号在树里定位。
        emit actionStarted(node.id, node.name);
        SPDLOG_INFO("[SequenceWorker] Node #{}: {}", node.id, node.name.toStdString());

        if (!ExecuteNodeWithRetry(node, node.id)) {
            if (impl_->cancel.load()) {
                emit interrupted(QStringLiteral("用户停止"));
            } else {
                // 故障锁存：保留上下文（scheme / currentIndex），待 ClearFault 才回 Idle；
                // 期间 RunSequence 会因 state != Idle 被拒，UI 也能据此禁用「启动」
                SetState(WorkerState::Fault);
                // 【D17 方案 A】原为 emit errorOccurred(action.name)：改用 lastError（具体原因，
                // 含坐标/轴号），为空才回退节点名——顺带修掉"具体原因被 name 覆盖"的既有瑕疵
                emit errorOccurred(impl_->lastError.isEmpty() ? node.name : impl_->lastError);
            }
            return false;
        }

        emit actionFinished(node.id, node.name);

        // 单步模式：每个**一级节点**完成后挂起，等待 NextStep()（分组 = 一步）
        if (impl_->stepMode.load()) {
            impl_->stepGatePending.store(true);
            SetState(WorkerState::Paused, PauseReason::Step);
            SPDLOG_INFO("[SequenceWorker] Step mode: pausing after node #{}", node.id);
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

// ---------------------------------------------------------------------------
// P6（doc/action_tree_concurrency_plan.md）：节点执行与单线程协作式调度
// ---------------------------------------------------------------------------

namespace {

// 一个动作节点的「段」数：Move/VisionGuide 按点位，Extrude 按挤出/回抽两段，Gripper 恒 1 段。
// 段 = 「一次下发 + 一次等待」的最小单元——这正是并发时可以被重叠的部分（§5.1）。
int SegmentCountOf(const ActionData& a)
{
    switch (a.type) {
    case ActionType::Move:        return a.points.size();
    case ActionType::VisionGuide: return 2;          // 目标上方 → 目标
    case ActionType::Gripper:     return 1;
    case ActionType::Extrude: {
        int n = 0;
        if (a.extrudeAmount > 0.0)  ++n;             // 挤出段
        if (a.suckBackAmount > 0.0) ++n;             // 回抽段
        return n;
    }
    default: return 0;                               // Delay / Vision 不走段模型
    }
}

struct ExtrudeSegment { double target; double speed; QString logText; };

// 挤出动作的分段（改造前 ExecuteExtrude 的两段，目标 / 速度 / 日志逐字保留）
QVector<ExtrudeSegment> ExtrudeSegmentsOf(const ActionData& a)
{
    QVector<ExtrudeSegment> segs;
    if (a.extrudeAmount > 0.0)
        segs.push_back({ a.extrudeAmount, a.extrudeSpeed,
                         QStringLiteral("挤出 %1 mm").arg(a.extrudeAmount) });
    if (a.suckBackAmount > 0.0)
        // 回抽目标是**绝对量**（挤出量 − 回抽量）。续走重发它不会把轴反向补回满量程再回抽
        //（否则等于多挤一份料）——语义与改造前 ExecuteExtrude 完全一致。
        segs.push_back({ a.extrudeAmount - a.suckBackAmount, a.suckBackSpeed,
                         QStringLiteral("回抽 %1 mm").arg(a.suckBackAmount) });
    return segs;
}

} // namespace

// 节点级重试包装（P6 由 ExecuteActionWithRetry 改名并上移到节点层）。
// retryCount=0（默认）时**直通** ExecuteNode → 零额外行为，与改造前逐字节一致。
// retryCount=N>0 时：仅 Vision（重新采图定位）与 Move（重新下发目标）可重试；
// Extrude / Gripper / 回抽 / Delay 重复执行有重复出料、重复夹取风险 → 直接失败。
// **分组按 §Q3 重试整个分组**——部分重试会破坏组内资源独占（一个子动作已走完、
// 另一个重试 → 状态错配）。
// 本函数**不 emit errorOccurred**：重试期间只写 logMessage，失败原因留在 lastError，
// 由会话级出口发出（D17 方案 A）。
bool SequenceWorker::ExecuteNodeWithRetry(const ActionData& node, int index)
{
    if (impl_->loopCfg.retryCount <= 0)
        return ExecuteNode(node, index);

    const int maxTry = impl_->loopCfg.retryCount + 1;
    const bool retryable = node.isGroup()
                               || node.type == ActionType::Vision
                               || node.type == ActionType::Move;

    for (int attempt = 1; attempt <= maxTry; ++attempt) {
        impl_->lastError.clear();
        if (ExecuteNode(node, index)) return true;
        if (impl_->cancel.load()) return false;             // 停止/急停：不重试
        if (!retryable || attempt == maxTry) break;         // 不可重试 或 已耗尽 → 上层 Fault
        emit logMessage(QStringLiteral("「%1」失败，%2ms 后重试（%3/%4）")
                            .arg(node.name)
                            .arg(impl_->loopCfg.retryDelayMs)
                            .arg(attempt)
                            .arg(maxTry - 1));
        if (!WaitRetryDelay(impl_->loopCfg.retryDelayMs)) return false;   // 可被暂停/停止打断
    }
    return false;   // 失败原因留在 lastError，由会话级出口单点发出
}

// 节点分派（§九 第 1 项）：按 `nodeType` 分派到动作或分组。
// **本函数及其子树一律不 emit errorOccurred**（D17 方案 A）。
bool SequenceWorker::ExecuteNode(const ActionData& node, int index)
{
    if (node.isGroup())
        return ExecuteGroup(node, index);
    return ExecuteAction(node, index);
}

// 动作节点 = **单元素调度器**。改造前这里是 5 个阻塞式函数的 switch 分派；P6 起动作体
// 本身也是可步进状态机（RunnerState），由同一个调度器驱动——这样「暂停 / 停止 / 急停 /
// 续走」在单动作与分组两条路径上**只有一套实现**。
// **本函数及其子树一律不 emit errorOccurred**（D17 方案 A）。
bool SequenceWorker::ExecuteAction(const ActionData& action, int index)
{
    switch (action.type) {
    case ActionType::Move:
    case ActionType::Vision:
    case ActionType::VisionGuide:
    case ActionType::Extrude:
    case ActionType::Delay:
    case ActionType::Gripper:
        break;
    default:
        SPDLOG_WARN("[SequenceWorker] Unknown action type {}", static_cast<int>(action.type));
        impl_->lastError = QStringLiteral("未知动作类型 (节点 #%1)").arg(index);
        return false;
    }

    std::vector<RunnerState> runners(1);
    runners[0].action = action;
    return RunScheduler(runners);
}

// 分组节点（§九 第 3 项）：为每个子节点建一个子状态机，单线程协作式调度至全部完成。
// 失败归因 = **首个失败者**（其余仅记日志），原因留在 lastError（§九 第 4 项）。
bool SequenceWorker::ExecuteGroup(const ActionData& group, int index)
{
    // 组内校验兜底（§6.2 资源互斥 / §6.4 数据依赖 / §Q5 不嵌套）：UI 已拦，此处再拦一次。
    // 两个绝对目标同时下发会让轴按后到者走 —— 静默出错比拒绝执行危险得多。
    const QString bad = ProcessManager::validateGroupChildren(group.children);
    if (!bad.isEmpty()) {
        SPDLOG_WARN("[SequenceWorker] Group '{}' rejected: {}",
                    group.name.toStdString(), bad.toStdString());
        impl_->lastError = QStringLiteral("并发分组「%1」不可执行：%2").arg(group.name, bad);
        return false;
    }
    for (const auto& child : group.children) {
        if (child.isGroup()) {          // 防御：不嵌套（normalizeActionIds + UI 已拦）
            impl_->lastError = QStringLiteral("并发分组「%1」含嵌套分组「%2」，拒绝执行")
                                   .arg(group.name, child.name);
            return false;
        }
    }

    SPDLOG_INFO("[SequenceWorker] Group #{} '{}' (index {}): {} child node(s)",
                group.id, group.name.toStdString(), index, group.children.size());
    emit groupStarted(group.id, group.name);

    if (group.children.isEmpty()) {
        emit logMessage(QStringLiteral("并发分组「%1」为空，跳过").arg(group.name));
        emit groupFinished(group.id, group.name);
        return true;
    }

    std::vector<RunnerState> runners;
    runners.reserve(static_cast<size_t>(group.children.size()));
    for (const auto& child : group.children) {
        runners.emplace_back();
        runners.back().action = child;
    }

    emit logMessage(QStringLiteral("并发分组「%1」：%2 个子动作同时下发、统一汇合")
                        .arg(group.name).arg(group.children.size()));
    const bool ok = RunScheduler(runners);
    emit groupFinished(group.id, group.name);
    return ok;
}

// 单线程协作式调度器（§5.2）。
// 核心语义（§Q1）：**同时下发、统一汇合**——每个 tick 对所有未完成的子状态机调一次
// StepRunner；先完成「下发」的不会阻塞后完成者，等待阶段自然重叠（省下的正是节拍大头）。
// 暂停 / 停止 / 急停在此统一处理（组内任一子状态机所在轴参与运动 → **整组减速停**），
// 子状态机自身**不处理**这些（§九 第 5/6 项）。
bool SequenceWorker::RunScheduler(std::vector<RunnerState>& runners)
{
    auto& hw = HardwareManager::instance();   // ⚠ 只在 InMainThread 内使用

    // 组内所有子状态机当前等待的轴（去重）——整组减速停的目标集合
    auto collectAxes = [&runners]() {
        QVector<LogicalAxis> all;
        for (const auto& r : runners)
            for (auto a : r.axes)
                if (!all.contains(a)) all.push_back(a);
        return all;
    };
    // §Q2：置 cancel（或内部失败中止）后**等待所有子动作安全停止**再返回。
    // 立即返回会让硬件仍在运动时退出执行循环 —— 失控。
    auto stopAndWaitAll = [&]() {
        const QVector<LogicalAxis> all = collectAxes();
        if (all.isEmpty()) return;
        InMainThread([&] { return hw.StopAxes(all); });
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 3000) {
            const bool busy = InMainThread([&] {
                for (auto a : all) if (hw.IsAxisBusy(a)) return true;
                return false;
            });
            if (!busy) return;
            QThread::msleep(20);
        }
        SPDLOG_WARN("[SequenceWorker] group stop: axes still busy after 3000 ms");
    };

    // 内部中止：某个子状态机失败 → 停全部子动作，但**不置会话级 cancel**
    //（置 cancel 会让上层把「动作失败」误报成「用户停止」）
    bool aborting = false;

    for (;;) {
        // ① 停止 / 急停 / 内部失败中止：先安全停下所有子动作，再返回（§Q2）
        if (impl_->cancel.load() || aborting) {
            stopAndWaitAll();
            return false;
        }

        // ② 暂停：整组减速停 → 汇合后统一挂起 → 恢复后统一重发当前段（§九 第 6 项）
        if (impl_->paused.load()) {
            QElapsedTimer pauseTimer;
            pauseTimer.start();
            const bool resumed = PauseGate(collectAxes(), [&] {
                for (auto& r : runners) {
                    if (r.done() || r.failed) continue;
                    if (r.phase != RunnerState::PhaseWait) continue;   // 只在等待中的段有目标可重发
                    ReissueRunnerSegment(r);
                }
                return true;
            });
            const long long pausedMs = pauseTimer.elapsed();
            for (auto& r : runners) r.pauseAccumMs += pausedMs;   // Delay 按剩余时间续算
            if (!resumed) continue;                               // 暂停中被停止 → 回到 ①
            continue;
        }

        // ③ 推进所有未完成的子状态机（单线程轮询：下发互不阻塞）
        bool allDone = true;
        for (auto& r : runners) {
            if (r.done() || r.failed) continue;
            if (!StepRunner(r)) {
                // 失败归因 = 首个失败者（其余仅记日志，§九 第 4 项）
                if (impl_->lastError.isEmpty())
                    impl_->lastError = r.failReason.isEmpty()
                                           ? QStringLiteral("动作「%1」执行失败").arg(r.action.name)
                                           : r.failReason;
                SPDLOG_WARN("[SequenceWorker] runner failed: node #{} '{}' — {}",
                            r.action.id, r.action.name.toStdString(),
                            impl_->lastError.toStdString());
                aborting = true;
                break;
            }
            if (!r.done()) allDone = false;
        }
        if (aborting) continue;      // 回到 ① 做安全停止（§Q2）
        if (allDone) return true;

        QThread::msleep(20);         // 轮询节拍：与改造前 WaitForAxes 的 20ms 一致
    }
}

// 推进一个子状态机一次（§5.2）。三种形态：
//   · Delay（含"无相机视觉"的模拟延时）—— 纯计时，无轴参与；
//   · Vision —— 同步检测（数十 ms 量级，§5.4 单线程足够），一次 Step 走完；
//   · 其余（Move / VisionGuide / Extrude / Gripper）—— 归约为「段序列」：
//     Begin（解析目标）→ Dispatch（下发）→ Wait（等到位）→ 下一段。
bool SequenceWorker::StepRunner(RunnerState& r)
{
    auto& hw = HardwareManager::instance();   // ⚠ 只在 InMainThread 内使用
    const ActionData& action = r.action;
    r.failReason.clear();

    // ---- Delay：暂停时长不计入延时预算（10s 延时跑到第 8s 被暂停，恢复后只应再等约 2s）----
    if (action.type == ActionType::Delay) {
        if (!r.delayStarted) {
            emit logMessage(QStringLiteral("延时 %1 ms").arg(action.delayMs));
            r.delayTimer.start();
            r.delayStarted = true;
        }
        const long long eff = r.delayTimer.elapsed() - r.pauseAccumMs;
        if (eff >= action.delayMs) { r.phase = RunnerState::PhaseDone; return true; }
        return true;
    }

    // ---- 识别：同步检测 ----
    if (action.type == ActionType::Vision) {
        if (r.phase == RunnerState::PhaseBegin) {
            auto* camera = InMainThread([&] { return hw.camera(); });
            auto* algo   = InMainThread([&] { return hw.algorithm(); });
            if (!camera || !algo) {
                SPDLOG_WARN("[SequenceWorker] Vision: camera/algo not available, simulate");
                emit logMessage(QStringLiteral("视觉：无相机/算法，模拟延时"));
                r.delayTimer.start();
                r.delayStarted = true;
                r.phase = RunnerState::PhaseWait;
                return true;
            }
            emit logMessage(QStringLiteral("视觉：采帧中 (%1)...").arg(action.visionType));
            CameraFrame frame = InMainThread([&] { return camera->CaptureFrame(); });

            emit logMessage(QStringLiteral("视觉：检测中 (阈值 %1)...").arg(action.threshold));
            // P1：`Detect` 改为纯函数式（参数由调用方传入），这里两件事都在**主线程**做——
            // `SnapshotParams()` 读 ConfigManager，只有主线程能读（见本文件 :106 的约定）。
            // S6 决策 6：参数快照单独取一份，供 `detectionReady` 载荷带上阈值（渲染叠加图用）。
            const auto algoParams = InMainThread([&] { return algo->SnapshotParams(); });
            auto results = InMainThread([&] { return algo->Detect(frame, algoParams); });

            // S6 决策 6（2026-09-23）：把「本次用的帧 + 图像空间结果」广播给自动运行页，
            // 使它**不必再订阅原始帧流**（那正是用例 11.26「消费 ≠ 持有」不对称的根源）。
            // **空结果也发**——「跑过检测但没检出目标」同样是一张有意义的画面（无框的原图），
            // 否则自动运行页会一直停在上一轮的旧图上。
            // `frame` 在此之后不再使用，用 move 转移进共享指针，避免 14.5 MB 深拷贝。
            {
                SequenceWorker::DetectionResult dr;
                dr.frame     = std::make_shared<const CameraFrame>(std::move(frame));
                dr.results   = QVector<PuffResult>(results.begin(), results.end());
                dr.threshold = algoParams.confidenceThreshold;
                emit detectionReady(dr);
            }

            if (results.empty()) {
                SPDLOG_WARN("[SequenceWorker] Vision: no target detected");
                r.failReason = QStringLiteral("视觉未检出目标");
                r.failed = true;
                return false;
            }

            const auto& best = results.front();
            Pose robot = impl_->coord.CameraToRobot(best.x, best.y, best.z);
            // P5 §5.4：结果落进会话结果表，供「视觉引导」动作按 sourceActionId 取用。
            // 用 action.id 作 key —— 这正是"按稳定编号引用"而非"按时序邻近"的落点：
            // 中间插入/删除/重排节点都不会让引用错位。
            // ⚠ 注意 PuffResult.yaw **不参与**：yaw 是相机图像平面内的偏航角，本项目无工艺意义（§七）。
            impl_->visionResults_.insert(action.id, robot);
            SPDLOG_INFO("[SequenceWorker] Vision target: cam({:.1f},{:.1f},{:.1f}) conf={:.2f} "
                        "→ robot({:.1f},{:.1f},{:.1f}) [node #{} yaw={:.1f} ignored]",
                        best.x, best.y, best.z, best.confidence, robot.x, robot.y, robot.z,
                        action.id, best.yaw);
            emit visionResultReady(action.id, robot.x, robot.y, robot.z);
            emit logMessage(QStringLiteral("视觉定位：基座 (%1, %2, %3) 置信度 %4")
                                .arg(robot.x, 0, 'f', 1)
                                .arg(robot.y, 0, 'f', 1)
                                .arg(robot.z, 0, 'f', 1)
                                .arg(best.confidence, 0, 'f', 2));
            r.phase = RunnerState::PhaseDone;
            return true;
        }
        // PhaseWait = 无相机时的模拟延时（改造前为 WaitForCancelOrTime(500)）
        if (r.delayTimer.elapsed() - r.pauseAccumMs >= 500) {
            r.phase = RunnerState::PhaseDone;
            return true;
        }
        return true;
    }

    // ---- 段序列（Move / VisionGuide / Extrude / Gripper）----
    for (;;) {
        if (r.phase == RunnerState::PhaseBegin) {
            if (!PrepareRunnerSegment(r)) { r.failed = true; return false; }
            r.phase = RunnerState::PhaseDispatch;
        }
        if (r.phase == RunnerState::PhaseDispatch) {
            if (!DispatchRunnerSegment(r)) {
                r.failReason = QStringLiteral("动作「%1」下发失败").arg(action.name);
                r.failed = true;
                return false;
            }
            r.waitTimer.restart();
            r.phase = RunnerState::PhaseWait;
            return true;            // 已下发：下一 tick 才检查到位（下发互不阻塞的关键）
        }
        // PhaseWait：等本段参与轴到位
        const bool busy = InMainThread([&] {
            for (auto a : r.axes) if (hw.IsAxisBusy(a)) return true;
            return false;
        });
        if (busy) {
            if (r.timeoutMs > 0 && r.waitTimer.elapsed() > r.timeoutMs) {
                r.failReason = QStringLiteral("动作「%1」到位超时").arg(action.name);
                r.failed = true;
                return false;
            }
            return true;
        }
        // 本段完成 → 下一段（或整节点完成）
        if (!AdvanceRunnerSegment(r)) { r.phase = RunnerState::PhaseDone; return true; }
        r.phase = RunnerState::PhaseBegin;
    }
}

// Begin：解析当前段的目标（IK / 软限位 / 日志）。失败返回 false 并写 r.failReason。
bool SequenceWorker::PrepareRunnerSegment(RunnerState& r)
{
    auto& hw = HardwareManager::instance();   // ⚠ 只在 InMainThread 内使用
    const ActionData& action = r.action;

    switch (action.type) {
    case ActionType::Move: {
        if (r.segIdx >= action.points.size()) return true;      // 无点位 → 直接收尾
        const PointData& pt = action.points[r.segIdx];
        emit logMessage(QStringLiteral("移动 → 点 %1 (%2)").arg(r.segIdx + 1).arg(pt.name));
        if (!ResolveRunnerJoints(r, pt)) return false;
        r.speedScale = qBound(0.01, action.speedPercent / 100.0, 1.0);
        r.axes = { LogicalAxis::J1, LogicalAxis::J2, LogicalAxis::Z, LogicalAxis::R };
        r.timeoutMs = 30000;
        return true;
    }
    case ActionType::VisionGuide: {
        // ① 取识别结果（§5.3 步骤 1）。取不到 → 报错且**不产生任何运动**（§4.3 执行期校验）。
        if (!r.baseResolved) {
            r.baseResolved = true;
            if (action.sourceActionId <= 0) {
                r.failReason = QStringLiteral(
                    "视觉引导：未指定识别动作编号，请先编辑该动作选择引用的识别动作");
                return false;
            }
            const auto it = impl_->visionResults_.constFind(action.sourceActionId);
            if (it == impl_->visionResults_.constEnd()) {
                r.failReason = QStringLiteral("视觉引导：未找到识别动作 #%1 的结果"
                                              "（该识别动作本次会话未执行，或未检出目标）")
                                   .arg(action.sourceActionId);
                return false;
            }
            r.baseX = it.value().x;
            r.baseY = it.value().y;
            r.baseZ = it.value().z;
            r.baseOk = true;
            SPDLOG_INFO("[SequenceWorker] VisionGuide #{} ← Vision #{}: base({:.1f},{:.1f},{:.1f}) "
                        "offsetZ={:.1f} r={:.1f}",
                        action.id, action.sourceActionId, r.baseX, r.baseY, r.baseZ,
                        action.offsetZ, action.guideR);
            emit logMessage(QStringLiteral("视觉引导：引用 #%1 → 目标上方 (%2, %3, %4) → 目标 (%5, %6, %7)")
                                .arg(action.sourceActionId)
                                .arg(r.baseX, 0, 'f', 1).arg(r.baseY, 0, 'f', 1)
                                .arg(r.baseZ + action.offsetZ, 0, 'f', 1)
                                .arg(r.baseX, 0, 'f', 1).arg(r.baseY, 0, 'f', 1)
                                .arg(r.baseZ, 0, 'f', 1));
        }
        if (!r.baseOk) {
            r.failReason = QStringLiteral("视觉引导：识别结果不可用");
            return false;
        }

        // ② 两组坐标（§5.2）：r 一律取 guideR（轴 4 固定值），**不是** PuffResult.yaw。
        // offsetZ 按**末端高度 Pose.z** 算、正值向上（决策 3），与 PointData.j3（Z 电机坐标）无关。
        // hasJoints 保持 false：坐标由识别结果决定，没有示教关节角可继承 → 走 IK。
        PointData pt;
        pt.name = (r.segIdx == 0) ? QStringLiteral("目标上方") : QStringLiteral("目标");
        pt.x = r.baseX;
        pt.y = r.baseY;
        pt.z = (r.segIdx == 0) ? (r.baseZ + action.offsetZ) : r.baseZ;
        pt.r = action.guideR;

        if (!ResolveRunnerJoints(r, pt)) return false;
        r.speedScale = qBound(0.01, action.speedPercent / 100.0, 1.0);
        r.axes = { LogicalAxis::J1, LogicalAxis::J2, LogicalAxis::Z, LogicalAxis::R };
        r.timeoutMs = 30000;
        return true;
    }
    case ActionType::Extrude: {
        const QVector<ExtrudeSegment> segs = ExtrudeSegmentsOf(action);
        if (r.segIdx >= segs.size()) return true;
        r.segTarget = segs[r.segIdx].target;
        r.segSpeed  = segs[r.segIdx].speed;
        emit logMessage(segs[r.segIdx].logText);
        r.axes = { LogicalAxis::Extruder };
        r.timeoutMs = 10000;
        return true;
    }
    case ActionType::Gripper: {
        // 行程 = 轴5 绝对目标坐标（mm），语义与手动控制页轴5 Go 一致：0 = 夹紧、负值 = 松开
        const double lo = InMainThread([&] { return hw.GetLimitMin(LogicalAxis::Gripper); });
        const double hi = InMainThread([&] { return hw.GetLimitMax(LogicalAxis::Gripper); });
        const double target = action.gripperTarget;
        // 软限位硬性拦截（与手动页 Go / MoveAbs 内部同源）：越界拒绝，绝不静默改写目标
        if (!InMainThread([&] { return hw.IsWithinSoftLimits(LogicalAxis::Gripper, target); })) {
            SPDLOG_WARN("[SequenceWorker] Gripper target {:.2f} out of soft limits [{:.2f}, {:.2f}]",
                        target, lo, hi);
            r.failReason = QStringLiteral("夹爪目标行程 %1 mm 超出轴5软限位（%2 ~ %3 mm）")
                               .arg(QString::number(target, 'f', 2),
                                    QString::number(lo, 'f', 2), QString::number(hi, 'f', 2));
            return false;
        }
        r.speedScale = qBound(0.01, action.speedPercent / 100.0, 1.0);
        r.segTarget = target;
        r.segSpeed  = InMainThread([&] { return hw.GetMaxSpeed(LogicalAxis::Gripper); }) * r.speedScale;
        emit logMessage(QStringLiteral("夹爪%1 → 目标 %2 mm（速度 %3 mm/s）")
                            .arg(action.isGripperOpen ? QStringLiteral("张开") : QStringLiteral("闭合"),
                                 QString::number(target, 'f', 2),
                                 QString::number(r.segSpeed, 'f', 2)));
        r.axes = { LogicalAxis::Gripper };
        r.timeoutMs = 0;    // 下发后按 MarkAxisBusy 估算的耗时动态兜底（见 DispatchRunnerSegment）
        return true;
    }
    default:
        return true;
    }
}

// 把点位解析为关节目标：示教 joints 优先（FK 一致性校验 ±0.5mm / ±0.5°，不匹配即回退 IK）。
// 逻辑与改造前 MoveToPoint 的前半段逐行一致，只是结果落到 RunnerState。
bool SequenceWorker::ResolveRunnerJoints(RunnerState& r, const PointData& pt)
{
    auto& hw = HardwareManager::instance();   // ⚠ 只在 InMainThread 内使用

    const double curJ2 = InMainThread([&] { return hw.GetPosition(LogicalAxis::J2); });

    Pose target{ pt.x, pt.y, pt.z, pt.r };
    // R 目标 ±180 wrap 归一化（必须在 IK 之前）：旧方案/边界示教可能存 wrap 值（-179.9 ≡ 180.1），
    // Kinematics::ValidateJoints 按 [rMin,rMax] 拒绝 wrap 值会报"目标点不可达"
    target.r = InMainThread([&] { return hw.NormalizeRotationAngle(LogicalAxis::R, target.r); });

    bool usedTeach = false;
    if (pt.hasJoints) {
        // 示教关节角路径：j1/j2=逻辑关节角、j3=Z 电机坐标（MoveAbs 同坐标系）、j4=R 角，
        // 直接下发，确定性复现示教姿态（无 IK 双解歧义）
        double j1 = pt.j1, j2 = pt.j2, jz = pt.j3, jr = pt.j4;
        jr = InMainThread([&] { return hw.NormalizeRotationAngle(LogicalAxis::R, jr); });
        // FK 一致性校验（±0.5mm / ±0.5°）：joints 与 coord 不匹配（json 手改未走 UI 作废、
        // 旧版硬编码全 0 占位残留等）时回退 IK，绝不按错误关节角运动。
        const Pose fk = impl_->kin.Forward(Joints{ j1, j2, jz, jr });
        double dr = std::fabs(fk.r - pt.r);
        while (dr > 180.0) dr = std::fabs(dr - 360.0);   // ±180 回绕感知的角度差
        if (std::fabs(fk.x - pt.x) <= 0.5 && std::fabs(fk.y - pt.y) <= 0.5 &&
            std::fabs(fk.z - pt.z) <= 0.5 && dr <= 0.5) {
            r.j1 = j1; r.j2 = j2; r.jz = jz; r.jr = jr;
            usedTeach = true;
        } else {
            SPDLOG_WARN("[SequenceWorker] joints/coord mismatch at '{}' "
                        "(FK {:.2f},{:.2f},{:.2f} vs coord {:.2f},{:.2f},{:.2f}), fallback to IK",
                        pt.name.toStdString(), fk.x, fk.y, fk.z, pt.x, pt.y, pt.z);
        }
    }
    if (!usedTeach) {
        Joints joints;
        if (!impl_->kin.InverseSmart(target, joints, curJ2)) {
            SPDLOG_WARN("[SequenceWorker] IK failed for point ({:.1f}, {:.1f}, {:.1f}) r={:.1f}",
                        pt.x, pt.y, pt.z, pt.r);
            r.failReason = QStringLiteral("目标点不可达：(%1, %2, %3)").arg(pt.x).arg(pt.y).arg(pt.z);
            return false;
        }
        r.j1 = joints.j1; r.j2 = joints.j2; r.jz = joints.z; r.jr = joints.r;
        SPDLOG_INFO("[SequenceWorker] IK → J({:.2f}, {:.2f}, {:.2f}, {:.2f})",
                    r.j1, r.j2, r.jz, r.jr);
    } else {
        SPDLOG_INFO("[SequenceWorker] Teach joints → J({:.2f}, {:.2f}, {:.2f}, {:.2f})",
                    r.j1, r.j2, r.jz, r.jr);
    }
    return true;
}

// Dispatch：下发当前段。**非阻塞**——MoveAbs 立即返回，到位靠 IsAxisBusy 轮询
//（这正是并发能重叠等待的前提：下发不占时间，等待才占）。
bool SequenceWorker::DispatchRunnerSegment(RunnerState& r)
{
    auto& hw = HardwareManager::instance();   // ⚠ 只在 InMainThread 内使用

    switch (r.action.type) {
    case ActionType::Move:
    case ActionType::VisionGuide:
        // 分步移动：先舵机（J2/R）再卡轴（J1/Z），避免机械干涉。
        // 已在位轴（差 ≤ 0.01）跳过下发 —— 见 DispatchPointMove 注释（S2c / D19 加固）。
        return InMainThread([&] {
            return DispatchPointMove(Joints{ r.j1, r.j2, r.jz, r.jr },
                                     hw.GetMaxSpeed(LogicalAxis::J1) * r.speedScale,
                                     hw.GetMaxSpeed(LogicalAxis::J2) * r.speedScale,
                                     hw.GetMaxSpeed(LogicalAxis::Z)  * r.speedScale,
                                     hw.GetMaxSpeed(LogicalAxis::R)  * r.speedScale);
        });
    case ActionType::Extrude:
        return InMainThread([&] {
            return hw.MoveAbs(LogicalAxis::Extruder, r.segTarget, r.segSpeed);
        });
    case ActionType::Gripper: {
        if (!InMainThread([&] {
                return hw.MoveAbs(LogicalAxis::Gripper, r.segTarget, r.segSpeed);
            }))
            return false;
        // 到位等待：按 MoveAbs 里 MarkAxisBusy 估算的耗时动态兜底——
        // 低速（如 10% ≈ 0.2mm/s 走 3mm 需 15s）时固定 10s 会误判超时；下限 3000 防 est=0 无限等待
        const int estMs = InMainThread([&] { return hw.GetAxisBusyMs(LogicalAxis::Gripper); });
        r.timeoutMs = qMax(3000, static_cast<int>(estMs * 1.2) + 3000);
        return true;
    }
    default:
        return true;
    }
}

// 段完成 → segIdx+1；返回 true = 还有下一段。
bool SequenceWorker::AdvanceRunnerSegment(RunnerState& r)
{
    ++r.segIdx;
    r.axes.clear();     // 防上一段的轴集合被下一段误用（segIdx 越界时也靠它立即"不忙"）
    return r.segIdx < SegmentCountOf(r.action);
}

// 恢复续走：重发**当前段**的绝对目标。**须在 InMainThread 内调用。**
void SequenceWorker::ReissueRunnerSegment(RunnerState& r)
{
    auto& hw = HardwareManager::instance();
    switch (r.action.type) {
    case ActionType::Move:
    case ActionType::VisionGuide:
        // 与首次下发同规则（含在位轴跳过）：恢复瞬间不把已在位轴重下发
        DispatchPointMove(Joints{ r.j1, r.j2, r.jz, r.jr },
                          hw.GetMaxSpeed(LogicalAxis::J1) * r.speedScale,
                          hw.GetMaxSpeed(LogicalAxis::J2) * r.speedScale,
                          hw.GetMaxSpeed(LogicalAxis::Z)  * r.speedScale,
                          hw.GetMaxSpeed(LogicalAxis::R)  * r.speedScale);
        break;
    case ActionType::Extrude:
        hw.MoveAbs(LogicalAxis::Extruder, r.segTarget, r.segSpeed);
        break;
    case ActionType::Gripper:
        if (hw.MoveAbs(LogicalAxis::Gripper, r.segTarget, r.segSpeed)) {
            const int estMs = hw.GetAxisBusyMs(LogicalAxis::Gripper);
            r.timeoutMs = qMax(3000, static_cast<int>(estMs * 1.2) + 3000);
        }
        break;
    default:
        break;
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
