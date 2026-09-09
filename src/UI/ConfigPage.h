#pragma once

#include <QWidget>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QListWidget>
#include <QLabel>
#include <QComboBox>

class QLineEdit;
class QCheckBox;
class QShowEvent;

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
    // 安全位字段刷新（2026-09-09 TR-079）：示教由手动控制页直接写 config（按设计不 emit
    // paramsChanged），配置页构造时读的初值不会自动更新 → 每次页面显示时重新读 config 回填，
    // 否则"示教完切到配置页看到的还是旧值，重启才对"。
    void RefreshSafePosFields();
    void showEvent(QShowEvent* event) override;

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

    // 安全位输入控件（控件 + config 路径），供 RefreshSafePosFields 回填
    QVector<QPair<QLineEdit*, QByteArray>> safePosInputs_;
    QCheckBox* safePosEnabledChk_ = nullptr;
};
