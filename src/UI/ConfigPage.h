#pragma once

#include <QWidget>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QListWidget>
#include <QLabel>
#include <QComboBox>

class ConfigPage : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigPage(QWidget* parent = nullptr);

signals:
    void paramsChanged();

private slots:
    void OnTabClicked(int index);

private:
    void SetupUI();

    QWidget* CreateTab1Comm();
    QWidget* CreateTab2Kinematics();
    QWidget* CreateTab3Vision();
    QWidget* CreateTab4TCP();
    QWidget* CreateElecMapTab();

    QButtonGroup*   tabGroup_   = nullptr;
    QStackedWidget* tabStack_   = nullptr;
    QLabel*         axisTitle_  = nullptr;
    QListWidget*    axisList_   = nullptr;
    QStackedWidget* transStack_ = nullptr;
};
