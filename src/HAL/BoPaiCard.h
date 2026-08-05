#pragma once

#include "IMotionCard.h"
#include "HALFactory.h"

#include <unordered_map>
#include <memory>
#include <mutex>
#include <string>

// ============================================================
// 博派运动控制卡 (Bopai) — IMotionCard 实现
// 协议见 3rdparty/bopai/include/MultiCardCPP.h (MultiCard 类)
// 轴号按 0 基内部使用，调用 SDK 时 +1（卡上轴号 1 基）。
// 所有位置/速度为脉冲单位（与 IMotionCard 契约一致），
// 换算参数由 HardwareManager 经 SetAxisConfig 喂入，不自读配置。
// ============================================================
class BoPaiCard : public IMotionCard
{
public:
    BoPaiCard();
    ~BoPaiCard() override;

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

private:
    void RefreshStatus();
    double PulsePerUnit(int axisId) const;
    AxisConfig* Cfg(int axisId);
    const AxisConfig* Cfg(int axisId) const;

    class Impl;
    std::unique_ptr<Impl> impl_;

    mutable std::mutex mutex_;
    std::unordered_map<int, AxisConfig> configs_;
    std::unordered_map<int, MotorStatus> lastStatus_;
};
