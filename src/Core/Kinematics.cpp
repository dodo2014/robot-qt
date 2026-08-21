#include "Kinematics.h"

#include <spdlog/spdlog.h>
#include <cmath>
#include <algorithm>

namespace
{
constexpr double kPi = 3.14159265358979323846; // π：圆周率，用于弧角度互转
constexpr double kDegToRad = kPi / 180.0;      // 角度转弧度系数：1° = π/180 rad ≈ 0.0174533
}

Kinematics::Kinematics()
{
    SPDLOG_INFO("[Kinematics] Initialized (2D SCARA + independent R + TCP-inward): L1={}, L2={}, Z0={}, H1={}, TCPf={}, TCPd={}",
                l1_, l2_, z0_, h1_, tcpForward_, tcpDown_);
}

void Kinematics::SetParams(double l1xy, double l2, double z0, double h1)
{
    l1_ = l1xy;
    l2_ = l2;
    z0_ = z0;
    h1_ = h1;
    SPDLOG_INFO("[Kinematics] Params updated: L1={}, L2={}, Z0={}, H1={}", l1_, l2_, z0_, h1_);
}

void Kinematics::SetTCP(double offsetX, double offsetY, double offsetZ)
{
    // 映射：沿小臂(L2)向前的距离 = X 向偏移；竖直向下物理距离 = -Z 向偏移。
    // 惯例 offsetZ 为负表示向下（config 默认 -130），故 tcpDown_ 恒为正数。
    tcpForward_ = offsetX;
    tcpDown_ = -offsetZ;

    if (offsetY != 0.0)
    {
        SPDLOG_WARN("[Kinematics] SetTCP: offsetY={} ignored — current 2D model ignores Y-direction TCP offset",
                    offsetY);
    }

    SPDLOG_INFO("[Kinematics] TCP set: forward={}mm, down={}mm", tcpForward_, tcpDown_);
}

void Kinematics::SetJointLimits(double j1min, double j1max,
                                double j2min, double j2max,
                                double zmin, double zmax,
                                double rmin, double rmax)
{
    j1Min_ = j1min; j1Max_ = j1max;
    j2Min_ = j2min; j2Max_ = j2max;
    zMin_ = zmin;   zMax_ = zmax;
    rMin_ = rmin;   rMax_ = rmax;
}

// 正解（Forward Kinematics）：由关节角算夹爪尖端坐标（TCP 已内化）。
// 等效小臂 l2_eff = l2_ + tcpForward_：夹爪沿小臂(L2)向前延伸，平面投影按该有效长度计算。
// SCARA 平面模型：第一段(L1)方向角 = J1；第二段(l2_eff)方向角 = J1 + J2（J2 为相对角）。
// 故末端 = L1·(cosJ1, sinJ1) + l2_eff·(cos(J1+J2), sin(J1+J2))。
// Z：夹爪尖端真实高度 = Z 电机物理高度 + z0 - h1 - tcpDown_。
//   z0 = 撞顶时大臂上表面绝对高度；h1 = 大臂向下倾斜落差；tcpDown_ = 夹爪向下延伸长度。
Pose Kinematics::Forward(const Joints& joints) const
{
    double l2_eff = l2_ + tcpForward_;   // 等效小臂 (mm)：小臂本体 + 夹爪沿 L2 向前延伸
    double t1 = joints.j1 * kDegToRad;   // J1 弧度：度 × π/180
    double t2 = joints.j2 * kDegToRad;   // J2 弧度（相对角）

    Pose pose;
    pose.x = l1_ * std::cos(t1) + l2_eff * std::cos(t1 + t2);   // 夹爪尖端 X = 大臂投影 + 等效小臂投影
    pose.y = l1_ * std::sin(t1) + l2_eff * std::sin(t1 + t2);   // 夹爪尖端 Y = 大臂投影 + 等效小臂投影
    pose.z = joints.z + z0_ - h1_ - tcpDown_;   // 夹爪尖端真实高度 (mm)
    pose.r = joints.r;                          // R 翻转轴独立，原样透传 (°)
    return pose;
}

// 关节角合法性校验：四个关节都必须落在各自的软限位区间内。
bool Kinematics::ValidateJoints(const Joints& joints) const
{
    if (joints.j1 < j1Min_ || joints.j1 > j1Max_) return false;
    if (joints.j2 < j2Min_ || joints.j2 > j2Max_) return false;
    if (joints.z < zMin_ || joints.z > zMax_) return false;
    if (joints.r < rMin_ || joints.r > rMax_) return false;
    return true;
}

// 逆解入口：先求纯数学解（ikSolve），再校验关节限位，全部通过才写回 out。
bool Kinematics::Inverse(const Pose& target, Joints& out, bool elbowUp) const
{
    Joints sol;
    if (!ikSolve(target, sol, elbowUp))
        return false;

    if (!ValidateJoints(sol))
    {
        SPDLOG_WARN("[Kinematics] IK: solution out of joint limits: J1={:.1f} J2={:.1f} Z={:.1f} R={:.1f}",
                    sol.j1, sol.j2, sol.z, sol.r);
        return false;
    }

    out = sol;
    return true;
}

