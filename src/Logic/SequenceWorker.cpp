#include "SequenceWorker.h"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QMetaObject>

#include <atomic>
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

} // namespace

class SequenceWorker::Impl
{
public:
    Kinematics     kin;
    CoordTransform coord;

    std::atomic<bool> running    {false};
    std::atomic<bool> cancel     {false};
    std::atomic<bool> stepMode   {false};
    std::atomic<bool> stepGo     {false};

    SchemeData scheme;
    int        currentIndex = -1;

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

bool SequenceWorker::RunSequence(const SchemeData& scheme)
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
    impl_->scheme = scheme;
    impl_->currentIndex = -1;
    impl_->running.store(true);

    emit stateChanged(QStringLiteral("运行中"));
    SPDLOG_INFO("[SequenceWorker] RunSequence: {} ({} actions)",
                scheme.schemeName.toStdString(), static_cast<int>(scheme.actions.size()));

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
    impl_->scheme = scheme;
    impl_->currentIndex = actionIndex;
    impl_->stepMode.store(false);   // 单动作执行与单步会话互斥（UI 侧另清 m_stepActive）
    impl_->running.store(true);

    emit stateChanged(QStringLiteral("执行选中动作"));
    SPDLOG_INFO("[SequenceWorker] RunSingleAction: [{}] {}", actionIndex,
                scheme.actions[actionIndex].name.toStdString());

    // 排队到 worker 线程执行（同 RunSequence 模式）
    QMetaObject::invokeMethod(this, "StartSingleExecution",
                              Qt::QueuedConnection, Q_ARG(int, actionIndex));
    return true;
}

void SequenceWorker::Stop()
{
    if (!impl_->running.load()) return;
    impl_->cancel.store(true);
    SPDLOG_INFO("[SequenceWorker] Stop requested");
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

bool SequenceWorker::IsRunning() const   { return impl_->running.load(); }
bool SequenceWorker::IsPaused() const    { return impl_->stepMode.load() && impl_->running.load(); }
bool SequenceWorker::IsStepMode() const  { return impl_->stepMode.load(); }

void SequenceWorker::StartExecution()
{
    if (ExecuteActions()) {
        SPDLOG_INFO("[SequenceWorker] Scheme finished: {}", impl_->scheme.schemeName.toStdString());
        emit schemeFinished();
    }
    impl_->running.store(false);
    emit stateChanged(QStringLiteral("空闲"));
}

void SequenceWorker::StartSingleExecution(int index)
{
    // 防御：调用链（RunSingleAction 已校验）保证合法，此处兜底防越界 UB
    if (index < 0 || index >= static_cast<int>(impl_->scheme.actions.size())) {
        SPDLOG_WARN("[SequenceWorker] StartSingleExecution: index out of range {}", index);
        impl_->running.store(false);
        emit stateChanged(QStringLiteral("空闲"));
        return;
    }
    const auto& action = impl_->scheme.actions[index];
    emit actionStarted(index, action.name);
    SPDLOG_INFO("[SequenceWorker] Single action {}: {}", index, action.name.toStdString());

    bool ok = ExecuteAction(action, index);
    if (!ok) {
        // 注意：ExecuteAction 内部多数失败路径（IK 失败/视觉未检出/各超时）已发 errorOccurred，
        // 此处再发一次为兜底语义（覆盖 MoveAbs 静默失败路径），与 ExecuteActions 行为一致。
        if (impl_->cancel.load())
            emit interrupted(QStringLiteral("用户停止"));
        else
            emit errorOccurred(action.name);
    } else {
        emit actionFinished(index, action.name);
    }
    emit singleActionFinished(index);
    impl_->running.store(false);
    emit stateChanged(QStringLiteral("空闲"));
}

bool SequenceWorker::ExecuteActions()
{
    const auto& actions = impl_->scheme.actions;
    for (int i = 0; i < actions.size(); ++i) {
        if (impl_->cancel.load()) {
            emit interrupted(QStringLiteral("用户停止"));
            return false;
        }
        impl_->currentIndex = i;
        const auto& action = actions[i];
        emit actionStarted(i, action.name);
        SPDLOG_INFO("[SequenceWorker] Action {}: {}", i, action.name.toStdString());

        if (!ExecuteAction(action, i)) {
            if (impl_->cancel.load())
                emit interrupted(QStringLiteral("用户停止"));
            else
                emit errorOccurred(action.name);
            return false;
        }

        emit actionFinished(i, action.name);

        // 单步模式：每个动作完成后挂起，等待 NextStep()
        if (impl_->stepMode.load()) {
            emit stateChanged(QStringLiteral("单步暂停"));
            SPDLOG_INFO("[SequenceWorker] Step mode: pausing after action {}", i);
            if (!impl_->WaitForStep()) {
                emit interrupted(QStringLiteral("用户停止"));
                return false;
            }
            emit stateChanged(QStringLiteral("运行中"));
        }
    }
    return true;
}

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
        emit errorOccurred(QStringLiteral("未知动作类型 (index %1)").arg(index));
        return false;
    }
}

