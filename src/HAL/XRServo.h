#pragma once

#include "IAxisServo.h"
#include "HALFactory.h"

#include <memory>
#include <string>

// ============================================================
// XR 串口总线舵机 (XRServo) — IAxisServo 实现
// 协议参考 bopai\puff\src\core\XRServo.*
// 修正点：舵机 ID 由 HardwareManager 从 config 读取并经
// SetServoId 喂入，不再硬编码。
// ============================================================
class XRServo : public IAxisServo
{
public:
    XRServo();
    ~XRServo() override;

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

    bool SetSpeed(double speedDps) override;

    double ReadAngle() override;
    ServoTelemetry ReadTelemetry() override;

    bool IsOnline() const override;
    bool ClearAlarm() override;

    std::string GetLastError() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
