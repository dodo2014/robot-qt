#include "PickCycleController.h"

#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <cmath>

#include "HAL/core/HardwareManager.h"

class PickCycleController::Impl
{
public:
    IMotionCard*    motionCard  = nullptr;
    IAxisServo*     servoJ2     = nullptr;
    IAxisServo*     servoJ3     = nullptr;
    IEndEffector*   gripper     = nullptr;
    ICamera*        camera      = nullptr;
    IPuffAlgorithm* algorithm   = nullptr;

    PickCycleState  state       = PickCycleState::Idle;
    std::atomic<bool> running   {false};
    std::atomic<bool> paused    {false};

    Pose3D pickPosition;
    Pose3D placePosition;
    double safeHeight = 100.0;

    StateCallback callback;

    Kinematics      kinematics;
    CoordTransform  coordTransform;

    bool ValidateHardware()
    {
        if (!motionCard || !servoJ2 || !servoJ3 || !gripper || !camera || !algorithm)
        {
            SPDLOG_ERROR("[PickCycle] Hardware not fully set");
            return false;
        }
        return true;
    }

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

    // SCARA 分步运动：J2/J3 舵机先动 → 轮询到位 → J1 再动
    bool MoveJointsSequential(const JointAngles& target, double speed)
    {
        // Step 1: 读取当前关节
        JointAngles current;
        current.angles[0] = motionCard->GetPosition(0);
        current.angles[1] = servoJ2->ReadAngle();
        current.angles[2] = motionCard->GetPosition(2);
        current.angles[3] = motionCard->GetPosition(3);

        SPDLOG_INFO("[PickCycle] Moving from J({:.1f}, {:.1f}, {:.1f}, {:.1f}) → "
                     "J({:.1f}, {:.1f}, {:.1f}, {:.1f})",
                     current.angles[0], current.angles[1],
                     current.angles[2], current.angles[3],
                     target.angles[0], target.angles[1],
                     target.angles[2], target.angles[3]);

        // Step 2: J2 舵机先动 (J2 是大臂, 先调整姿态)
        int moveTimeMs = static_cast<int>(std::abs(target.angles[1] - current.angles[1]) / speed * 1000.0);
        if (moveTimeMs < 100) moveTimeMs = 100;
        if (moveTimeMs > 3000) moveTimeMs = 3000;

        if (!servoJ2->MoveToAngle(target.angles[1], moveTimeMs))
        {
            SPDLOG_ERROR("[PickCycle] Servo J2 move failed");
            return false;
        }

        // Step 3: J3 舵机同时动
        if (!servoJ3->MoveToAngle(target.angles[2], moveTimeMs))
        {
            SPDLOG_ERROR("[PickCycle] Servo J3 move failed");
            return false;
        }

        // Step 4: 等待 J2/J3 到位
        std::this_thread::sleep_for(std::chrono::milliseconds(moveTimeMs + 50));

        // Step 5: J1 (伺服电机) 运动
        if (!motionCard->MoveAbs(0, target.angles[0], speed))
        {
            SPDLOG_ERROR("[PickCycle] Motion card J1 move failed");
            return false;
        }

        // Step 6: J3 (线性轴)
        if (!motionCard->MoveAbs(2, target.angles[2], speed))
        {
            SPDLOG_ERROR("[PickCycle] Motion card J3 (linear) move failed");
            return false;
        }

        return true;
    }

    bool ExecuteOneCycle()
    {
        if (!ValidateHardware())
        {
            SetState(PickCycleState::Error, "Hardware not ready");
            return false;
        }

        SetState(PickCycleState::Capturing, "Capturing camera frame...");
        CameraFrame frame = camera->CaptureFrame();

        SetState(PickCycleState::Detecting, "Detecting puff position...");
        PuffResult puff = algorithm->LocateBest(frame);
        if (puff.confidence < 0.5)
        {
            SetState(PickCycleState::Error, "Low confidence detection");
            return false;
        }
        SPDLOG_INFO("[PickCycle] Puff detected at ({:.1f}, {:.1f}, {:.1f}) confidence={:.2f}",
                     puff.x, puff.y, puff.z, puff.confidence);

        // 计算抓取位姿
        JointAngles currentJoints;
        currentJoints.angles[0] = motionCard->GetPosition(0);
        currentJoints.angles[1] = servoJ2->ReadAngle();
        currentJoints.angles[2] = motionCard->GetPosition(2);
        currentJoints.angles[3] = motionCard->GetPosition(3);

        Pose3D pickPose;
        pickPose.x = puff.x;
        pickPose.y = puff.y;
        pickPose.z = puff.z;
        pickPose.yaw = puff.yaw;

        JointAngles pickJoints;
        if (!kinematics.Inverse(pickPose, pickJoints, currentJoints))
        {
            SetState(PickCycleState::Error, "IK no solution for pick position");
            return false;
        }

        // 上方安全点 (Z 提升)
        JointAngles approachJoints = pickJoints;
        approachJoints.angles[2] = pickJoints.angles[2] + safeHeight;

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
        if (!gripper->Close())
        {
            SetState(PickCycleState::Error, "Gripper close failed");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        SetState(PickCycleState::Lifting, "Lifting puff...");
        JointAngles liftJoints = pickJoints;
        liftJoints.angles[2] = pickJoints.angles[2] + safeHeight;
        if (!MoveJointsSequential(liftJoints, 30.0))
        {
            SetState(PickCycleState::Error, "Lift move failed");
            return false;
        }

        SetState(PickCycleState::Placing, "Moving to place position...");
        JointAngles placeJoints;
        if (!kinematics.Inverse(placePosition, placeJoints, liftJoints))
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
        if (!gripper->Open())
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

void PickCycleController::SetHardware(IMotionCard* motion, IAxisServo* j2, IAxisServo* j3,
                                       IEndEffector* gripper, ICamera* camera, IPuffAlgorithm* algo)
{
    impl_->motionCard = motion;
    impl_->servoJ2    = j2;
    impl_->servoJ3    = j3;
    impl_->gripper    = gripper;
    impl_->camera     = camera;
    impl_->algorithm  = algo;
    SPDLOG_INFO("[PickCycleController] Hardware set");
}

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

bool PickCycleController::SetPickPosition(const Pose3D& pos)
{
    impl_->pickPosition = pos;
    SPDLOG_INFO("[PickCycleController] Pick position set: ({:.1f}, {:.1f}, {:.1f})",
                 pos.x, pos.y, pos.z);
    return true;
}

bool PickCycleController::SetPlacePosition(const Pose3D& pos)
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
