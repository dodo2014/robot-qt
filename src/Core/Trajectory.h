#pragma once

#include <functional>
#include <vector>

#include "Kinematics.h"

struct TrajectoryPoint
{
    JointAngles joints;
    double      timeFromStart = 0.0; // ms
};

class Trajectory
{
public:
    Trajectory();

    // 笛卡尔空间直线插补（分解为关节空间的离散点）
    std::vector<TrajectoryPoint> PlanLinear(const Pose3D& from, const Pose3D& to,
                                             double speedMmPerSec, double dtMs = 10.0) const;

    // 关节空间点到点运动
    std::vector<TrajectoryPoint> PlanPTP(const JointAngles& from, const JointAngles& to,
                                          double speedDps) const;

    // 多点路径规划（遍历途经点）
    std::vector<TrajectoryPoint> PlanPath(const std::vector<Pose3D>& waypoints,
                                           double speedMmPerSec) const;

    using StepCallback = std::function<bool(const JointAngles&)>;

    // 执行轨迹（逐点回调）
    bool Execute(const std::vector<TrajectoryPoint>& trajectory,
                 StepCallback callback) const;
};
