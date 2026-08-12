#pragma once

#include <array>

// ============================================================
// 逻辑轴映射 (AxisMap)
// 将界面逻辑轴 (J1/J2/Z/R/Gripper/Extruder) 映射到具体硬件：
//   - 运动控制卡上的脉冲轴 (axisId，即 BoPai 卡 axis 号)
//   - 串口总线舵机 (servo index，总线 ID)
//
// 默认映射与 BoPai 实物接线一致：
//   J1 → 卡轴1(0), J2 → 舵机0, Z → 卡轴2(1), R → 舵机1,
//   夹爪 → 卡轴4(3), 挤出 → 卡轴3(2)
//
// 注意：本表在 HardwareManager::Initialize 时由 config 的
// `portId` 覆盖注入（卡轴 index = 卡 axis 号，舵机 index = 总线 ID），
// 故默认值仅作 config 缺失时的兜底。
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
    // 逻辑轴 → 硬件绑定（初始化后由 config portId 覆盖）
    static AxisBinding Get(LogicalAxis axis)
    {
        return Table()[static_cast<int>(axis)];
    }

    // 运行时注入：Card 轴 index 为卡 axis 号，Servo 轴 index 为总线 ID。
    // 仅在 Initialize 阶段调用，之后表保持只读（PollTick 线程安全）。
    static void SetBinding(LogicalAxis axis, AxisBinding::Type type, int index)
    {
        Table()[static_cast<int>(axis)] = AxisBinding{ type, index };
    }

    // 恢复默认（config 缺失时兜底用）
    static void Reset()
    {
        Table() = DefaultTable();
    }

private:
    using TableT = std::array<AxisBinding, static_cast<int>(LogicalAxis::Count)>;

    static const TableT& DefaultTable()
    {
        static const TableT kDefault = {
            /* J1 */       AxisBinding{ AxisBinding::Type::Card,  0 },
            /* J2 */       AxisBinding{ AxisBinding::Type::Servo, 0 },
            /* Z  */       AxisBinding{ AxisBinding::Type::Card,  1 },
            /* R  */       AxisBinding{ AxisBinding::Type::Servo, 1 },
            /* Gripper */  AxisBinding{ AxisBinding::Type::Card,  3 },
            /* Extruder */ AxisBinding{ AxisBinding::Type::Card,  2 },
        };
        return kDefault;
    }

    static TableT& Table()
    {
        static TableT table = DefaultTable();
        return table;
    }
};
