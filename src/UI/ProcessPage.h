#pragma once

#include <QWidget>
#include <QString>
#include <QListWidget>
#include <QStackedWidget>
#include <QTableWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QSlider>
#include <QLabel>

class SequenceWorker;

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

    SequenceWorker* m_worker = nullptr;
    bool m_stepActive = false;
    void ResetStepSession();
};
