#include "Kinematics.h"

#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>
#include <limits>

Kinematics::Kinematics()
{
    SPDLOG_INFO("[Kinematics] Initialized with default SCARA params: "
                 "L1={}, L2={}, L3={}, L4={}", l1_, l2_, l3_, l4_);
}

void Kinematics::SetLinkLengths(double l1, double l2, double l3, double l4)
{
    l1_ = l1; l2_ = l2; l3_ = l3; l4_ = l4;
    SPDLOG_INFO("[Kinematics] Link lengths updated: L1={}, L2={}, L3={}, L4={}",
                 l1_, l2_, l3_, l4_);
}

void Kinematics::SetDHParams(const std::vector<double>& params)
{
    dhParams_ = params;
    SPDLOG_INFO("[Kinematics] DH params updated ({} values)", params.size());
}

JointAngles Kinematics::Forward(const JointAngles& joint) const
{
    double t1 = joint.angles[0] * M_PI / 180.0;
    double t2 = joint.angles[1] * M_PI / 180.0;
    double d3 = joint.angles[2];
    double t4 = joint.angles[3] * M_PI / 180.0;

    double r  = l2_ * std::cos(t2) + l3_ * std::cos(t2 + t4);
    double ex = r * std::cos(t1);
    double ey = r * std::sin(t1);
    double ez = l1_ + d3;
    double eyaw = t1 + t2 + t4;

    JointAngles result;
    result.angles[0] = ex;
    result.angles[1] = ey;
    result.angles[2] = ez;
    result.angles[3] = eyaw * 180.0 / M_PI;

    return result;
}

Pose3D Kinematics::ForwardToPose(const JointAngles& joint) const
{
    double t1 = joint.angles[0] * M_PI / 180.0;
    double t2 = joint.angles[1] * M_PI / 180.0;
    double d3 = joint.angles[2];
    double t4 = joint.angles[3] * M_PI / 180.0;

    double r  = l2_ * std::cos(t2) + l3_ * std::cos(t2 + t4);
    double ex = r * std::cos(t1);
    double ey = r * std::sin(t1);
    double ez = l1_ + d3;
    double eyaw = t1 + t2 + t4;

    Pose3D pose;
    pose.x   = ex;
    pose.y   = ey;
    pose.z   = ez;
    pose.yaw = eyaw * 180.0 / M_PI;
    pose.pitch = 0.0;
    pose.roll  = 0.0;

    return pose;
}

bool Kinematics::Inverse(const Pose3D& target, JointAngles& output,
                         const JointAngles& initGuess) const
{
    JointAngles sol1, sol2;
    bool ok1 = ikSolve(target, sol1, true);
    bool ok2 = ikSolve(target, sol2, false);

    if (!ok1 && !ok2)
    {
        SPDLOG_WARN("[Kinematics] IK: No solution found for target ({:.1f}, {:.1f}, {:.1f})",
                     target.x, target.y, target.z);
        return false;
    }
    if (ok1 && !ok2) { output = sol1; return true; }
    if (!ok1 && ok2) { output = sol2; return true; }

    double d1 = 0.0, d2 = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        d1 += std::abs(sol1.angles[i] - initGuess.angles[i]);
        d2 += std::abs(sol2.angles[i] - initGuess.angles[i]);
    }
    output = (d1 <= d2) ? sol1 : sol2;
    return true;
}

bool Kinematics::ValidateJoints(const JointAngles& joint) const
{
    static constexpr double J1_LIMIT = 180.0;
    static constexpr double J2_LIMIT = 150.0;
    static constexpr double J3_MIN   = 0.0;
    static constexpr double J3_MAX   = 300.0;
    static constexpr double J4_LIMIT = 180.0;

    if (std::abs(joint.angles[0]) > J1_LIMIT) return false;
    if (std::abs(joint.angles[1]) > J2_LIMIT) return false;
    if (joint.angles[2] < J3_MIN || joint.angles[2] > J3_MAX) return false;
    if (std::abs(joint.angles[3]) > J4_LIMIT) return false;
    return true;
}

bool Kinematics::ikSolve(const Pose3D& target, JointAngles& sol, bool elbowUp) const
{
    double x = target.x;
    double y = target.y;
    double z = target.z;
    double yaw = target.yaw * M_PI / 180.0;

    double d3 = z - l1_;
    if (d3 < 0) return false;

    double r = std::sqrt(x * x + y * y);
    if (r < 1e-6) return false;

    double t1 = std::atan2(y, x);
    double cos_t2_num = (l2_ * l2_ + r * r - l3_ * l3_) / (2.0 * l2_ * r);
    if (std::abs(cos_t2_num) > 1.0) return false;
    double t2_abs = std::acos(cos_t2_num);
    double t2 = elbowUp ? -t2_abs : t2_abs;

    double alpha = std::atan2(l3_ * std::sin(t2), l2_ + l3_ * std::cos(t2));
    double beta  = std::atan2(r * std::sin(t2), r * std::cos(t2));

    double phi = std::atan2(l3_ * std::sin(std::abs(t2)), l2_ + l3_ * std::cos(std::abs(t2)));
    double t4 = yaw - t1 - t2;

    sol.angles[0] = t1 * 180.0 / M_PI;
    sol.angles[1] = t2 * 180.0 / M_PI;
    sol.angles[2] = d3;
    sol.angles[3] = t4 * 180.0 / M_PI;

    return ValidateJoints(sol);
}
