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
    SPDLOG_INFO("[SimCard] Initialized with 4 virtual axes");
}

SimCard::~SimCard()
{
    Disconnect();
}

bool SimCard::Connect(const std::string& ip, int port)
{
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = true;
    SPDLOG_INFO("[SimCard] Connected (simulated) — ip={}, port={}", ip, port);
    return true;
}

void SimCard::Disconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    SPDLOG_INFO("[SimCard] Disconnected");
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
    SPDLOG_INFO("[SimCard] Axis {} enabled", axisId);
    return true;
}

bool SimCard::DisableAxis(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    axis.enabled = false;
    SPDLOG_INFO("[SimCard] Axis {} disabled", axisId);
    return true;
}

bool SimCard::HomeAxis(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    axis.position = 0.0;
    axis.homeDone = true;
    SPDLOG_INFO("[SimCard] Axis {} homed → position=0.0", axisId);
    return true;
}

bool SimCard::StopAxis(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    axis.velocity = 0.0;
    axis.running  = false;
    SPDLOG_INFO("[SimCard] Axis {} stopped", axisId);
    return true;
}

bool SimCard::StopAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, axis] : axes_)
    {
        axis.velocity = 0.0;
    }
    SPDLOG_INFO("[SimCard] All axes stopped");
    return true;
}

bool SimCard::EmergencyStop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, axis] : axes_)
    {
        axis.velocity = 0.0;
    }
    SPDLOG_CRITICAL("[SimCard] EMERGENCY STOP — all axes halted immediately");
    return true;
}

bool SimCard::MoveAbs(int axisId, double position, double speed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    double oldPos = axis.position;
    axis.position = position;
    // 仿真中 MoveAbs 瞬时完成，不保持运行状态
    axis.velocity = 0.0;
    axis.running  = false;
    SPDLOG_INFO("[SimCard] Axis {} MoveAbs: {:.2f} → {:.2f} (speed={:.1f})",
                 axisId, oldPos, position, speed);
    return true;
}

bool SimCard::MoveRel(int axisId, double distance, double speed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    double oldPos = axis.position;
    axis.position += distance;
    axis.velocity = 0.0;
    axis.running  = false;
    SPDLOG_INFO("[SimCard] Axis {} MoveRel: {:.2f} → {:.2f} (dist={:.2f}, speed={:.1f})",
                 axisId, oldPos, axis.position, distance, speed);
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
    SPDLOG_INFO("[SimCard] MoveLinear — {}", oss.str());
    return true;
}

bool SimCard::MoveJog(int axisId, double speedPulsesPerSec, double accel, int direction)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    axis.jogSpeed = speedPulsesPerSec;
    axis.jogDir   = direction;
    axis.velocity = speedPulsesPerSec * direction;
    axis.running  = true;
    SPDLOG_INFO("[SimCard] Axis {} MoveJog: speed={:.1f} pps, dir={:+d}",
                 axisId, speedPulsesPerSec, direction);
    return true;
}

bool SimCard::StopJog(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    axis.jogSpeed = 0.0;
    axis.velocity = 0.0;
    axis.running  = false;
    SPDLOG_INFO("[SimCard] Axis {} StopJog", axisId);
    return true;
}

void SimCard::Step(double dtSeconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, axis] : axes_)
    {
        if (axis.running && axis.jogSpeed != 0.0)
            axis.position += axis.velocity * dtSeconds;
        // 软限位夹紧（limitMinPulse/MaxPulse 已在 SetAxisConfig 换算为脉冲域）
        if (axis.limitMinPulse < axis.limitMaxPulse) {
            if (axis.position > axis.limitMaxPulse) axis.position = axis.limitMaxPulse;
            if (axis.position < axis.limitMinPulse) axis.position = axis.limitMinPulse;
        }
    }
}

bool SimCard::SetAxisConfig(int axisId, const AxisConfig& cfg)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    axis.cfg = cfg;
    axis.speed = cfg.maxSpeed;
    axis.accel = cfg.maxAccel;
    axis.decel = cfg.maxDecel;
    // 把物理单位软限位换算成脉冲域（与 BoPaiCard::PulsePerUnit 同一公式）
    double pulsesPerRev = (cfg.pulsesPerRev > 0) ? cfg.pulsesPerRev : 1.0;
    double steps = pulsesPerRev * (cfg.microSteps > 1 ? cfg.microSteps : 1);
    double denom = (cfg.hardwareType == 0) ? 360.0 : (cfg.lead > 0 ? cfg.lead : 1.0);
    double ppu = steps / denom;
    if (cfg.limitMin < cfg.limitMax) {
        axis.limitMinPulse = cfg.limitMin * ppu;
        axis.limitMaxPulse = cfg.limitMax * ppu;
    } else {
        axis.limitMinPulse = -1.0e30;
        axis.limitMaxPulse =  1.0e30;
    }
    SPDLOG_INFO("[SimCard] Axis {} config set: ppr={}, microSteps={}, lead={}, type={}",
                 axisId, cfg.pulsesPerRev, cfg.microSteps, cfg.lead, cfg.hardwareType);
    return true;
}

bool SimCard::SetSpeed(int axisId, double speed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    axis.speed = speed;
    SPDLOG_INFO("[SimCard] Axis {} speed set to {:.1f}", axisId, speed);
    return true;
}

bool SimCard::SetAccel(int axisId, double accel, double decel)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto& axis = GetOrCreateAxis(axisId);
    axis.accel = accel;
    axis.decel = (decel > 0) ? decel : accel;
    SPDLOG_INFO("[SimCard] Axis {} accel={:.1f} decel={:.1f}", axisId, axis.accel, axis.decel);
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
    status.axisId        = axisId;
    status.position      = axis.position;
    status.velocity      = axis.velocity;
    status.current       = 0.0;
    status.enabled       = axis.enabled;
    status.alarm         = axis.alarm;
    status.homeDone      = axis.homeDone;
    status.running       = axis.running;
    status.limitPositive = axis.limitPositive;
    status.limitNegative = axis.limitNegative;
    return status;
}

std::vector<MotorStatus> SimCard::GetAllStatus()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MotorStatus> statuses;
    for (const auto& [id, axis] : axes_)
    {
        MotorStatus s;
        s.axisId        = id;
        s.position      = axis.position;
        s.velocity      = axis.velocity;
        s.current       = 0.0;
        s.enabled       = axis.enabled;
        s.alarm         = axis.alarm;
        s.homeDone      = axis.homeDone;
        s.running       = axis.running;
        s.limitPositive = axis.limitPositive;
        s.limitNegative = axis.limitNegative;
        statuses.push_back(s);
    }
    return statuses;
}

bool SimCard::SetDO(int channel, bool state)
{
    std::lock_guard<std::mutex> lock(mutex_);
    doStates_[channel] = state;
    SPDLOG_INFO("[SimCard] DO[{}] = {}", channel, state ? "ON" : "OFF");
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
