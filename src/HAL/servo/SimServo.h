#pragma once

#include "IAxisServo.h"
#include "HALFactory.h"

#include <mutex>

// 串口舵机 (IAxisServo) 的仿真实现 — 纯内存角度模拟，不接触真实硬件
class SimServo : public IAxisServo
{
public:
    SimServo();
    ~SimServo() override;

    bool Connect(const std::string& port, int baudRate) override;
    void Disconnect() override;
    bool IsConnected() const override;

    bool SetServoId(uint8_t id) override;
    uint8_t GetServoId() const override;

    bool TorqueOn() override;
    bool TorqueOff() override;

    bool MoveToAngle(double angleDeg, int timeMs) override;
    bool MoveAtSpeed(double angleDeg, double speedDps) override;
    bool Stop() override;

    int GetLastMoveTimeMs() const override;

    bool SetSpeed(double speedDps) override;

    double ReadAngle() override;
    ServoTelemetry ReadTelemetry() override;

    bool IsOnline() const override;
    bool ClearAlarm() override;

    std::string GetLastError() const override;

private:
    bool connected_ = false;
    bool torque_    = false;
    bool alarm_     = false;
    uint8_t servoId_ = 1;
    double angle_    = 90.0;
    double speed_    = 50.0;
    std::string lastError_;
    mutable std::mutex mutex_;
};
