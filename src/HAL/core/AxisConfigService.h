#pragma once

#include <QString>
#include <QVector>

#include "AxisMap.h"
#include "IMotionCard.h"

// 每轴速度/单位/软限位查询服务（从 HardwareManager 拆出，纯查询 + 快照更新）。
// 读取优先级：jogSpeed/AxisUnit 用 config 快照（configs_）；maxSpeed/maxAccel/limitMin/limitMax
// 实时读 ConfigManager（与「电控与映射」编辑保持一致）。底层品牌代码不依赖本类。
class AxisConfigService
{
public:
    explicit AxisConfigService(QVector<AxisConfig>& configs);

    double GetJogSpeed(LogicalAxis axis) const;
    double GetMaxSpeed(LogicalAxis axis) const;
    double GetMaxAccel(LogicalAxis axis) const;
    bool   SetJogSpeed(LogicalAxis axis, double mmOrDegPerSec);

    // 轴显示单位（rotation→"°"，linear→"mm"，舵机→"°"）
    QString AxisUnit(LogicalAxis axis) const;

    double GetLimitMin(LogicalAxis axis) const;
    double GetLimitMax(LogicalAxis axis) const;
    bool   IsWithinSoftLimits(LogicalAxis axis, double pos) const;

private:
    QVector<AxisConfig>& configs_;
};
