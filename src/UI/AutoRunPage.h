#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QTextEdit>
#include <QVector>
#include <QPointer>
#include "HAL/interfaces/ICamera.h"
#include "HAL/interfaces/IMotionCard.h"
#include "HAL/interfaces/IAxisServo.h"
#include "Core/Kinematics.h"
#include "KinematicsHelper.h"
#include "Logic/SequenceWorker.h"   // 槽签名用 WorkerState/PauseReason（Q_ENUM 元类型）

class SequenceWorker;

class AutoRunPage : public QWidget
{
    Q_OBJECT

public:
    explicit AutoRunPage(QWidget* parent = nullptr);

    void SetSequenceWorker(SequenceWorker* worker);

    // 模式互锁入口（下一步 TR-073 接入；本次恒 true，行为不变）
    void SetAutoModeActive(bool active);

private slots:
    // 5 按钮（2026-09-07 语义重构：暂停=减速停保持上下文；停止=立即减速停废弃上下文；
    // 复位→清除报警。原「复位」实为一键回零，已归手动控制页）
    void OnStartOrResumeClicked();
    void OnPauseClicked();
    void OnStopClicked();
    void OnAlarmClearClicked();
    void OnInitClicked();
    void OnEmergencyTriggered();   // 响应全局急停信号（硬件/worker 中断在 MainWindow 触发点完成）
    void OnWorkerStateChanged(SequenceWorker::WorkerState state,
                              SequenceWorker::PauseReason reason);

    void OnFrameReady(const CameraFrame& frame);
    void OnLogMessage(const QString& msg);
    void OnStateUpdated(const QVector<MotorStatus>& axes);
    void OnServoStateUpdated(const QVector<ServoTelemetry>& servos);
    void OnActionStarted(int index, const QString& name);
    void OnSchemeFinished();
    void OnInterrupted(const QString& reason);
    void OnError(const QString& message);
    // 循环进度（TR-091）：每轮开始发一次；index 从 1 起，total < 0 = 无限循环
    void OnCycleChanged(int index, int total);

private:
    void SetupUI();
    void RefreshCoordPanel();
    void RefreshSchemeCombo();
    void showEvent(QShowEvent* event) override;

    // 循环生产控件（TR-091）：读 config 初始化 / 写回 config / 组装 LoopConfig
    void LoadLoopConfigFromConfig();
    void PersistLoopConfig();
    SequenceWorker::LoopConfig BuildLoopConfig() const;
    void ResetCycleLabel();   // 启动前复位进度文案（单轮 / 循环 0/N / 循环 0/∞）

    // 按钮启用的唯一出口（杜绝多处 setEnabled 互相覆盖绕过互锁）
    void UpdateControlsEnabled();
    void SetHint(const QString& text, const QString& color);
    void UpdateStatusByState(SequenceWorker::WorkerState state, SequenceWorker::PauseReason reason);
    void RequestHomeAll();   // 一键回零（不占按钮位，供后续物理按钮/MainWindow 转接）

    QLabel*     m_statusLabel      = nullptr;
    QLabel*     m_coordPanel       = nullptr;
    QComboBox*  m_schemeCombo      = nullptr;
    QTextEdit*  m_logTextEdit      = nullptr;
    QLabel*     m_cameraRgbLabel   = nullptr;
    QLabel*     m_cameraOverlayLabel = nullptr;
    QLabel*     m_hintLabel        = nullptr;

    // 循环生产控件（TR-091，一行排布；布局待真机效果后定）
    QComboBox*  m_loopModeCombo    = nullptr;   // 单轮 / 指定次数 / 无限循环
    QSpinBox*   m_loopCountSpin    = nullptr;   // 1..9999，仅「指定次数」可用
    QCheckBox*  m_loopSafePosCheck = nullptr;   // 「回安全位」（循环间隔）
    QLabel*     m_cycleLabel       = nullptr;   // 进度：单轮 / 循环 3/10 / 循环 3/∞

    QPointer<SequenceWorker> m_worker;   // TR-084：观察者持弱引用，防 worker 析构后悬垂
    bool m_safePosSessionActive = false;   // TR-088：actionStarted 时记录的安全位临时会话标志（完成槽不能直查 safeSession——已被清）

    // 5 按钮（下标分派，不再靠 text.contains 匹配——改文案会静默错配）
    QPushButton*    m_btnStartOrResume = nullptr;
    QPushButton*    m_btnPause         = nullptr;
    QPushButton*    m_btnStop          = nullptr;
    QPushButton*    m_btnAlarmClear    = nullptr;
    QPushButton*    m_btnInit          = nullptr;

    // 互锁状态位（TR-075 已接入）：
    // m_autoModeActive 由 MainWindow 顶栏【手动/自动】开关下发（SetAutoModeActive）；
    // 急停锁存不放本页——全局硬件级状态由 HardwareManager 自持，gate 直读 IsEStopPending()
    bool m_autoModeActive = true;
    QString m_lastError;             // OnError 保存，供清报警提示（避免跨线程读 worker）

    // 关节位置缓存（避免每 50ms 实时读舵机串口，走 stateUpdated/servoStateUpdated 缓存）
    double m_j1 = 0.0, m_j2 = 0.0, m_z = 0.0, m_r = 0.0;
    Kinematics m_kin;
    bool   m_kinParamsLoaded = false;
    double m_kinL1 = 0.0, m_kinL2 = 0.0, m_kinZ0 = 0.0, m_kinH1 = 0.0;
    double m_kinTcpX = 0.0, m_kinTcpY = 0.0, m_kinTcpZ = 0.0;
    QString m_lastCoordText;
};