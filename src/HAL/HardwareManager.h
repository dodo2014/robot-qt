#pragma once

#include <QObject>
#include <QTimer>
#include <QVector>

#include <memory>
#include <string>

#include "IMotionCard.h"
#include "IAxisServo.h"
#include "IEndEffector.h"
#include "ICamera.h"
#include "IPuffAlgorithm.h"
#include "AxisMap.h"

// ============================================================
// 硬件组装管理 (HardwareManager)
// 单例。职责：
//   1. 根据 config 的 simulation.*Type 经 HALFactory 创建各硬件实例
//   2. 将每轴换算参数通过 IMotionCard::SetAxisConfig 喂给底层卡
//      （HAL 底层卡代码不主动读配置）
//   3. 对外提供物理单位(mm/度)的调用门面，内部经 AxisConverter
//      换算成脉冲后调用底层卡
//   4. 50ms QTimer 高频轮询底层状态，反向换算后经 Qt 信号广播，
//      供 ManualControlPage 等 UI 刷新位置与状态灯
// ============================================================
class HardwareManager : public QObject
{
    Q_OBJECT

public:
    static HardwareManager& instance();

    // 读取 config 并创建/连接/使能所有硬件。可重复调用（幂等）。
    bool Initialize();
    bool IsInitialized() const;

    // ---- 物理单位门面 (mm / 度) ----
    bool MoveAbs(LogicalAxis axis, double mmOrDeg);
    bool MoveJog(LogicalAxis axis, double mmOrDegPerSec, int direction = 1);
    void StopJog(LogicalAxis axis);
    bool HomeAxis(LogicalAxis axis);
    bool StopAxis(LogicalAxis axis);
    bool HomeAll();
    bool EnableAll();
    bool DisableAll();
    bool EmergencyStop();

    double GetPosition(LogicalAxis axis) const;

    // ---- 每轴速度/限位参数（加载自 config，供 UI 读取与修改） ----
    double GetJogSpeed(LogicalAxis axis) const;
    double GetMaxSpeed(LogicalAxis axis) const;
    bool   SetJogSpeed(LogicalAxis axis, double mmOrDegPerSec);

    // ---- 连接状态 ----
    bool IsMotionCardConnected() const;
    bool IsServoConnected() const;
    QString ConnectionStatus() const;

    // ---- 硬件访问器 ----
    IMotionCard*   motionCard()  const { return motionCard_.get(); }
    IAxisServo*    servoJ2()     const { return servoJ2_.get(); }
    IAxisServo*    servoJ3()     const { return servoJ3_.get(); }
    IEndEffector*  gripper()     const { return gripper_.get(); }
    ICamera*       camera()      const { return camera_.get(); }
    IPuffAlgorithm* algorithm()  const { return algorithm_.get(); }

signals:
    // 高频状态广播 (50ms)，position 已换算为物理单位 (mm/度)
    void stateUpdated(const QVector<MotorStatus>& axes);
    void servoStateUpdated(const QVector<ServoTelemetry>& servos);
    void axisAlarm(int logicalAxis, bool alarm);
    void limitTriggered(int logicalAxis, bool positive, bool negative);
    // 连接状态变化（Initialize 之后或重连后发出）
    void connectionChanged();

private:
    HardwareManager();
    ~HardwareManager() override;
    HardwareManager(const HardwareManager&) = delete;
    HardwareManager& operator=(const HardwareManager&) = delete;

    void PollTick();
    void LoadAxisConfigsFromConfig();
    void JogTick();

    QTimer* pollTimer_ = nullptr;
    QTimer* jogTimer_  = nullptr;
    bool initialized_ = false;

    // 舵机连续点动状态（jogTimer_ 驱动）
    LogicalAxis jogAxis_   = LogicalAxis::J1;
    int         jogDir_    = 1;
    double      jogStep_   = 1.0;
    double      jogSpeed_  = 50.0;

    std::unique_ptr<IMotionCard>    motionCard_;
    std::unique_ptr<IAxisServo>     servoJ2_;
    std::unique_ptr<IAxisServo>     servoJ3_;
    std::unique_ptr<IEndEffector>   gripper_;
    std::unique_ptr<ICamera>        camera_;
    std::unique_ptr<IPuffAlgorithm> algorithm_;

    // 每个逻辑轴从 config 加载的配置快照（LoadAxisConfigsFromConfig 填充）
    QVector<AxisConfig> axisConfigs_;

    // 每个逻辑轴的报警/限位上次状态（用于边沿触发）
    QVector<bool> lastAlarm_;
    QVector<bool> lastLimitPos_;
    QVector<bool> lastLimitNeg_;
};
