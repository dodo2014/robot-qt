#pragma once

#include <functional>
#include <memory>

#include "Core/Kinematics.h"
#include "Core/CoordTransform.h"
#include "HAL/interfaces/IMotionCard.h"
#include "HAL/interfaces/IAxisServo.h"
#include "HAL/interfaces/IEndEffector.h"
#include "HAL/interfaces/ICamera.h"
#include "HAL/interfaces/IPuffAlgorithm.h"

enum class PickCycleState
{
    Idle,
    Capturing,
    Detecting,
    Approaching,
    Gripping,
    Lifting,
    Placing,
    Releasing,
    Completed,
    Error
};

class PickCycleController
{
public:
    PickCycleController();
    ~PickCycleController();

    void SetHardware(IMotionCard* motion, IAxisServo* j2, IAxisServo* j3,
                     IEndEffector* gripper, ICamera* camera, IPuffAlgorithm* algo);

    bool StartCycle();
    bool StopCycle();
    bool PauseCycle();
    bool ResumeCycle();

    PickCycleState GetState() const;
    std::string GetStateName() const;

    bool SetPickPosition(const Pose3D& pos);
    bool SetPlacePosition(const Pose3D& pos);
    bool SetSafeHeight(double heightMm);

    using StateCallback = std::function<void(PickCycleState, const std::string&)>;
    void SetStateCallback(StateCallback cb);

    bool ExecuteOneShot();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
