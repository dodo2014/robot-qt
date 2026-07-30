#include "SimCard.h"

#include <spdlog/spdlog.h>
#include <cmath>
#include <sstream>

// 在全局作用域自动注册到工厂
REGISTER_MOTION_CARD("SimCard", SimCard)

SimCard::SimCard()
{
    for (int i = 0; i < 4; ++i)
    {
        auto& axis = GetOrCreateAxis(i);
        axis.position  = 0.0;
        axis.velocity  = 0.0;
        axis.speed     = 100.0;
        axis.accel     = 200.0;
        axis.decel     = 200.0;
        axis.enabled   = false;
        axis.alarm     = false;
        axis.homeDone  = false;
    }
    spdlog::info("[SimCard] Initialized with 4 virtual axes");
}

SimCard::~SimCard()
{
    Disconnect();
}

bool SimCard::Connect(const std::string& ip, int port)
{
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = true;
    spdlog::info("[SimCard] Connected (simulated) — ip={}, port={}", ip, port);
    return true;
}

void SimCard::Disconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    spdlog::info("[SimCard] Disconnected");
}

bool SimCard::IsConnected() const
{
    return connected_;
}

bool SimCard::EnableAxis(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    axis.enabled = true;
    spdlog::info("[SimCard] Axis {} enabled", axisId);
    return true;
}

bool SimCard::DisableAxis(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    axis.enabled = false;
    spdlog::info("[SimCard] Axis {} disabled", axisId);
    return true;
}

bool SimCard::HomeAxis(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    axis.position = 0.0;
    axis.homeDone = true;
    spdlog::info("[SimCard] Axis {} homed → position=0.0", axisId);
    return true;
}

bool SimCard::StopAxis(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    axis.velocity = 0.0;
    spdlog::info("[SimCard] Axis {} stopped", axisId);
    return true;
}

bool SimCard::StopAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, axis] : axes_)
    {
        axis.velocity = 0.0;
    }
    spdlog::info("[SimCard] All axes stopped");
    return true;
}

bool SimCard::EmergencyStop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, axis] : axes_)
    {
        axis.velocity = 0.0;
    }
    spdlog::critical("[SimCard] EMERGENCY STOP — all axes halted immediately");
    return true;
}

bool SimCard::MoveAbs(int axisId, double position, double speed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    double oldPos = axis.position;
    axis.position = position;
    axis.velocity = (speed > 0) ? speed : axis.speed;
    spdlog::info("[SimCard] Axis {} MoveAbs: {:.2f} → {:.2f} (speed={:.1f})",
                 axisId, oldPos, position, axis.velocity);
    return true;
}

bool SimCard::MoveRel(int axisId, double distance, double speed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    double oldPos = axis.position;
    axis.position += distance;
    axis.velocity = (speed > 0) ? speed : axis.speed;
    spdlog::info("[SimCard] Axis {} MoveRel: {:.2f} → {:.2f} (dist={:.2f}, speed={:.1f})",
                 axisId, oldPos, axis.position, distance, axis.velocity);
    return true;
}

bool SimCard::MoveLinear(const std::vector<double>& targetPositions, double speed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;
    for (size_t i = 0; i < targetPositions.size(); ++i)
    {
        auto& axis = GetOrCreateAxis(static_cast<int>(i));
        double oldPos = axis.position;
        axis.position = targetPositions[i];
        axis.velocity = (speed > 0) ? speed : axis.speed;
        if (i > 0) oss << ", ";
        oss << "Axis" << i << ": " << oldPos << " → " << targetPositions[i];
    }
    spdlog::info("[SimCard] MoveLinear — {}", oss.str());
    return true;
}

bool SimCard::SetSpeed(int axisId, double speed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    axis.speed = speed;
    spdlog::info("[SimCard] Axis {} speed set to {:.1f}", axisId, speed);
    return true;
}

bool SimCard::SetAccel(int axisId, double accel, double decel)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    axis.accel = accel;
    axis.decel = (decel > 0) ? decel : accel;
    spdlog::info("[SimCard] Axis {} accel={:.1f} decel={:.1f}", axisId, axis.accel, axis.decel);
    return true;
}

double SimCard::GetPosition(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return GetOrCreateAxis(axisId).position;
}

MotorStatus SimCard::GetAxisStatus(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    MotorStatus status;
    status.axisId   = axisId;
    status.position = axis.position;
    status.velocity = axis.velocity;
    status.current  = 0.0;
    status.enabled  = axis.enabled;
    status.alarm    = axis.alarm;
    status.homeDone = axis.homeDone;
    return status;
}

std::vector<MotorStatus> SimCard::GetAllStatus()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MotorStatus> statuses;
    for (const auto& [id, axis] : axes_)
    {
        MotorStatus s;
        s.axisId   = id;
        s.position = axis.position;
        s.velocity = axis.velocity;
        s.current  = 0.0;
        s.enabled  = axis.enabled;
        s.alarm    = axis.alarm;
        s.homeDone = axis.homeDone;
        statuses.push_back(s);
    }
    return statuses;
}

bool SimCard::SetDO(int channel, bool state)
{
    std::lock_guard<std::mutex> lock(mutex_);
    doStates_[channel] = state;
    spdlog::info("[SimCard] DO[{}] = {}", channel, state ? "ON" : "OFF");
    return true;
}

bool SimCard::GetDI(int channel)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = diStates_.find(channel);
    return it != diStates_.end() ? it->second : false;
}

std::string SimCard::GetLastError() const
{
    return lastError_;
}

SimCard::SimAxis& SimCard::GetOrCreateAxis(int axisId)
{
    auto it = axes_.find(axisId);
    if (it == axes_.end())
    {
        SimAxis axis;
        axis.position = 0.0;
        axis.homeDone = false;
        axes_[axisId] = axis;
        return axes_[axisId];
    }
    return it->second;
}
