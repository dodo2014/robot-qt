#include "Trajectory.h"

#include <spdlog/spdlog.h>
#include <cmath>
#include <limits>

Trajectory::Trajectory()
{
    SPDLOG_INFO("[Trajectory] Initialized");
}

std::vector<TrajectoryPoint> Trajectory::PlanLinear(const Pose3D& from, const Pose3D& to,
                                                     double speedMmPerSec, double dtMs) const
{
    double dx = to.x - from.x;
    double dy = to.y - from.y;
    double dz = to.z - from.z;
    double dist = std::sqrt(dx*dx + dy*dy + dz*dz);

    if (dist < 1e-6)
    {
        SPDLOG_WARN("[Trajectory] PlanLinear: start and end are the same point");
        return {};
    }

    double totalTimeMs = (dist / speedMmPerSec) * 1000.0;
    int numPoints = static_cast<int>(totalTimeMs / dtMs) + 1;

    std::vector<TrajectoryPoint> trajectory;
    trajectory.reserve(numPoints);

    Kinematics kin;

    for (int i = 0; i <= numPoints; ++i)
    {
        double t = (numPoints > 0) ? static_cast<double>(i) / numPoints : 1.0;
        Pose3D p;
        p.x   = from.x + dx * t;
        p.y   = from.y + dy * t;
        p.z   = from.z + dz * t;
        p.yaw = from.yaw + (to.yaw - from.yaw) * t;

        JointAngles joints;
        if (kin.Inverse(p, joints))
        {
            TrajectoryPoint tp;
            tp.joints = joints;
            tp.timeFromStart = i * dtMs;
            trajectory.push_back(tp);
        }
    }

    SPDLOG_INFO("[Trajectory] PlanLinear: {} points, dist={:.1f}mm, total={:.0f}ms",
                 trajectory.size(), dist, totalTimeMs);
    return trajectory;
}

std::vector<TrajectoryPoint> Trajectory::PlanPTP(const JointAngles& from, const JointAngles& to,
                                                   double speedDps) const
{
    double maxDelta = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        double delta = std::abs(to.angles[i] - from.angles[i]);
        if (delta > maxDelta) maxDelta = delta;
    }

    double totalTimeMs = (maxDelta / speedDps) * 1000.0;
    double dtMs = 20.0;
    int numPoints = static_cast<int>(totalTimeMs / dtMs) + 1;

    std::vector<TrajectoryPoint> trajectory;
    trajectory.reserve(numPoints);

    for (int i = 0; i <= numPoints; ++i)
    {
        double t = (numPoints > 0) ? static_cast<double>(i) / numPoints : 1.0;
        JointAngles j;
        for (int k = 0; k < 4; ++k)
            j.angles[k] = from.angles[k] + (to.angles[k] - from.angles[k]) * t;

        TrajectoryPoint tp;
        tp.joints = j;
        tp.timeFromStart = i * dtMs;
        trajectory.push_back(tp);
    }

    SPDLOG_INFO("[Trajectory] PlanPTP: {} points, total={:.0f}ms",
                 trajectory.size(), totalTimeMs);
    return trajectory;
}

std::vector<TrajectoryPoint> Trajectory::PlanPath(const std::vector<Pose3D>& waypoints,
                                                    double speedMmPerSec) const
{
    if (waypoints.size() < 2)
    {
        SPDLOG_WARN("[Trajectory] PlanPath: need at least 2 waypoints, got {}", waypoints.size());
        return {};
    }

    std::vector<TrajectoryPoint> fullTrajectory;
    double currentTime = 0.0;

    for (size_t i = 0; i < waypoints.size() - 1; ++i)
    {
        auto segment = PlanLinear(waypoints[i], waypoints[i + 1], speedMmPerSec);
        for (auto& pt : segment)
        {
            pt.timeFromStart += currentTime;
            fullTrajectory.push_back(pt);
        }
        currentTime = fullTrajectory.empty() ? 0.0 : fullTrajectory.back().timeFromStart;
    }

    SPDLOG_INFO("[Trajectory] PlanPath: {} segments → {} total points",
                 waypoints.size() - 1, fullTrajectory.size());
    return fullTrajectory;
}

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
