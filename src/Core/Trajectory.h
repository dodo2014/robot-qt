#pragma once

#include <functional>
#include <vector>

#include "Kinematics.h"

// 轨迹离散点：某一时刻的关节目标 + 从轨迹起点算起的时刻（毫秒）。
// 上游逐点下发硬件（走 HardwareManager 门面），由 timeFromStart 决定发点节奏。
struct TrajectoryPoint
{
    Joints  joints;                      // 该时刻的关节角/高度目标（逻辑坐标）
    double  timeFromStart = 0.0;         // 相对轨迹起点的时刻 (ms)，起点为 0
};

class Trajectory
{
public:
    Trajectory();

    // 笛卡尔空间直线插补（分解为关节空间的离散点）。
    // speedMmPerSec = 末端线速度 (mm/s)；dtMs = 插补周期 (ms，默认 10ms，
    // 即每 10ms 一个点，100Hz 刷新率)。对每个中间点用 Inverse 逆解成关节角，
    // 逆解失败（出工作区）的点跳过。
    std::vector<TrajectoryPoint> PlanLinear(const Pose& from, const Pose& to,
                                             double speedMmPerSec, double dtMs = 10.0) const;

    // 关节空间点到点运动：四轴(j1/j2/z/r)独立线性插值。
    // speedDps = 关节速度 (°/s，Z 轴按 mm/s 同量表)；内部固定插补周期 20ms。
    std::vector<TrajectoryPoint> PlanPTP(const Joints& from, const Joints& to,
                                          double speedDps) const;

    // 多点路径规划：顺序连接相邻途经点，各段独立 PlanLinear，
    // 后续点段的 timeFromStart 累加前段时长（时间连续不重叠）。
    std::vector<TrajectoryPoint> PlanPath(const std::vector<Pose>& waypoints,
                                           double speedMmPerSec) const;

    using StepCallback = std::function<bool(const Joints&)>;

    // 执行轨迹（逐点回调）：按时间顺序对每个离散点调用 callback(joints)；
    // 回调返回 false 立即中止整个轨迹执行并返回 false。
    bool Execute(const std::vector<TrajectoryPoint>& trajectory,
                 StepCallback callback) const;
};
