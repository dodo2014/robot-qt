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

    bool MoveJog(int axisId, double speedPulsesPerSec, double accel = -1.0, int direction = 1) override;
    bool StopJog(int axisId) override;

    bool SetSpeed(int axisId, double speed) override;
    bool SetAccel(int axisId, double accel, double decel = -1.0) override;
    bool SetAxisConfig(int axisId, const AxisConfig& cfg) override;

    double GetPosition(int axisId) override;
    MotorStatus GetAxisStatus(int axisId) override;
    std::vector<MotorStatus> GetAllStatus() override;

    bool SetDO(int channel, bool state) override;
    bool GetDI(int channel) override;

    std::string GetLastError() const override;

    // 仿真专用：按 dt 秒积分点动运动（由 HardwareManager 轮询时调用）
    void Step(double dtSeconds);

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
        bool   running     = false;
        bool   limitPositive = false;
        bool   limitNegative = false;
        double jogSpeed    = 0.0;
        int    jogDir      = 1;
        AxisConfig cfg;
        // 软限位换算成脉冲域（SetAxisConfig 计算，避免把脉冲位置和物理单位限位直接比较）
        double limitMinPulse = -1.0e30;
        double limitMaxPulse =  1.0e30;
    };

    std::unordered_map<int, SimAxis> axes_;
    std::unordered_map<int, bool>    diStates_;
    std::unordered_map<int, bool>    doStates_;

    SimAxis& GetOrCreateAxis(int axisId);
};
