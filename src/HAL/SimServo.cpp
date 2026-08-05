#include "SimServo.h"

#include <spdlog/spdlog.h>

// 在全局作用域自动注册到工厂
REGISTER_AXIS_SERVO("SimServo", SimServo)

SimServo::SimServo() = default;

SimServo::~SimServo()
{
    Disconnect();
}

bool SimServo::Connect(const std::string& port, int baudRate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = true;
    spdlog::info("[SimServo] Connected (simulated) — port={}, baud={}", port, baudRate);
    return true;
}

void SimServo::Disconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    torque_    = false;
    spdlog::info("[SimServo] Disconnected");
}

bool SimServo::IsConnected() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

bool SimServo::SetServoId(uint8_t id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    servoId_ = id;
    spdlog::info("[SimServo] Servo id set to {}", id);
    return true;
}

uint8_t SimServo::GetServoId() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return servoId_;
}

bool SimServo::TorqueOn()
{
    std::lock_guard<std::mutex> lock(mutex_);
    torque_ = true;
    spdlog::info("[SimServo] Torque ON (id={})", servoId_);
    return true;
}

bool SimServo::TorqueOff()
{
    std::lock_guard<std::mutex> lock(mutex_);
    torque_ = false;
    spdlog::info("[SimServo] Torque OFF (id={})", servoId_);
    return true;
}

bool SimServo::MoveToAngle(double angleDeg, int timeMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    double old = angle_;
    angle_ = angleDeg;
    spdlog::info("[SimServo] (id={}) MoveToAngle: {:.1f}° → {:.1f}° in {}ms",
                 servoId_, old, angle_, timeMs);
    return true;
}

bool SimServo::MoveAtSpeed(double angleDeg, double speedDps)
{
    std::lock_guard<std::mutex> lock(mutex_);
    double old = angle_;
    angle_ = angleDeg;
    if (speedDps > 0) speed_ = speedDps;
    spdlog::info("[SimServo] (id={}) MoveAtSpeed: {:.1f}° → {:.1f}° @ {:.1f}°/s",
                 servoId_, old, angle_, speed_);
    return true;
}

bool SimServo::Stop()
{
    spdlog::info("[SimServo] (id={}) Stop", servoId_);
    return true;
}

bool SimServo::SetSpeed(double speedDps)
{
    std::lock_guard<std::mutex> lock(mutex_);
    speed_ = speedDps;
    spdlog::info("[SimServo] (id={}) speed set to {:.1f}°/s", servoId_, speed_);
    return true;
}

double SimServo::ReadAngle()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return angle_;
}

ServoTelemetry SimServo::ReadTelemetry()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ServoTelemetry t;
    t.voltage     = 7.4f;
    t.temperature = 35;
    t.current     = 0;
    t.rawPosition = static_cast<int32_t>(angle_ * 10.0);
    t.angleDeg    = angle_;
    t.online      = connected_;
    return t;
}

bool SimServo::IsOnline() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

bool SimServo::ClearAlarm()
{
    std::lock_guard<std::mutex> lock(mutex_);
    alarm_ = false;
    spdlog::info("[SimServo] (id={}) alarm cleared", servoId_);
    return true;
}

std::string SimServo::GetLastError() const
{
    return lastError_;
}