// 双解就近逆解：同时求上肘/下肘两组解，取合法且 J2 最接近当前 J2 的一组，
// 避免相邻目标点间 J2 大跳变（甩臂）。
bool Kinematics::InverseSmart(const Pose& target, Joints& out, double currentJ2) const
{
    Joints up, down;
    bool okUp   = ikSolve(target, up, true);
    bool okDown = ikSolve(target, down, false);

    bool validUp   = okUp   && ValidateJoints(up);
    bool validDown = okDown && ValidateJoints(down);

    if (!validUp && !validDown)
    {
        SPDLOG_WARN("[Kinematics] IK(Smart): no valid solution for target ({:.1f}, {:.1f}, {:.1f})",
                    target.x, target.y, target.z);
        return false;
    }
    if (validUp && !validDown)   { out = up;   return true; }   // 仅上肘合法
    if (!validUp && validDown)   { out = down; return true; }   // 仅下肘合法

    double dUp   = std::fabs(up.j2 - currentJ2);   // 上肘与当前 J2 的偏差
    double dDown = std::fabs(down.j2 - currentJ2); // 下肘与当前 J2 的偏差
    out = (dUp <= dDown) ? up : down;              // 取偏差小者（就近原则）
    return true;
}

// 纯 2D 平面逆解核心（余弦定理法）。坐标系约定：
//   夹爪尖端在基座正前方，J1=0 时大臂沿 X 正方向；J2 为小臂相对大臂的转角（正=逆时针）。
// 等效小臂 l2_eff = l2_ + tcpForward_（夹爪沿 L2 向前延伸，TCP 已内化）。
// 甜甜圈工作空间（Annulus）：二连杆可达范围是外径 (l1 + l2_eff) 与内径 |l1 - l2_eff| 之间的圆环，
//   原点（内孔）与超出外径均不可达，必须双向边界校验。
// 推导（与 k1/k2 系数一致）：
//   由尖端 (x,y) 得极径 r = √(x²+y²)。三角形三边为 l1、l2_eff、r，
//   余弦定理 cosθ2 = (r² − l1² − l2_eff²) / (2·l1·l2_eff)（θ2 为 l1 与 l2_eff 夹角，
//   实际 J2 = ±acos(cosθ2)，正负号即上肘/下肘构型）。
//   J1 = atan2(y,x) − atan2(l2_eff·sinθ2, l1 + l2_eff·cosθ2)（尖端极角减去小臂相对偏转角）。
bool Kinematics::ikSolve(const Pose& target, Joints& sol, bool elbowUp) const
{
    double x = target.x;
    double y = target.y;
    double rSq = x * x + y * y;                  // 夹爪尖端极径平方（mm²）
    double r = std::sqrt(rSq);                   // 夹爪尖端极径（mm）
    double l2_eff = l2_ + tcpForward_;           // 等效小臂 (mm)：小臂本体 + 夹爪沿 L2 向前延伸

    // 甜甜圈外边界：尖端距离超过最大可达半径 l1 + l2_eff → 不可达。
    // 0.001 为浮点容差(mm)：允许目标刚好在边界上因舍入产生的 1mm 误差。
    if (r > (l1_ + l2_eff) + 0.001)
    {
        SPDLOG_WARN("[Kinematics] IK: target out of workspace (beyond outer radius), dist={:.2f} > L1+L2_eff={:.2f}",
                    r, l1_ + l2_eff);
        return false;
    }

    // 甜甜圈内边界：尖端距离小于内孔半径 |l1 - l2_eff| → 落入空心区（含原点），物理上不可达。
    // 0.001 为浮点容差(mm)。当 l2_eff ≥ l1 时内孔半径 = l2_eff - l1（如 268 - 138.83 = 129.17mm）。
    double rInner = std::fabs(l1_ - l2_eff);
    if (r < rInner - 0.001)
    {
        SPDLOG_WARN("[Kinematics] IK: target inside annulus hole, dist={:.2f} < |L1-L2_eff|={:.2f}",
                    r, rInner);
        return false;
    }

    // 余弦定理求 J2（相对角）：
    //   cosθ2 = (r² − l1² − l2_eff²) / (2·l1·l2_eff)
    //   分母 2·l1·l2_eff 恒为正，分子可为负（目标接近内孔时 θ2 为钝角）。
    double cosTheta2 = (rSq - l1_ * l1_ - l2_eff * l2_eff) / (2.0 * l1_ * l2_eff);
    // 数值稳定：浮点舍入可能使 cosθ2 略超 [-1, 1]，acos 会返回 NaN，
    // 钳制到合法区间后保证 θ2 可算。
    cosTheta2 = std::max(-1.0, std::min(1.0, cosTheta2));

    // J2 = ±acos(cosθ2)：elbowUp=true 取正(上肘，小臂逆时针抬起)，
    // false 取负(下肘，小臂顺时针落下)，即双解。
    double theta2 = elbowUp ? std::acos(cosTheta2)
                            : -std::acos(cosTheta2);

    // 求 J1：尖端极角 atan2(y,x) 减去小臂相对大臂造成的偏转角。
    // 小臂端点在"以 J2 为原点的极坐标"里为 (l2_eff·cosθ2, l2_eff·sinθ2)，
    // 故偏转角 = atan2(l2_eff·sinθ2, l1 + l2_eff·cosθ2)（分母是 l1 加小臂在 J1 方向的投影）。
    double k1 = l1_ + l2_eff * cosTheta2;        // 小臂端点相对 J1 轴的 X 向投影
    double k2 = l2_eff * std::sin(theta2);       // 小臂端点相对 J1 轴的 Y 向投影
    double theta1 = std::atan2(y, x) - std::atan2(k2, k1);

    sol.j1 = theta1 / kDegToRad;   // 弧度转角度：× 180/π
    sol.j2 = theta2 / kDegToRad;
    sol.z  = target.z - z0_ + h1_ + tcpDown_;  // 反推 Z 电机物理高度 = 目标尖端高度 − z0 + h1 + tcpDown_
    sol.r  = target.r;                         // R 轴独立，直接透传 (°)
    return true;
}