bool SequenceWorker::ExecuteMove(const ActionData& action)
{
    const double speedScale = qBound(0.01, action.speedPercent / 100.0, 1.0);
    for (int p = 0; p < action.points.size(); ++p) {
        if (impl_->cancel.load()) return false;
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
    Joints joints;
    if (!impl_->kin.InverseSmart(target, joints, curJ2)) {
        SPDLOG_WARN("[SequenceWorker] IK failed for point ({:.1f}, {:.1f}, {:.1f}) r={:.1f}",
                    pt.x, pt.y, pt.z, pt.r);
        emit errorOccurred(QStringLiteral("目标点不可达：(%1, %2, %3)").arg(pt.x).arg(pt.y).arg(pt.z));
        return false;
    }

    SPDLOG_INFO("[SequenceWorker] IK → J({:.2f}, {:.2f}, {:.2f}, {:.2f})",
                joints.j1, joints.j2, joints.z, joints.r);

    // 各轴速度：maxSpeed × speedPercent
    const double v1 = InMainThread([&] { return hw.GetMaxSpeed(LogicalAxis::J1); }) * speedScale;
    const double v2 = InMainThread([&] { return hw.GetMaxSpeed(LogicalAxis::J2); }) * speedScale;
    const double vz = InMainThread([&] { return hw.GetMaxSpeed(LogicalAxis::Z); }) * speedScale;
    const double vr = InMainThread([&] { return hw.GetMaxSpeed(LogicalAxis::R); }) * speedScale;

    // 分步移动：先舵机（J2/R）再卡轴（J1/Z），避免机械干涉
    if (!InMainThread([&] { return hw.MoveAbs(LogicalAxis::J2, joints.j2, v2); })) return false;
    if (!InMainThread([&] { return hw.MoveAbs(LogicalAxis::R,  joints.r, vr); })) return false;
    if (!InMainThread([&] { return hw.MoveAbs(LogicalAxis::J1, joints.j1, v1); })) return false;
    if (!InMainThread([&] { return hw.MoveAbs(LogicalAxis::Z,  joints.z, vz); })) return false;

    // 等待全部轴到位（IsAxisBusy 时间戳 + 轮询；超时 30s 兜底）
    QVector<LogicalAxis> axes{ LogicalAxis::J1, LogicalAxis::J2, LogicalAxis::Z, LogicalAxis::R };
    if (!WaitForAxes(axes, 30000)) {
        if (impl_->cancel.load()) return false;
        SPDLOG_WARN("[SequenceWorker] WaitForAxes timeout at point ({:.1f}, {:.1f}, {:.1f})",
                    pt.x, pt.y, pt.z);
        emit errorOccurred(QStringLiteral("移动到位超时"));
        return false;
    }
    return true;
}

bool SequenceWorker::WaitForAxes(const QVector<LogicalAxis>& axes, int timeoutMs)
{
    auto& hw = HardwareManager::instance();
    QElapsedTimer t;
    t.start();
    for (;;) {
        if (impl_->cancel.load()) return false;
        bool allDone = true;
        for (auto a : axes) {
            if (InMainThread([&] { return hw.IsAxisBusy(a); })) { allDone = false; break; }
        }
        if (allDone) return true;
        if (timeoutMs > 0 && t.elapsed() > timeoutMs) return false;
        QThread::msleep(20);
    }
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
        emit errorOccurred(QStringLiteral("视觉未检出目标"));
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
        if (!WaitForAxes(axes, 10000)) {
            if (impl_->cancel.load()) return false;
            emit errorOccurred(QStringLiteral("挤出到位超时"));
            return false;
        }
    }
    if (action.suckBackAmount > 0.0) {
        emit logMessage(QStringLiteral("回抽 %1 mm").arg(action.suckBackAmount));
        const double target = action.extrudeAmount - action.suckBackAmount;
        if (!InMainThread([&] { return hw.MoveAbs(LogicalAxis::Extruder, target, action.suckBackSpeed); }))
            return false;
        QVector<LogicalAxis> axes{ LogicalAxis::Extruder };
        if (!WaitForAxes(axes, 10000)) {
            if (impl_->cancel.load()) return false;
            emit errorOccurred(QStringLiteral("回抽到位超时"));
            return false;
        }
    }
    return true;
}

bool SequenceWorker::ExecuteDelay(const ActionData& action)
{
    emit logMessage(QStringLiteral("延时 %1 ms").arg(action.delayMs));
    return impl_->WaitForCancelOrTime(action.delayMs);
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
        emit errorOccurred(QStringLiteral("夹爪目标行程 %1 mm 超出轴5软限位（%2 ~ %3 mm）")
                               .arg(QString::number(target, 'f', 2),
                                    QString::number(lo, 'f', 2), QString::number(hi, 'f', 2)));
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
    if (!WaitForAxes(axes, qMax(3000, static_cast<int>(estMs * 1.2) + 3000))) {
        if (impl_->cancel.load()) return false;
        emit errorOccurred(QStringLiteral("夹爪到位超时"));
        return false;
    }
    return true;
}