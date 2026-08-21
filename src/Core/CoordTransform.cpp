#include "CoordTransform.h"

#include <Eigen/Dense>
#include <spdlog/spdlog.h>

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
    handEye_ = matrix;
    hasHandEye_ = true;
    SPDLOG_INFO("[CoordTransform] Hand-eye matrix set (4x4)");
}

// 相机系坐标 → 机器人基座标。齐次变换：p_base = T × p_cam。
// p_cam 补 w=1 使平移分量生效；T 为 row-major 存储的 4×4 矩阵，
// handEye_[i*4+j] 即第 i 行第 j 列元素（i,j ∈ 0..3）。
Pose CoordTransform::CameraToRobot(double xc, double yc, double zc) const
{
    Eigen::Vector4d pCam(xc, yc, zc, 1.0);   // 齐次坐标，w 恒为 1.0（有效点而非无穷远点）

    if (!hasHandEye_)
    {
        SPDLOG_WARN("[CoordTransform] Hand-eye matrix not set, returning camera coords unchanged");
        Pose out;
        out.x = xc;
        out.y = yc;
        out.z = zc;
        return out;
    }

    Eigen::Matrix4d T;   // 手眼外参矩阵（Eigen 列主序，此处逐元素填值，语义仍为 row-major）
    for (int i = 0; i < 4; ++i)          // 行：0..3（前 3 行为旋转+平移，第 4 行为 [0,0,0,1]）
        for (int j = 0; j < 4; ++j)      // 列：0..3
            T(i, j) = handEye_[i * 4 + j];   // row-major 展开：第 i 行第 j 列 = 元素 [i*4+j]

    Eigen::Vector4d pBase = T * pCam;    // 齐次变换：相机系点 → 机器人基座标

    Pose out;
    out.x = pBase(0);   // 结果 X 分量 (mm)
    out.y = pBase(1);   // 结果 Y 分量 (mm)
    out.z = pBase(2);   // 结果 Z 分量 (mm)
    return out;
}

// 像素 + 深度 → 机器人基座标（内参反投影 + 手眼两步）。
// 小孔成像反投影（针孔模型）：以光心为原点，u/v 相对主点 (cx,cy) 的偏移
// 除以焦距 fx/fy 得到归一化方向，再乘深度 depthMm 得到相机系坐标：
//   xc = (u − cx) × depth / fx     （像素列差 → mm）
//   yc = (v − cy) × depth / fy     （像素行差 → mm）
//   zc = depth                     （深度方向即相机光轴方向）
Pose CoordTransform::PixelToRobot(int u, int v, double depthMm) const
{
    if (fx_ <= 0.0 || fy_ <= 0.0 || depthMm <= 0.0)
    {
        // fx_/fy_ 为 0 表示内参未设置（无标定数据），depthMm ≤ 0 表示深度无效
        SPDLOG_WARN("[CoordTransform] Camera intrinsics not set or invalid depth {}", depthMm);
        return {};
    }

    double xc = (static_cast<double>(u) - cx_) * depthMm / fx_;   // 像素列偏移换算为 X (mm)
    double yc = (static_cast<double>(v) - cy_) * depthMm / fy_;   // 像素行偏移换算为 Y (mm)
    double zc = depthMm;                                         // 深度即相机系 Z (mm)

    return CameraToRobot(xc, yc, zc);   // 第二步：相机系 → 机器人基座标
}
