#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QTextEdit>
#include <QVector>
#include "HAL/interfaces/ICamera.h"
#include "HAL/interfaces/IMotionCard.h"
#include "HAL/interfaces/IAxisServo.h"
#include "Core/Kinematics.h"
#include "KinematicsHelper.h"

class SequenceWorker;

class AutoRunPage : public QWidget
{
    Q_OBJECT

public:
    explicit AutoRunPage(QWidget* parent = nullptr);

    void SetSequenceWorker(SequenceWorker* worker);

private slots:
    void OnStartClicked();
    void OnResetClicked();
    void OnStopClicked();
    void OnInitClicked();
    void OnEmergencyClicked();

    void OnFrameReady(const CameraFrame& frame);
    void OnLogMessage(const QString& msg);
    void OnStateUpdated(const QVector<MotorStatus>& axes);
    void OnServoStateUpdated(const QVector<ServoTelemetry>& servos);
    void OnActionStarted(int index, const QString& name);
    void OnSchemeFinished();
    void OnInterrupted(const QString& reason);
    void OnError(const QString& message);

private:
    void SetupUI();
    void RefreshCoordPanel();
    void RefreshSchemeCombo();
    void showEvent(QShowEvent* event) override;

    QLabel*     m_statusLabel      = nullptr;
    QLabel*     m_coordPanel       = nullptr;
    QComboBox*  m_schemeCombo      = nullptr;
    QTextEdit*  m_logTextEdit      = nullptr;
    QLabel*     m_cameraRgbLabel   = nullptr;
    QLabel*     m_cameraOverlayLabel = nullptr;
    QLabel*     m_hintLabel        = nullptr;

    SequenceWorker* m_worker       = nullptr;
    QPushButton*    m_btnStart     = nullptr;

    // 关节位置缓存（避免每 50ms 实时读舵机串口，走 stateUpdated/servoStateUpdated 缓存）
    double m_j1 = 0.0, m_j2 = 0.0, m_z = 0.0, m_r = 0.0;
    Kinematics m_kin;
    bool   m_kinParamsLoaded = false;
    double m_kinL1 = 0.0, m_kinL2 = 0.0, m_kinZ0 = 0.0, m_kinH1 = 0.0;
    double m_kinTcpX = 0.0, m_kinTcpY = 0.0, m_kinTcpZ = 0.0;
    QString m_lastCoordText;
};