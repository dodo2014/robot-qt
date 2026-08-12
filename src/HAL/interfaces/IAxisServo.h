#pragma once

#include <cstdint>
#include <string>

struct ServoTelemetry
{
    float   voltage      = 0.0f;
    int16_t temperature  = 0;
    int16_t current      = 0;
    int32_t rawPosition  = 0;
    double  angleDeg     = 0.0;
    bool    online       = false;
};

enum class ServoMoveMode
{
    TimeBased,   // 指定到达时间 (ms)
    SpeedBased   // 指定运行速度 (度/s)
};

class IAxisServo
{
public:
    virtual ~IAxisServo() = default;

    virtual bool Connect(const std::string& port, int baudRate) = 0;
    virtual void Disconnect() = 0;
    virtual bool IsConnected() const = 0;

    virtual bool SetServoId(uint8_t id) = 0;
    virtual uint8_t GetServoId() const = 0;

    virtual bool TorqueOn() = 0;
    virtual bool TorqueOff() = 0;

    virtual bool MoveToAngle(double angleDeg, int timeMs) = 0;
    virtual bool MoveAtSpeed(double angleDeg, double speedDps) = 0;
    virtual bool Stop() = 0;

    // 最近一次运动指令的估计到达时间(ms)，0 表示立即/未知。用于"到位后恢复按钮"等。
    virtual int GetLastMoveTimeMs() const = 0;

    virtual bool SetSpeed(double speedDps) = 0;

    virtual double ReadAngle() = 0;
    virtual ServoTelemetry ReadTelemetry() = 0;

    virtual bool IsOnline() const = 0;
    virtual bool ClearAlarm() = 0;

    virtual std::string GetLastError() const = 0;
};
