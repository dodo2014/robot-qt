#pragma once

#include <QMainWindow>
#include <QLabel>
#include <QStackedWidget>
#include <QTimer>
#include <QButtonGroup>
#include <QPushButton>

class AutoRunPage;
class ManualControlPage;
class ProcessPage;
class VisionTestPage;
class ConfigPage;

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

    QLabel*              clockLabel_     = nullptr;
    QLabel*              modeLabelLeft_  = nullptr;
    QLabel*              modeLabelRight_ = nullptr;
    QStackedWidget*      stack_          = nullptr;
    QButtonGroup*        navGroup_       = nullptr;
    QTimer*              clockTimer_     = nullptr;

    AutoRunPage*         autoRunPage_    = nullptr;
    ManualControlPage*   manualPage_     = nullptr;
    ProcessPage*         processPage_    = nullptr;
    VisionTestPage*      visionTestPage_ = nullptr;
    ConfigPage*          configPage_     = nullptr;
};
