#pragma once

#include <QWidget>

class ProcessPage : public QWidget
{
    Q_OBJECT

public:
    explicit ProcessPage(QWidget* parent = nullptr);

private slots:
    void OnNewScheme();
    void OnConfirmSwitch();
    void OnStepExecute();
    void OnNewAction();
    void OnDeleteAction();
    void OnAddPoint();
    void OnDeletePoint();
    void OnMoveUp();
    void OnMoveDown();
    void OnTeachRead();
    void OnSaveAction();

private:
    void SetupUI();
};
