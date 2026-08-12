#pragma once

#include "IMotionCard.h"

#include <unordered_map>

// ============================================================
// 单位换算拦截层 (Unit Conversion)
// 界面(UI) 传入/传出的全是物理单位 (mm / 度)，
// 而底层控制卡只认脉冲 (Pulse)，舵机只认内部值。
// 本类负责 物理单位 <-> 脉冲 的双向换算。
//
// 注意：本类不自读配置文件，所有参数由 HardwareManager
// 通过 ConfigureAxis() 喂入，保持 HAL 底层纯净。
// ============================================================
class AxisConverter
{
public:
    static AxisConverter& Instance();

    // 由 HardwareManager 注入单轴换算参数
    void ConfigureAxis(int axisId, const AxisConfig& cfg);

    // 物理单位 -> 脉冲
    long ToPulse(int axisId, double mmOrDeg) const;
    // 脉冲 -> 物理单位
    double ToPhysical(int axisId, long pulse) const;
    // 物理速度 (mm/s 或 度/s) -> 脉冲速度 (Pulse/s)
    double SpeedToPulse(int axisId, double mmOrDegPerSec) const;
    // 脉冲速度 -> 物理速度
    double PulseToSpeed(int axisId, double pulsePerSec) const;

    // 每脉冲对应的物理量 (mm/pulse 或 度/pulse)
    double PhysicalPerPulse(int axisId) const;

private:
    AxisConverter() = default;
    ~AxisConverter() = default;
    AxisConverter(const AxisConverter&) = delete;
    AxisConverter& operator=(const AxisConverter&) = delete;

    const AxisConfig* Find(int axisId) const;

    std::unordered_map<int, AxisConfig> configs_;
};
