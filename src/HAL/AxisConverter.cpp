#include "AxisConverter.h"

#include <cmath>
#include <spdlog/spdlog.h>

AxisConverter& AxisConverter::Instance()
{
    static AxisConverter inst;
    return inst;
}

void AxisConverter::ConfigureAxis(int axisId, const AxisConfig& cfg)
{
    configs_[axisId] = cfg;
}

const AxisConfig* AxisConverter::Find(int axisId) const
{
    auto it = configs_.find(axisId);
    return (it != configs_.end()) ? &it->second : nullptr;
}

// 每脉冲对应的物理量
//   - hardwareType==0 (运动控制卡/角度轴): 度/脉冲 = 360 / (pulsesPerRev * microSteps)
//   - hardwareType==1 (直线轴或舵机):      mm/脉冲 = lead / (pulsesPerRev * microSteps)
double AxisConverter::PhysicalPerPulse(int axisId) const
{
    const AxisConfig* cfg = Find(axisId);
    if (!cfg) {
        SPDLOG_INFO("[AxisConverter] axis {} not configured, using default 1.0", axisId);
        return 1.0;
    }
    double pulsesPerRev = (cfg->pulsesPerRev > 0) ? cfg->pulsesPerRev : 1.0;
    double steps = pulsesPerRev * std::max(1, cfg->microSteps);
    double denom = (cfg->hardwareType == 0) ? 360.0 : (cfg->lead > 0 ? cfg->lead : 1.0);
    return denom / steps;
}

long AxisConverter::ToPulse(int axisId, double mmOrDeg) const
{
    double perPulse = PhysicalPerPulse(axisId);
    return static_cast<long>(std::llround(mmOrDeg / perPulse));
}

double AxisConverter::ToPhysical(int axisId, long pulse) const
{
    double perPulse = PhysicalPerPulse(axisId);
    return pulse * perPulse;
}

double AxisConverter::SpeedToPulse(int axisId, double mmOrDegPerSec) const
{
    double perPulse = PhysicalPerPulse(axisId);
    return (perPulse > 0) ? (mmOrDegPerSec / perPulse) : 0.0;
}

double AxisConverter::PulseToSpeed(int axisId, double pulsePerSec) const
{
    double perPulse = PhysicalPerPulse(axisId);
    return pulsePerSec * perPulse;
}
