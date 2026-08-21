#pragma once

#include <array>

#include "Kinematics.h"

// ============================================================
// 相机坐标 → 机器人基座标转换（3D 手眼标定）
// 依据 gemini_qr.md：
//   像素(u,v) + 深度 → 相机内参 → 相机系坐标 (xc,yc,zc)
//   → 手眼外参 4×4 齐次矩阵 → 机器人基座标 (x,y,z)
// 与 TCP 剥离、逆解严格分层：本类只做"相机→基座"，
// 姿态(r)由上层业务决定，不在此计算。
// ============================================================
class CoordTransform
{
public:
    CoordTransform();

    // 相机内参：fx/fy = 焦距（像素单位），cx/cy = 主点（光心）像素坐标。
    // 全部 > 0 才算已设置；未设置时 PixelToRobot 返回零位 Pose。
    void SetCameraIntrinsics(double fx, double fy, double cx, double cy);

    // 手眼外参矩阵 T_cam_to_robot（4x4，row-major，16 个元素），源：
    // config.tcpCalibration.handEyeMatrix（九点/3D 标定结果）
    // 语义：p_robot = T × p_cam（齐次坐标），把相机系下的点映射到机器人基座系。
    void SetHandEyeMatrix(const std::array<double, 16>& matrix);
    bool HasHandEye() const { return hasHandEye_; }

    // 相机系坐标（如 PuffResult 输出的 x/y/z，单位 mm）→ 机器人基座标。
    // 未设置手眼矩阵时返回原值（单位阵退化，即假设相机系=基座标）。
    Pose CameraToRobot(double xc, double yc, double zc) const;

    // 像素 + 深度(mm) → 机器人基座标（内参 + 手眼两步）。
    // 内参未设置或深度非法(≤0) 返回零位 Pose。
    Pose PixelToRobot(int u, int v, double depthMm) const;

private:
    double fx_ = 0.0, fy_ = 0.0, cx_ = 0.0, cy_ = 0.0;   // 相机内参（像素单位），0.0 表示未设置
    std::array<double, 16> handEye_{};   // T_cam_to_robot (4x4 row-major，16 元素)
    bool hasHandEye_ = false;            // 手眼矩阵是否已通过 SetHandEyeMatrix 设置
};
