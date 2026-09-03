#pragma once

#include <QWidget>
#include <QString>
#include <QListWidget>
#include <QStackedWidget>
#include <QTableWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QLabel>

class SequenceWorker;
class QPushButton;

class ProcessPage : public QWidget
{
    Q_OBJECT

public:
    explicit ProcessPage(QWidget* parent = nullptr);

    void SetSequenceWorker(SequenceWorker* worker);

private slots:
    void OnNewScheme();
    void OnDeleteScheme();
    void OnConfirmSwitch();
    void OnStepExecute();
    void OnRunSelectedAction();
    void OnNewAction();
    void OnEditAction();
    void OnDeleteAction();
    void OnAddPoint();
    void OnDeletePoint();
    void OnMoveUp();
    void OnMoveDown();
    void OnTeachRead();
    void OnSaveAction();
    void OnSchemeNameEdited();
    void OnMoveActionUp();
    void OnMoveActionDown();

private:
    void SetupUI();
    void ApplySpeedPercentToCurrentAction(int v);
    void ApplyGripperTargetToCurrentAction(double v);
    void RefreshGripperLimitRange();
    // 执行状态标签统一入口：text 已含状态符号，color 为前景色
    void SetStatusText(const QString& text, const QString& color);
    // 单步/自动执行进行中：此时禁止切换/删除方案（worker 跑的是启动时值拷贝的 scheme）
    bool IsExecutionActive() const;
    void RefreshActionList();
    void RefreshActionDetail(int idx);
    void RefreshSchemeCombo();

    int m_currentSchemeIdx = -1;
    int m_currentActionIdx = -1;

    QLineEdit* m_schemeNameEdit = nullptr;
    QComboBox* m_schemeCombo = nullptr;

    QListWidget* m_actionList = nullptr;

    QStackedWidget* m_detailStack = nullptr;
    QLabel* m_currentActionLabel = nullptr;
    QLabel* m_statusLabel = nullptr;

    QWidget* m_speedPercentRow = nullptr;
    QSpinBox* m_speedPercentSpin = nullptr;
    QSlider* m_speedPercentSlider = nullptr;

    QTableWidget* m_pointTable = nullptr;

    QComboBox* m_visionTypeCombo = nullptr;
    QLineEdit* m_exposureEdit = nullptr;
    QLineEdit* m_templateEdit = nullptr;
    QLineEdit* m_thresholdEdit = nullptr;

    QLineEdit* m_extrudeAmountEdit = nullptr;
    QLineEdit* m_extrudeSpeedEdit = nullptr;
    QLineEdit* m_suckBackAmountEdit = nullptr;
    QLineEdit* m_suckBackSpeedEdit = nullptr;

    QSpinBox* m_delaySpin = nullptr;

    QComboBox* m_gripperCombo = nullptr;
    QDoubleSpinBox* m_gripperTargetSpin = nullptr;
    QLabel* m_gripperUnitLabel = nullptr;
    QLabel* m_gripperHintLabel = nullptr;
    // 按方向各记一份最近行程值（mm）：切换下拉时恢复用户自己的数，不覆盖已填值
    double m_lastOpenTarget = -3.0;
    double m_lastCloseTarget = 0.0;

    SequenceWorker* m_worker = nullptr;
    QPushButton* m_runSelectedBtn = nullptr;
    QPushButton* m_stepBtn = nullptr;
    bool m_stepActive = false;
    // 会话终态（完成/已停止/出错）：StartExecution 在 emit 终态信号后还会补发 stateChanged("空闲")，
    // 不做保留的话终态会被紧随其后的「空闲」覆盖。新会话启动时清空。
    QString m_finalStatus;
    QString m_finalColor;
    void ResetStepSession();
};
