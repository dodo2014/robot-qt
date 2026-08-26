#pragma once

#include "IAxisServo.h"
#include "HALFactory.h"

#include <memory>
#include <string>

// ============================================================
// FashionStar 总线伺服舵机 (XRServo) — IAxisServo 实现
// 协议：Fashionrobo 总线舵机串口协议（帧头 0x4C12/0x1C05），
// 参考同事工程 D:\workspace\projects\ServoTest\FashionStar_UartServoProtocol.*
// 注意：真机为 FashionStar 舵机，非 bopai\puff 移植的 0xF9/0xFF 协议
// （曾用错协议导致点动无动作、角度读取错误）。
// 舵机 ID 由 HardwareManager 从 config 读取并经 SetServoId 喂入。
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

    int GetLastMoveTimeMs() const override;

    bool SetSpeed(double speedDps) override;

    double ReadAngle() override;
    ServoTelemetry ReadTelemetry() override;

    bool IsOnline() const override;
    bool ClearAlarm() override;
    bool Ping() override;

    std::string GetLastError() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
