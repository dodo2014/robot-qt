#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MotorStatus
{
    int     axisId    = 0;
    double  position  = 0.0;
    double  velocity  = 0.0;
    double  current   = 0.0;
    bool    enabled   = false;
    bool    alarm     = false;
    bool    homeDone  = false;
};

struct AxisConfig
{
    int     axisId        = 0;
    double  maxSpeed      = 100.0;
    double  maxAccel      = 200.0;
    double  maxDecel      = 200.0;
    double  homePos       = 0.0;
    double  limitMin      = -180.0;
    double  limitMax      = 180.0;
    bool    inverted      = false;
    double  pulsesPerUnit = 100.0;
};

class IMotionCard
{
public:
    virtual ~IMotionCard() = default;

    virtual bool Connect(const std::string& ip, int port) = 0;
    virtual void Disconnect() = 0;
    virtual bool IsConnected() const = 0;

    virtual bool EnableAxis(int axisId) = 0;
    virtual bool DisableAxis(int axisId) = 0;
    virtual bool HomeAxis(int axisId) = 0;
    virtual bool StopAxis(int axisId) = 0;
    virtual bool StopAll() = 0;
    virtual bool EmergencyStop() = 0;

    virtual bool MoveAbs(int axisId, double position, double speed = -1.0) = 0;
    virtual bool MoveRel(int axisId, double distance, double speed = -1.0) = 0;
    virtual bool MoveLinear(const std::vector<double>& targetPositions, double speed = -1.0) = 0;

    virtual bool SetSpeed(int axisId, double speed) = 0;
    virtual bool SetAccel(int axisId, double accel, double decel = -1.0) = 0;

    virtual double GetPosition(int axisId) = 0;
    virtual MotorStatus GetAxisStatus(int axisId) = 0;
    virtual std::vector<MotorStatus> GetAllStatus() = 0;

    virtual bool SetDO(int channel, bool state) = 0;
    virtual bool GetDI(int channel) = 0;

    virtual std::string GetLastError() const = 0;
};
