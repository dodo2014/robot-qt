#include "PickCycleController.h"

#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <cmath>

#include "HAL/core/HardwareManager.h"
#include "HAL/core/AxisMap.h"

class PickCycleController::Impl
{
public:
    PickCycleState  state       = PickCycleState::Idle;
    std::atomic<bool> running   {false};
    std::atomic<bool> paused    {false};

    Pose pickPosition;
    Pose placePosition;
    double safeHeight = 100.0;

    StateCallback callback;

    Kinematics      kinematics;
    CoordTransform  coordTransform;

    void SetState(PickCycleState newState, const std::string& msg)
    {
        state = newState;
        SPDLOG_INFO("[PickCycle] State → {}: {}", GetStateName(newState), msg);
        if (callback)
            callback(newState, msg);
    }

    static std::string GetStateName(PickCycleState s)
    {
        switch (s)
        {
            case PickCycleState::Idle:       return "Idle";
            case PickCycleState::Capturing:  return "Capturing";
            case PickCycleState::Detecting:  return "Detecting";
            case PickCycleState::Approaching: return "Approaching";
            case PickCycleState::Gripping:   return "Gripping";
            case PickCycleState::Lifting:    return "Lifting";
            case PickCycleState::Placing:    return "Placing";
            case PickCycleState::Releasing:  return "Releasing";
            case PickCycleState::Completed:  return "Completed";
            case PickCycleState::Error:      return "Error";
        }
        return "Unknown";
    }

