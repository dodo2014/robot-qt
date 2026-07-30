#pragma once

#include <array>

#include "Kinematics.h"

class CoordTransform
{
public:
    CoordTransform();

    // 设置手眼标定参数
    void SetCameraIntrinsics(double fx, double fy, double cx, double cy);
    void SetHandEyeMatrix(const std::array<double, 16>& matrix); // 4x4 row-major
    void SetCameraOffset(double dx, double dy, double rotationDeg);
    void SetGripperOffset(double dx, double dy, double zDiff);
    void SetGripperInstallAngle(double angleDeg);
    void SetAngleOffset(double offsetDeg);

    // 像素 → 机器人基座标
    Pose3D PixelToRobot(int u, int v, float depthMm,
                        const JointAngles& currentJoints) const;

    // 相机坐标 → 机器人基座标
    Pose3D CameraToRobot(double xc, double yc, double zc,
                         const JointAngles& currentJoints) const;

private:
    double fx_ = 0, fy_ = 0, cx_ = 0, cy_ = 0;
    double camOffsetX_ = 0, camOffsetY_ = 0, camRotation_ = 0;
    double gripOffsetX_ = 0, gripOffsetY_ = 0, gripZDiff_ = 0;
    double gripInstallAngle_ = 0;
    double angleOffset_ = 0;

    std::array<double, 16> handEyeMatrix_{}; // T_cam_to_robot (4x4 row-major)
    bool hasHandEye_ = false;
};
