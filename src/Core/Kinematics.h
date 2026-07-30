#pragma once

#include <array>
#include <vector>

#ifdef HAS_EIGEN
#include <Eigen/Dense>
#endif

struct Pose3D
{
    double x = 0.0, y = 0.0, z = 0.0;
    double roll = 0.0, pitch = 0.0, yaw = 0.0;
};

struct JointAngles
{
    std::array<double, 4> angles{}; // J1(°) J2(°) J3(mm) J4(°)
};

class Kinematics
{
public:
    Kinematics();

    void SetLinkLengths(double l1, double l2, double l3, double l4);
    void SetDHParams(const std::vector<double>& params);

    JointAngles Forward(const JointAngles& joint) const;
    Pose3D      ForwardToPose(const JointAngles& joint) const;

    bool Inverse(const Pose3D& target, JointAngles& output,
                 const JointAngles& initGuess = JointAngles{}) const;

    bool ValidateJoints(const JointAngles& joint) const;

private:
    double l1_ = 168.5;
    double l2_ = 190.0;
    double l3_ = 145.3;
    double l4_ = 0.0;

    std::vector<double> dhParams_;

    bool ikSolve(const Pose3D& target, JointAngles& sol, bool elbowUp) const;
};
