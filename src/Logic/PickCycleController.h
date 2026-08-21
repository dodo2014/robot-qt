#pragma once

#include <functional>
#include <memory>
#include <string>

#include "Core/Kinematics.h"
#include "Core/CoordTransform.h"

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

    bool StartCycle();
    bool StopCycle();
    bool PauseCycle();
    bool ResumeCycle();

    PickCycleState GetState() const;
    std::string GetStateName() const;

    bool SetPickPosition(const Pose& pos);
    bool SetPlacePosition(const Pose& pos);
    bool SetSafeHeight(double heightMm);

    using StateCallback = std::function<void(PickCycleState, const std::string&)>;
    void SetStateCallback(StateCallback cb);

    bool ExecuteOneShot();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
