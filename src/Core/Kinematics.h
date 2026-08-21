#pragma once

// ============================================================
// 运动学核心（降维 2D SCARA + 独立 R 翻转 + TCP 内化等效小臂）
// 依据 gemini_qr.md 结论：
//   - L1 = 大臂水平投影距离（J1 轴心 → J2 轴心连线在水平面投影）
//   - 正逆解退化为 2D 平面三角（X/Y 由 L1/L2 决定）+ 独立 Z 升降
//   - R(Pitch 翻转) 不参与平面正逆解，直接透传目标角
//   - 夹爪直接安装在 J4 前方并朝下 → TCP 内化为"等效小臂"：
//       平面 l2_eff = L2 + tcpForward_（沿小臂向前延伸），
//       Z 方向再扣除 tcpDown_（竖直向下物理延伸）。
//   - 全库使用"逻辑角度"（已扣 HomeOffset），机械换算在 HAL 层
// ============================================================

// 工具坐标（TCP/夹爪尖端，单位 mm/度）。x/y/z = 夹爪尖端，为内化 TCP 后的真实位置，
// r = 夹爪翻转角 Pitch（垂直朝下为 0，向上为负，向下为正）。
struct Pose
{
    double x = 0.0, y = 0.0, z = 0.0, r = 0.0;   // 默认零位：原点到地平面，翻转角垂直朝下
};

// 逻辑关节角。j1/j2 = 大臂/小臂角度（°），z = Z 轴电机物理高度（mm），
// r = 夹爪翻转角（°），与 Pose.r 语义一致（透传）。
struct Joints
{
    double j1 = 0.0, j2 = 0.0, z = 0.0, r = 0.0;  // 默认零位：两臂共线朝 X 正方向、Z 落到基准面
};

class Kinematics
{
public:
    Kinematics();

    // 连杆参数：l1xy = 大臂水平投影（138.83mm，2026-08 由 174.35 重测修正）、l2 = 小臂长（166.86mm）、
    // z0 = 撞顶时大臂上表面的绝对高度（mm）、h1 = 大臂向下倾斜落差（mm）。
    void SetParams(double l1xy, double l2, double z0, double h1);
    double L1() const { return l1_; }   // 大臂水平投影长 (mm)
    double L2() const { return l2_; }   // 小臂长 (mm)
    double Z0() const { return z0_; }   // 大臂上表面基准高度 (mm)
    double H1() const { return h1_; }   // 大臂向下倾斜落差 (mm)

    // TCP 配置接口（与 config.json 的 tcpCalibration.toolOffsetX/Y/Z 联动）：
    //   offsetX = 夹爪沿小臂(L2)向前的距离 (mm) → 内部 tcpForward_
    //   offsetZ = 夹爪竖直偏移 (mm)，惯例为负表示向下 → 内部 tcpDown_ = -offsetZ（绝对正数）
    //   offsetY ≠ 0 时打印警告：当前模型（2D 平面）忽略 Y 向 TCP 偏移。
    void SetTCP(double offsetX, double offsetY, double offsetZ);
    double TCPForward() const { return tcpForward_; }   // 沿小臂(L2)向前的 TCP 距离 (mm)
    double TCPDown() const { return tcpDown_; }         // 竖直向下的 TCP 物理距离 (mm，绝对正数)

    // 关节软限位（逻辑角度/高度，默认宽松 ±180°/±1000mm）。由上层从 config 读取喂入。
    void SetJointLimits(double j1min, double j1max,
                        double j2min, double j2max,
                        double zmin, double zmax,
                        double rmin, double rmax);

    // 正解：关节角 → 夹爪尖端世界坐标（已含 TCP）。r 原样透传。
    //   X/Y 用等效小臂 l2_eff = L2 + tcpForward_ 计算（夹爪沿小臂向前延伸）。
    //   Z = Z 电机物理高度 + z0 - h1 - tcpDown_（夹爪尖端真实高度）。
    Pose Forward(const Joints& joints) const;

    // 逆解（输入夹爪尖端目标，已含 TCP）。肘部构型 elbowUp 选择双解之一
    // （elbowUp=true 取 J2 为正的"上肘"，false 取 J2 为负的"下肘"）。
    // 目标落入甜甜圈内孔 / 超出外径 / 超限位 → 返回 false（只记 SPDLOG_WARN，不抛异常）。
    bool Inverse(const Pose& target, Joints& out, bool elbowUp = true) const;

    // 双解就近：按当前 J2 选择 elbow_up/down 中更近的一组，避免大甩臂。
    bool InverseSmart(const Pose& target, Joints& out, double currentJ2) const;

    bool ValidateJoints(const Joints& joints) const;

private:
    double l1_ = 138.83;   // 大臂水平投影 (mm)：J1 轴心到 J2 轴心的水平距离（2026-08 重测 174.35 → 138.83）
    double l2_ = 166.86;   // 小臂长 (mm)：J2 轴心到腕点(J4 轴心)的距离
    double z0_ = 0.0;      // 大臂上表面基准高度 (mm)：撞顶时大臂上表面的绝对高度
    double h1_ = 0.0;      // 大臂向下倾斜落差 (mm)：大臂倾斜致腕点比大臂根部低 h1（0 = 无落差，待真机标定）

    double tcpForward_ = 0.0;   // 沿小臂(L2)向前的 TCP 距离 (mm)，默认 0 = 无 TCP 前伸
    double tcpDown_ = 0.0;      // 竖直向下的 TCP 物理距离 (mm，绝对正数)，默认 0 = 无 TCP 下探

    // 默认关节软限位（逻辑坐标，单位：角度为 °，Z 为 mm）：
    // 未调用 SetJointLimits 时用宽松范围，避免误拒绝。j1/j2/r 为 ±180°，
    // z 为 ±1000mm（远超实际行程，属安全兜底而非工艺限位）。
    double j1Min_ = -180.0, j1Max_ = 180.0;
    double j2Min_ = -180.0, j2Max_ = 180.0;
    double zMin_ = -1000.0, zMax_ = 1000.0;
    double rMin_ = -180.0, rMax_ = 180.0;

    bool ikSolve(const Pose& target, Joints& sol, bool elbowUp) const;
};