#pragma once

#include <QWidget>
#include <QVector>
#include <QLabel>
#include <QDoubleSpinBox>

class QPushButton;

#include "HAL/IMotionCard.h"
#include "HAL/IAxisServo.h"

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
    QVector<bool> enabledState_;
    QVector<bool> runningState_;
    QVector<bool> homeDoneState_;
    bool homingPending_ = false;
};
