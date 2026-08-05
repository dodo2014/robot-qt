#pragma once

// ============================================================
// 逻辑轴映射 (AxisMap)
// 将界面逻辑轴 (J1/J2/Z/R/Gripper/Extruder) 映射到具体硬件：
//   - 运动控制卡上的脉冲轴 (axisId)
//   - 串口总线舵机 (servo index)
// 默认 SCARA 映射与 bopai 实物一致：
//   J1 → 卡轴0, J2 → 舵机0, Z → 卡轴2, R → 舵机1,
//   夹爪 → 卡轴3, 挤出 → 卡轴1
// ============================================================

enum class LogicalAxis
{
    J1       = 0,   // 轴1 大臂
    J2       = 1,   // 轴2 小臂 (舵机)
    Z        = 2,   // 轴3 Z 升降
    R        = 3,   // 轴4 翻转 (舵机)
    Gripper  = 4,   // 轴5 夹爪
    Extruder = 5,   // 轴6 挤出
    Count
};

struct AxisBinding
{
    enum class Type { Card, Servo, None };

    Type type  = Type::None;
    int  index = -1;
};

class AxisMap
{
public:
    static AxisBinding Get(LogicalAxis axis)
    {
        switch (axis)
        {
            case LogicalAxis::J1:       return { AxisBinding::Type::Card,  0 };
            case LogicalAxis::J2:       return { AxisBinding::Type::Servo, 0 };
            case LogicalAxis::Z:        return { AxisBinding::Type::Card,  2 };
            case LogicalAxis::R:        return { AxisBinding::Type::Servo, 1 };
            case LogicalAxis::Gripper:  return { AxisBinding::Type::Card,  3 };
            case LogicalAxis::Extruder: return { AxisBinding::Type::Card,  1 };
            default:                    return { AxisBinding::Type::None, -1 };
        }
    }
};
