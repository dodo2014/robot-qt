#include "Trajectory.h"

#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>

Trajectory::Trajectory()
{
    SPDLOG_INFO("[Trajectory] Initialized");
}

// 笛卡尔空间直线插补：
// 1. 计算起点→终点的位移向量 (dx,dy,dz,dr) 与空间直线长度 dist（含 Z，忽略 R——R 单独线性插值）；
// 2. 按线速度算总时长 totalTimeMs = dist/speed×1000（1000 为 秒→毫秒 换算）；
// 3. 以 dtMs 为周期离散成 numPoints+1 个点（+1 保证含终点），归一化参数 t=i/numPoints∈[0,1] 逐点线性插值；
// 4. 每个插值点用 Kinematics::Inverse 逆解为关节角（elbowUp=true 固定上肘构型）。
std::vector<TrajectoryPoint> Trajectory::PlanLinear(const Pose& from, const Pose& to,
                                                     double speedMmPerSec, double dtMs) const
{
    double dx = to.x - from.x;
    double dy = to.y - from.y;
    double dz = to.z - from.z;
    double dr = to.r - from.r;
    double dist = std::sqrt(dx*dx + dy*dy + dz*dz);   // 空间直线长度 (mm)

    // 1e-6 (mm)：起点与终点重合（距离小于 0.001 微米），无运动可规划，返回空轨迹。
    if (dist < 1e-6)
    {
        SPDLOG_WARN("[Trajectory] PlanLinear: start and end are the same point");
        return {};
    }

    double totalTimeMs = (dist / speedMmPerSec) * 1000.0;   // ×1000：mm/s → ms 计时
    int numPoints = static_cast<int>(totalTimeMs / dtMs) + 1;   // 点数 = 总时长/周期，+1 覆盖终点

    std::vector<TrajectoryPoint> trajectory;
    trajectory.reserve(numPoints);

    Kinematics kin;   // 逆解器（L1=138.83 / L2=166.86，逻辑坐标）

    for (int i = 0; i <= numPoints; ++i)
    {
        double t = (numPoints > 0) ? static_cast<double>(i) / numPoints : 1.0;   // 归一化进度 [0,1]，i=numPoints 时 t=1 即终点
        Pose p;
        p.x = from.x + dx * t;   // 直线插值：起点 + 位移×进度
        p.y = from.y + dy * t;
        p.z = from.z + dz * t;
        p.r = from.r + dr * t;

        Joints joints;
        if (kin.Inverse(p, joints, true))   // 逆解失败（出工作区）的点直接跳过
        {
            TrajectoryPoint tp;
            tp.joints = joints;
            tp.timeFromStart = i * dtMs;   // 时刻 = 序号×周期 (ms)
            trajectory.push_back(tp);
        }
    }

    SPDLOG_INFO("[Trajectory] PlanLinear: {} points, dist={:.1f}mm, total={:.0f}ms",
                trajectory.size(), dist, totalTimeMs);
    return trajectory;
}

// 关节空间 PTP：以四轴中位移最大的轴为准确定总时长，保证最慢轴也匀速走满。
// maxDelta = max(|Δj1|, |Δj2|, |Δz|, |Δr|)，总时长 = maxDelta/speed×1000。
// 固定插补周期 dtMs=20ms（50Hz，关节插补比笛卡尔 10ms 更粗，够用且点数更少）。
std::vector<TrajectoryPoint> Trajectory::PlanPTP(const Joints& from, const Joints& to,
                                                  double speedDps) const
{
    double maxDelta = std::max({
        std::abs(to.j1 - from.j1),   // J1 位移 (°)
        std::abs(to.j2 - from.j2),   // J2 位移 (°)
        std::abs(to.z - from.z),     // Z 位移 (mm)
        std::abs(to.r - from.r),     // R 位移 (°)
    });

    double totalTimeMs = (maxDelta / speedDps) * 1000.0;   // ×1000：°/s → ms 计时
    double dtMs = 20.0;                                    // 关节插补周期 (ms)
    int numPoints = static_cast<int>(totalTimeMs / dtMs) + 1;   // +1 覆盖终点

    std::vector<TrajectoryPoint> trajectory;
    trajectory.reserve(numPoints);

    for (int i = 0; i <= numPoints; ++i)
    {
        double t = (numPoints > 0) ? static_cast<double>(i) / numPoints : 1.0;   // 归一化进度 [0,1]
        Joints j;
        j.j1 = from.j1 + (to.j1 - from.j1) * t;   // 每轴独立线性插值
        j.j2 = from.j2 + (to.j2 - from.j2) * t;
        j.z  = from.z  + (to.z  - from.z)  * t;
        j.r  = from.r  + (to.r  - from.r)  * t;

        TrajectoryPoint tp;
        tp.joints = j;
        tp.timeFromStart = i * dtMs;   // 时刻 = 序号×周期 (ms)
        trajectory.push_back(tp);
    }

    SPDLOG_INFO("[Trajectory] PlanPTP: {} points, total={:.0f}ms",
                trajectory.size(), totalTimeMs);
    return trajectory;
}

// 多点路径：逐段 PlanLinear，每段时间戳累加前段累计时长（currentTime），
// 形成整条时间连续（首尾相接不重叠）的轨迹序列。
std::vector<TrajectoryPoint> Trajectory::PlanPath(const std::vector<Pose>& waypoints,
                                                   double speedMmPerSec) const
{
    if (waypoints.size() < 2)   // 至少 2 个途经点才能构成一段运动
    {
        SPDLOG_WARN("[Trajectory] PlanPath: need at least 2 waypoints, got {}", waypoints.size());
        return {};
    }

    std::vector<TrajectoryPoint> fullTrajectory;
    double currentTime = 0.0;   // 已累积的轨迹总时长 (ms)

    for (size_t i = 0; i < waypoints.size() - 1; ++i)
    {
        auto segment = PlanLinear(waypoints[i], waypoints[i + 1], speedMmPerSec);
        for (auto& pt : segment)
        {
            pt.timeFromStart += currentTime;   // 本段时间戳平移到全局时间轴
            fullTrajectory.push_back(pt);
        }
        currentTime = fullTrajectory.empty() ? 0.0 : fullTrajectory.back().timeFromStart;   // 更新累计时长（取段末点时刻）
    }

    SPDLOG_INFO("[Trajectory] PlanPath: {} segments → {} total points",
                waypoints.size() - 1, fullTrajectory.size());
    return fullTrajectory;
}

// 执行轨迹：按时间顺序逐点回调（真正的定时下发由调用方负责）。
// 任一回调返回 false（如硬件拒绝、急停触发）立即中止剩余点并返回 false。
bool Trajectory::Execute(const std::vector<TrajectoryPoint>& trajectory,
                          StepCallback callback) const
{
    if (trajectory.empty())
    {
        SPDLOG_WARN("[Trajectory] Execute: empty trajectory");
        return false;
    }

    SPDLOG_INFO("[Trajectory] Execute: {} points", trajectory.size());
    for (const auto& pt : trajectory)
    {
        if (!callback(pt.joints))
        {
            SPDLOG_WARN("[Trajectory] Execute: callback returned false — aborting at t={:.0f}ms",
                        pt.timeFromStart);
            return false;
        }
    }
    SPDLOG_INFO("[Trajectory] Execute: completed");
    return true;
}
