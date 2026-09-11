#pragma once

#include <QWidget>
#include <QVector>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QPointer>

class QPushButton;
class SequenceWorker;

// TR-084：worker_ 为 QPointer，内联 setter 的赋值需完整类型（QPointer::operator=(T*)）
#include "Logic/SequenceWorker.h"
#include "HAL/interfaces/IMotionCard.h"
#include "HAL/interfaces/IAxisServo.h"
#include "Core/Kinematics.h"

class ManualControlPage : public QWidget
{
    Q_OBJECT

public:
    explicit ManualControlPage(QWidget* parent = nullptr);

    // 注入执行引擎（2026-09-07）：「回安全位」经 SequenceWorker::RunSafePos 执行
    void SetSequenceWorker(SequenceWorker* worker) { worker_ = worker; }

private slots:
    void OnGlobalEnable();
    void OnGlobalDisable();
    void OnGlobalHome();
    void OnGoSafePos();      // 回安全位（先抬Z再水平走，走 worker 临时方案）
    void OnTeachSafePos();   // 示教安全位：读当前四轴关节角写入 config
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
    // 一键回零前的残留会话处置（TR-083）：Paused 强制终止后放行回零；Fault 不动锁存仅预告；
    // Running 拒绝回零。返回 false = 已给提示并拒绝回零；hintPrefix/hintColor 供回零提示拼接。
    bool PrepareHomingSession(QString& hintPrefix, QString& hintColor);
    void ResetAxisStates();   // 急停后清除残留状态数组（曾导致轴1/3 误显示"限位"而非"未使能"）

    QVector<QLabel*> posLabels_;
    QVector<QLabel*> statusDots_;
    QVector<QDoubleSpinBox*> targetSpins_;
    QVector<QDoubleSpinBox*> speedSpins_;
    QVector<QPushButton*> goButtons_;
    QLabel* connStatusLabel_ = nullptr;
    QLabel* hintLabel_ = nullptr;

    QPointer<SequenceWorker> worker_;   // TR-084：执行引擎（回安全位用），弱引用防悬垂

    QVector<bool> alarmState_;
    QVector<bool> limitState_;
    QVector<int> softLimitDir_;   // 1=到达最大限位, -1=到达最小限位, 0=正常
    QVector<QString> alarmDetail_; // 每轴告警详情（驱动器报警/跟随误差/急停/硬软限位），tooltip 用
    QVector<bool> enabledState_;
    QVector<bool> runningState_;
    QVector<bool> homeDoneState_;
    QVector<bool> homingAxes_;   // 每轴回零进行中标记：OnHomeAxis/OnGlobalHome 置位，OnAxisMoveFinished(超时/到位) 或 OnStopAxis 清除
    bool servoAllOnlinePrev_ = true;   // 上次遥测时两舵机是否全部在线（检测离线/重连边沿，驱动提示）
    bool estopClearPending_ = false;   // TR-088：estopCleared 已发但"回零完成"提示未消费——期间的回零全完成应显示绿字③而非"回零完成"

    QVector<QLabel*> coordLabels_;   // 末端坐标面板 X/Y/Z/R（FK 实时刷新，曾为静态假数据）
    // 坐标面板 FK：关节位缓存 + 运动学参数变化检测缓存（对齐 AutoRunPage 模式，
    // 避免每 50ms 重建 Kinematics 刷日志）
    double cachedJ1_ = 0, cachedJ2_ = 0, cachedZ_ = 0, cachedR_ = 0;
    Kinematics coordKin_;
    bool coordKinLoaded_ = false;
    double kinL1_ = 0, kinL2_ = 0, kinZ0_ = 0, kinH1_ = 0;
    double kinTcpX_ = 0, kinTcpY_ = 0, kinTcpZ_ = 0;
};
