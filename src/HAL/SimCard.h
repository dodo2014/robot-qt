#pragma once

#include "IMotionCard.h"
#include "HALFactory.h"

#include <unordered_map>
#include <mutex>

class SimCard : public IMotionCard
{
public:
    SimCard();
    ~SimCard() override;

    bool Connect(const std::string& ip, int port) override;
    void Disconnect() override;
    bool IsConnected() const override;

    bool EnableAxis(int axisId) override;
    bool DisableAxis(int axisId) override;
    bool HomeAxis(int axisId) override;
    bool StopAxis(int axisId) override;
    bool StopAll() override;
    bool EmergencyStop() override;

    bool MoveAbs(int axisId, double position, double speed = -1.0) override;
    bool MoveRel(int axisId, double distance, double speed = -1.0) override;
    bool MoveLinear(const std::vector<double>& targetPositions, double speed = -1.0) override;

    bool SetSpeed(int axisId, double speed) override;
    bool SetAccel(int axisId, double accel, double decel = -1.0) override;

    double GetPosition(int axisId) override;
    MotorStatus GetAxisStatus(int axisId) override;
    std::vector<MotorStatus> GetAllStatus() override;

    bool SetDO(int channel, bool state) override;
    bool GetDI(int channel) override;

    std::string GetLastError() const override;

private:
    bool connected_ = false;
    std::string lastError_;
    std::mutex mutex_;

    struct SimAxis
    {
        double position    = 0.0;
        double velocity    = 0.0;
        double targetPos   = 0.0;
        double speed       = 100.0;
        double accel       = 200.0;
        double decel       = 200.0;
        bool   enabled     = false;
        bool   alarm       = false;
        bool   homeDone    = false;
    };

    std::unordered_map<int, SimAxis> axes_;
    std::unordered_map<int, bool>    diStates_;
    std::unordered_map<int, bool>    doStates_;

    SimAxis& GetOrCreateAxis(int axisId);
};
