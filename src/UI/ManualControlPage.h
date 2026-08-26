#pragma once

#include <QWidget>
#include <QVector>
#include <QLabel>
#include <QDoubleSpinBox>

class QPushButton;

#include "HAL/interfaces/IMotionCard.h"
#include "HAL/interfaces/IAxisServo.h"
#include "Core/Kinematics.h"

class ManualControlPage : public QWidget
{
    Q_OBJECT

public:
    explicit ManualControlPage(QWidget* parent = nullptr);

private slots:
    void OnGlobalEnable();
    void OnGlobalDisable();
    void OnGlobalHome();
    void OnJogMinus(int axis);
    void OnJogPlus(int axis);
    void OnJogStop(int axis);
    void OnGoClicked(int axis);
    void OnStopAxis(int axis);
    void OnHomeAxis(int axis);
    void OnStateUpdated(const QVector<MotorStatus>& axes);
    void OnServoStateUpdated(const QVector<ServoTelemetry>& servos);
    void OnConnectionChanged();
    void OnEnableStateChanged();
    void OnAxisAlarm(int axis, bool alarm);
    void OnLimitTriggered(int axis, bool positive, bool negative);
    void OnSoftLimit(int axis, bool positive);
    void OnAxisMoveFinished(int axis);

private:
    void SetupUI();
    QWidget* CreateAxisRow(int axis, const QString& label, const QString& typeIcon,
                           double speed, double accel, double decel,
                           const QString& pos, double target, bool isGripper = false, bool isExtrude = false);
    void RefreshStatusDot(int axis);
    void SetHint(const QString& text, const QString& color = QString());
    void RefreshSoftLimitHint();
    void RefreshCoordPanel();

    QVector<QLabel*> posLabels_;
    QVector<QLabel*> statusDots_;
    QVector<QDoubleSpinBox*> targetSpins_;
    QVector<QDoubleSpinBox*> speedSpins_;
    QVector<QPushButton*> goButtons_;
    QLabel* connStatusLabel_ = nullptr;
    QLabel* hintLabel_ = nullptr;

    QVector<bool> alarmState_;
    QVector<bool> limitState_;
    QVector<int> softLimitDir_;   // 1=到达最大限位, -1=到达最小限位, 0=正常
    QVector<QString> alarmDetail_; // 每轴告警详情（驱动器报警/跟随误差/急停/硬软限位），tooltip 用
    QVector<bool> enabledState_;
    QVector<bool> runningState_;
    QVector<bool> homeDoneState_;
    QVector<bool> homingAxes_;   // 每轴回零进行中标记：OnHomeAxis/OnGlobalHome 置位，OnAxisMoveFinished(超时/到位) 或 OnStopAxis 清除
    bool servoAllOnlinePrev_ = true;   // 上次遥测时两舵机是否全部在线（检测离线/重连边沿，驱动提示）

    QVector<QLabel*> coordLabels_;   // 末端坐标面板 X/Y/Z/R（FK 实时刷新，曾为静态假数据）
    // 坐标面板 FK：关节位缓存 + 运动学参数变化检测缓存（对齐 AutoRunPage 模式，
    // 避免每 50ms 重建 Kinematics 刷日志）
    double cachedJ1_ = 0, cachedJ2_ = 0, cachedZ_ = 0, cachedR_ = 0;
    Kinematics coordKin_;
    bool coordKinLoaded_ = false;
    double kinL1_ = 0, kinL2_ = 0, kinZ0_ = 0, kinH1_ = 0;
    double kinTcpX_ = 0, kinTcpY_ = 0, kinTcpZ_ = 0;
};
