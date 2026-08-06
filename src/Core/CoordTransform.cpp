#include "CoordTransform.h"

#include <spdlog/spdlog.h>
#include <cmath>

CoordTransform::CoordTransform()
{
    SPDLOG_INFO("[CoordTransform] Initialized");
}

void CoordTransform::SetCameraIntrinsics(double fx, double fy, double cx, double cy)
{
    fx_ = fx; fy_ = fy; cx_ = cx; cy_ = cy;
    SPDLOG_INFO("[CoordTransform] Camera intrinsics set: fx={}, fy={}, cx={}, cy={}",
                 fx, fy, cx, cy);
}

void CoordTransform::SetHandEyeMatrix(const std::array<double, 16>& matrix)
{
    handEyeMatrix_ = matrix;
    hasHandEye_ = true;
    SPDLOG_INFO("[CoordTransform] Hand-eye matrix set (4x4)");
}

void CoordTransform::SetCameraOffset(double dx, double dy, double rotationDeg)
{
    camOffsetX_ = dx; camOffsetY_ = dy; camRotation_ = rotationDeg;
    SPDLOG_INFO("[CoordTransform] Camera offset: dx={}, dy={}, rotation={}°",
                 dx, dy, rotationDeg);
}

void CoordTransform::SetGripperOffset(double dx, double dy, double zDiff)
{
    gripOffsetX_ = dx; gripOffsetY_ = dy; gripZDiff_ = zDiff;
    SPDLOG_INFO("[CoordTransform] Gripper offset: dx={}, dy={}, zDiff={}",
                 dx, dy, zDiff);
}

void CoordTransform::SetGripperInstallAngle(double angleDeg)
{
    gripInstallAngle_ = angleDeg;
    SPDLOG_INFO("[CoordTransform] Gripper install angle: {}°", angleDeg);
}

void CoordTransform::SetAngleOffset(double offsetDeg)
{
    angleOffset_ = offsetDeg;
    SPDLOG_INFO("[CoordTransform] Angle offset: {}°", offsetDeg);
}

Pose3D CoordTransform::PixelToRobot(int u, int v, float depthMm,
                                    const JointAngles& currentJoints) const
{
    if (fx_ <= 0 || fy_ <= 0)
    {
        SPDLOG_ERROR("[CoordTransform] Camera intrinsics not set");
        return {};
    }

    double xc = (u - cx_) * depthMm / fx_;
    double yc = (v - cy_) * depthMm / fy_;
    double zc = depthMm;

    return CameraToRobot(xc, yc, zc, currentJoints);
}

Pose3D CoordTransform::CameraToRobot(double xc, double yc, double zc,
                                     const JointAngles& currentJoints) const
{
    (void)currentJoints;

    double rotRad = camRotation_ * M_PI / 180.0;
    double cosR = std::cos(rotRad);
    double sinR = std::sin(rotRad);

    double xr = xc * cosR - yc * sinR + camOffsetX_ + gripOffsetX_;
    double yr = xc * sinR + yc * cosR + camOffsetY_ + gripOffsetY_;
    double zr = zc + gripZDiff_;

    double j1 = currentJoints.angles[0];
    double j2 = currentJoints.angles[1];
    double j4 = currentJoints.angles[3];
    double yaw = j1 + j2 + j4 + gripInstallAngle_ + angleOffset_;

    Pose3D pose;
    pose.x   = xr;
    pose.y   = yr;
    pose.z   = zr;
    pose.yaw = yaw;
    return pose;
}
