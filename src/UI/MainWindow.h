#pragma once

#include <QMainWindow>
#include <QLabel>
#include <QStackedWidget>
#include <QTimer>
#include <QButtonGroup>
#include <QPushButton>
#include <QThread>
#include <QPointer>

class AutoRunPage;
class ManualControlPage;
class ProcessPage;
class VisionTestPage;
class ConfigPage;
class SequenceWorker;
class ToggleSwitch;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void OnNavButtonClicked(int index);
    void OnModeToggled(bool autoMode);
    void UpdateClock();

private:
    void SetupUI();
    QWidget* CreateTopBar();
    QWidget* CreateNavSidebar();
    void ApplyGlobalStyle();
    void ShutdownWorker();
    // 模式互锁（TR-075）：模式态唯一出口（标签高亮+导航白名单+强制跳页+下发各页+切自动清理残留）
    void ApplyModeState(bool autoMode);
    // 拒绝切换时开关回弹：QSignalBlocker 阻断 toggled，防"拒绝→回弹→再拒绝"递归
    void RevertModeToggle(bool backToAuto);

    QLabel*              clockLabel_     = nullptr;
    QLabel*              modeLabelLeft_  = nullptr;
    QLabel*              modeLabelRight_ = nullptr;
    // 顶栏【手动/自动】模式开关与两侧标签（TR-075 提升为成员：互锁需程序化回弹与高亮）
    QLabel*              modeManualLabel_ = nullptr;
    QLabel*              modeAutoLabel_   = nullptr;
    ToggleSwitch*        modeToggle_      = nullptr;
    QStackedWidget*      stack_          = nullptr;
    QButtonGroup*        navGroup_       = nullptr;
    QTimer*              clockTimer_     = nullptr;

    AutoRunPage*         autoRunPage_    = nullptr;
    ManualControlPage*   manualPage_     = nullptr;
    ProcessPage*         processPage_    = nullptr;
    VisionTestPage*      visionTestPage_ = nullptr;
    ConfigPage*          configPage_     = nullptr;

    QPointer<SequenceWorker> sequenceWorker_;   // TR-084：QPointer——worker 由 deleteLater 销毁时自动置空，防退出悬垂
    QThread*             workerThread_   = nullptr;
    bool                 workerShutdown_ = false;
    bool                 lastHomed_      = false;   // homeStateChanged 上升沿判定（电平语义，单轴重回零会重复 emit true）
};