    // 硬件访问：全部走 HardwareManager 门面（物理单位 mm/度，内部含 Home Offset
    // 与方向反转换算），不直接持有 IMotionCard/IAxisServo 指针。
    // 注：底层同步阻塞调用（含舵机串口事务），UI 线程调用会短暂卡顿，后续可移入
    // 独立线程（与 SequenceWorker 同模式）。
    bool MoveJointsSequential(const Joints& target, double speed)
    {
        auto& hw = HardwareManager::instance();

        // Step 1: 读取当前关节（逻辑坐标，门面已换算）
        Joints current;
        current.j1 = hw.GetPosition(LogicalAxis::J1);
        current.j2 = hw.GetPosition(LogicalAxis::J2);
        current.z  = hw.GetPosition(LogicalAxis::Z);
        current.r  = hw.GetPosition(LogicalAxis::R);

        SPDLOG_INFO("[PickCycle] Moving from J({:.1f}, {:.1f}, {:.1f}, {:.1f}) → "
                     "J({:.1f}, {:.1f}, {:.1f}, {:.1f})",
                     current.j1, current.j2, current.z, current.r,
                     target.j1, target.j2, target.z, target.r);

        // Step 2: 舵机先动（J2 小臂 → R 翻转），再卡轴（J1 大臂 → Z 升降），避免机械干涉
        if (!hw.MoveAbs(LogicalAxis::J2, target.j2, speed)) {
            SPDLOG_ERROR("[PickCycle] Servo J2 move failed");
            return false;
        }
        if (!hw.MoveAbs(LogicalAxis::R, target.r, speed)) {
            SPDLOG_ERROR("[PickCycle] Servo R move failed");
            return false;
        }
        if (!hw.MoveAbs(LogicalAxis::J1, target.j1, speed)) {
            SPDLOG_ERROR("[PickCycle] J1 move failed");
            return false;
        }
        if (!hw.MoveAbs(LogicalAxis::Z, target.z, speed)) {
            SPDLOG_ERROR("[PickCycle] Z move failed");
            return false;
        }

        // Step 3: 等待全部轴到位（IsAxisBusy 时间戳轮询，30s 兜底）
        const int timeoutMs = 30000;
        auto waitStart = std::chrono::steady_clock::now();
        for (;;) {
            if (!running.load() || paused.load()) return false;
            bool done = true;
            for (LogicalAxis a : { LogicalAxis::J1, LogicalAxis::J2, LogicalAxis::Z, LogicalAxis::R }) {
                if (hw.IsAxisBusy(a)) { done = false; break; }
            }
            if (done) break;
            if (std::chrono::steady_clock::now() - waitStart > std::chrono::milliseconds(timeoutMs)) {
                SPDLOG_ERROR("[PickCycle] Move wait timeout");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        return true;
    }

    bool ExecuteOneCycle()
    {
        auto& hw = HardwareManager::instance();

        // 使能门禁：自动循环与手动界面共用同一安全门禁（先使能再运动）
        if (!hw.IsGlobalEnabled()) {
            SetState(PickCycleState::Error, "Axes not enabled, enable first");
            return false;
        }

        SetState(PickCycleState::Capturing, "Capturing camera frame...");
        if (!hw.camera() || !hw.algorithm()) {
            SetState(PickCycleState::Error, "Camera or algorithm not available");
            return false;
        }
        CameraFrame frame = hw.camera()->CaptureFrame();

        SetState(PickCycleState::Detecting, "Detecting puff position...");
        PuffResult puff = hw.algorithm()->LocateBest(frame);
        if (puff.confidence < 0.5)
        {
            SetState(PickCycleState::Error, "Low confidence detection");
            return false;
        }
        SPDLOG_INFO("[PickCycle] Puff detected at ({:.1f}, {:.1f}, {:.1f}) confidence={:.2f}",
                     puff.x, puff.y, puff.z, puff.confidence);

        // 计算抓取位姿
        Joints currentJoints;
        currentJoints.j1 = hw.GetPosition(LogicalAxis::J1);
        currentJoints.j2 = hw.GetPosition(LogicalAxis::J2);
        currentJoints.z  = hw.GetPosition(LogicalAxis::Z);
        currentJoints.r  = hw.GetPosition(LogicalAxis::R);

        Pose pickPose;
        pickPose.x = puff.x;
        pickPose.y = puff.y;
        pickPose.z = puff.z;
        pickPose.r = puff.yaw;

        Joints pickJoints;
        if (!kinematics.InverseSmart(pickPose, pickJoints, currentJoints.j2))
        {
            SetState(PickCycleState::Error, "IK no solution for pick position");
            return false;
        }

        // 上方安全点 (Z 提升)
        Joints approachJoints = pickJoints;
        approachJoints.z = pickJoints.z + safeHeight;

        SetState(PickCycleState::Approaching, "Moving above puff...");
        if (!MoveJointsSequential(approachJoints, 50.0))
        {
            SetState(PickCycleState::Error, "Approach move failed");
            return false;
        }

        SetState(PickCycleState::Approaching, "Descending to puff...");
        if (!MoveJointsSequential(pickJoints, 20.0))
        {
            SetState(PickCycleState::Error, "Descend move failed");
            return false;
        }

        SetState(PickCycleState::Gripping, "Closing gripper...");
        if (!hw.gripper()) {
            SetState(PickCycleState::Error, "Gripper not available");
            return false;
        }
        if (!hw.gripper()->Close())
        {
            SetState(PickCycleState::Error, "Gripper close failed");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        SetState(PickCycleState::Lifting, "Lifting puff...");
        Joints liftJoints = pickJoints;
        liftJoints.z = pickJoints.z + safeHeight;
        if (!MoveJointsSequential(liftJoints, 30.0))
        {
            SetState(PickCycleState::Error, "Lift move failed");
            return false;
        }

        SetState(PickCycleState::Placing, "Moving to place position...");
        Joints placeJoints;
        if (!kinematics.InverseSmart(placePosition, placeJoints, liftJoints.j2))
        {
            SetState(PickCycleState::Error, "IK no solution for place position");
            return false;
        }
        if (!MoveJointsSequential(placeJoints, 50.0))
        {
            SetState(PickCycleState::Error, "Place move failed");
            return false;
        }

        SetState(PickCycleState::Releasing, "Opening gripper...");
        if (!hw.gripper()->Open())
        {
            SetState(PickCycleState::Error, "Gripper open failed");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        SetState(PickCycleState::Completed, "Cycle complete");
        return true;
    }
};

PickCycleController::PickCycleController()
    : impl_(std::make_unique<Impl>())
{
    SPDLOG_INFO("[PickCycleController] Created");
}

PickCycleController::~PickCycleController() = default;

bool PickCycleController::StartCycle()
{
    // 使能门禁：自动循环与手动界面共用同一安全门禁（先使能再运动）
    if (!HardwareManager::instance().IsGlobalEnabled()) {
        SPDLOG_WARN("[PickCycleController] Start cycle rejected: axes not enabled");
        impl_->SetState(PickCycleState::Error, "Axes not enabled, enable first");
        return false;
    }
    impl_->running = true;
    SPDLOG_INFO("[PickCycleController] Start cycle");
    return impl_->ExecuteOneCycle();
}

bool PickCycleController::StopCycle()
{
    impl_->running = false;
    impl_->paused  = false;
    impl_->SetState(PickCycleState::Idle, "Stopped by user");
    SPDLOG_INFO("[PickCycleController] Stopped");
    return true;
}

bool PickCycleController::PauseCycle()
{
    impl_->paused = true;
    SPDLOG_INFO("[PickCycleController] Paused");
    return true;
}

bool PickCycleController::ResumeCycle()
{
    impl_->paused = false;
    SPDLOG_INFO("[PickCycleController] Resumed");
    return true;
}

PickCycleState PickCycleController::GetState() const
{
    return impl_->state;
}

std::string PickCycleController::GetStateName() const
{
    return Impl::GetStateName(impl_->state);
}

bool PickCycleController::SetPickPosition(const Pose& pos)
{
    impl_->pickPosition = pos;
    SPDLOG_INFO("[PickCycleController] Pick position set: ({:.1f}, {:.1f}, {:.1f})",
                pos.x, pos.y, pos.z);
    return true;
}

bool PickCycleController::SetPlacePosition(const Pose& pos)
{
    impl_->placePosition = pos;
    SPDLOG_INFO("[PickCycleController] Place position set: ({:.1f}, {:.1f}, {:.1f})",
                pos.x, pos.y, pos.z);
    return true;
}

bool PickCycleController::SetSafeHeight(double heightMm)
{
    impl_->safeHeight = heightMm;
    SPDLOG_INFO("[PickCycleController] Safe height set: {:.1f}mm", heightMm);
    return true;
}

void PickCycleController::SetStateCallback(StateCallback cb)
{
    impl_->callback = std::move(cb);
}

bool PickCycleController::ExecuteOneShot()
{
    // 使能门禁：自动循环与手动界面共用同一安全门禁（先使能再运动）
    if (!HardwareManager::instance().IsGlobalEnabled()) {
        SPDLOG_WARN("[PickCycleController] ExecuteOneShot rejected: axes not enabled");
        impl_->SetState(PickCycleState::Error, "Axes not enabled, enable first");
        return false;
    }
    return impl_->ExecuteOneCycle();
}