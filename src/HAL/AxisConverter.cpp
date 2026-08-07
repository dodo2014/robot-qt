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
//   - axisType==0 (旋转轴): 度/脉冲 = 360 / (pulsesPerRev * microSteps)
//   - axisType==1 (直线轴):  mm/脉冲 = (lead × gearRatio) / (pulsesPerRev * microSteps)
double AxisConverter::PhysicalPerPulse(int axisId) const
{
    const AxisConfig* cfg = Find(axisId);
    if (!cfg) {
        SPDLOG_INFO("[AxisConverter] axis {} not configured, using default 1.0", axisId);
        return 1.0;
    }
    double pulsesPerRev = (cfg->pulsesPerRev > 0) ? cfg->pulsesPerRev : 1.0;
    double steps = pulsesPerRev * std::max(1, cfg->microSteps);
    double denom;
    if (cfg->axisType == 1) {
        double mmPerRev = cfg->lead * cfg->gearRatio;
        denom = (mmPerRev > 0) ? mmPerRev : 1.0;
    } else {
        denom = 360.0;
    }
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
