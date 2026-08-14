#include "MainWindow.h"
#include "AutoRunPage.h"
#include "ManualControlPage.h"
#include "ProcessPage.h"
#include "VisionTestPage.h"
#include "ConfigPage.h"
#include "ToggleSwitch.h"

#include "HAL/core/HardwareManager.h"
#include "spdlog/spdlog.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QDateTime>
#include <QDebug>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("CreamPuffRobot — 机器人控制系统"));
    resize(1480, 980);
    setMinimumSize(1200, 700);
    ApplyGlobalStyle();
    SetupUI();

    // 初始化硬件（仿真/真实卡自动按 config 组装）
    HardwareManager::instance().Initialize();

    clockTimer_ = new QTimer(this);
    connect(clockTimer_, &QTimer::timeout, this, &MainWindow::UpdateClock);
    clockTimer_->start(1000);
    UpdateClock();

    qDebug() << "[MainWindow] UI initialized";
}

MainWindow::~MainWindow() = default;

void MainWindow::ApplyGlobalStyle()
{
    qApp->setStyleSheet(R"(
        * {
            font-family: 'Segoe UI', 'Microsoft YaHei', sans-serif;
        }
        QMainWindow {
            background: #1a1e24;
        }
        QWidget {
            color: #b8cce3;
            font-size: 13px;
        }
        QPushButton {
            border: none;
            border-radius: 10px;
            font-weight: 600;
            font-size: 15px;
            padding: 10px 18px;
            color: white;
        }
        QPushButton:hover {
            /* individual colors handled per class */
        }

        QLineEdit, QSpinBox, QDoubleSpinBox {
            background: #111a22;
            border: 1px solid #3f4e5e;
            color: #dbe6f0;
            padding: 4px 8px;
            border-radius: 6px;
            font-size: 13px;
        }
        QComboBox {
            background: #111a22;
            border: 1px solid #3f4e5e;
            color: #dbe6f0;
            padding: 4px 24px 4px 8px;
            border-radius: 6px;
            font-size: 13px;
            min-height: 22px;
        }
        QComboBox::drop-down {
            subcontrol-origin: padding;
            subcontrol-position: center right;
            width: 18px;
            border: none;
        }
        QComboBox::down-arrow {
            width: 0;
            height: 0;
            border-top: 5px solid #8da3bb;
            border-left: 4px solid transparent;
            border-right: 4px solid transparent;
            margin-right: 3px;
        }
        QComboBox QAbstractItemView {
            background: #1a2129;
            color: #dbe6f0;
            border: none;
            outline: 1px solid #3f4e5e;
            selection-background-color: #2f6f9f;
        }
        QTableWidget {
            background: #1a2129;
            border: 1px solid #33404d;
            border-radius: 12px;
            gridline-color: #2a3542;
            color: #c8d6e5;
            font-size: 14px;
        }
        QTableWidget::item {
            padding: 8px 12px;
            border-bottom: 1px solid #2a3542;
        }
        QHeaderView::section {
            background: #242e3a;
            color: #9bb3cf;
            padding: 10px 12px;
            border: none;
            border-bottom: 1px solid #33404d;
            font-weight: 500;
            font-size: 14px;
        }
        QScrollBar:vertical {
            background: #1b1f26;
            width: 8px;
            border-radius: 4px;
        }
        QScrollBar::handle:vertical {
            background: #3a424e;
            border-radius: 4px;
            min-height: 30px;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0;
        }
    )");
}

void MainWindow::SetupUI()
{
    auto* centralWidget = new QWidget(this);
    centralWidget->setObjectName("centralWidget");
    setCentralWidget(centralWidget);

    auto* mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(0);

    // Main window container (模仿 .main-window 的圆角 + shadow)
    auto* windowContainer = new QWidget();
    windowContainer->setObjectName("mainWindowContainer");
    windowContainer->setStyleSheet(R"(
        #mainWindowContainer {
            background: #23272e;
            border-radius: 18px;
            border: 1px solid #3b414b;
        }
    )");

    auto* containerLayout = new QVBoxLayout(windowContainer);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(0);

    // Top bar
    containerLayout->addWidget(CreateTopBar());

    // Main body: nav + content
    auto* bodyWidget = new QWidget();
    auto* bodyLayout = new QHBoxLayout(bodyWidget);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    bodyLayout->addWidget(CreateNavSidebar());

    stack_ = new QStackedWidget();
    stack_->setStyleSheet("background: #262c34;");

    autoRunPage_   = new AutoRunPage();
    manualPage_    = new ManualControlPage();
    processPage_   = new ProcessPage();
    visionTestPage_ = new VisionTestPage();
    configPage_    = new ConfigPage();

    stack_->addWidget(autoRunPage_);
    stack_->addWidget(manualPage_);
    stack_->addWidget(processPage_);
    stack_->addWidget(visionTestPage_);
    stack_->addWidget(configPage_);
    stack_->setCurrentIndex(0);

    bodyLayout->addWidget(stack_, 1);

    containerLayout->addWidget(bodyWidget, 1);

    mainLayout->addWidget(windowContainer);
}

QWidget* MainWindow::CreateTopBar()
{
    auto* topBar = new QWidget();
    topBar->setFixedHeight(62);
    topBar->setStyleSheet(R"(
        background: #1a2028;
        border-bottom: 1px solid #343b45;
        border-top-left-radius: 18px;
        border-top-right-radius: 18px;
    )");

    auto* layout = new QHBoxLayout(topBar);
    layout->setContentsMargins(24, 0, 24, 0);

    // ---- Left section ----
    auto* leftSection = new QHBoxLayout();
    leftSection->setSpacing(12);

    auto* titleLabel = new QLabel(QStringLiteral("机器人控制系统"));
    titleLabel->setStyleSheet(R"(
        font-size: 20px; font-weight: 700; color: #d6e6ff;
        background: transparent; border: none;
        letter-spacing: 0.5px;
    )");

    auto* versionLabel = new QLabel(QStringLiteral("V1.0"));
    versionLabel->setStyleSheet(R"(
        font-weight: 400; color: #7c9fc0; font-size: 14px;
        background: transparent; border: none;
        margin-left: 6px;
    )");

    leftSection->addWidget(titleLabel);
    leftSection->addWidget(versionLabel);
    leftSection->addStretch();

    // ---- Center section: mode toggle (two buttons as toggle group) ----
    // ---- Center section: mode toggle (自定义滑块方案) ----
    auto* centerLayout = new QHBoxLayout();
    centerLayout->setSpacing(10);

    // 左侧：手动 标签
    auto* manualLabel = new QLabel(QStringLiteral("手动"));
    manualLabel->setStyleSheet("color: #7ed67e; font-size: 15px; font-weight: 600; background: transparent; border: none;");

    // 中间：我们自己画的滑块
    auto* toggle = new ToggleSwitch();

    // 右侧：自动 标签
    auto* autoLabel = new QLabel(QStringLiteral("自动"));
    autoLabel->setStyleSheet("color: #8da3bb; font-size: 15px; font-weight: 600; background: transparent; border: none;");

    // 连接滑块的切换信号，实现文字颜色的联动变色
    connect(toggle, &ToggleSwitch::toggled, this, [this, manualLabel, autoLabel](bool isAuto) {
        if (isAuto) {
            manualLabel->setStyleSheet("color: #8da3bb; font-size: 15px; font-weight: 600; background: transparent; border: none;");
            autoLabel->setStyleSheet("color: #7ed67e; font-size: 15px; font-weight: 600; background: transparent; border: none;");
        }
        else {
            manualLabel->setStyleSheet("color: #7ed67e; font-size: 15px; font-weight: 600; background: transparent; border: none;");
            autoLabel->setStyleSheet("color: #8da3bb; font-size: 15px; font-weight: 600; background: transparent; border: none;");
        }
        // 调用你之前的业务逻辑函数
        OnModeToggled(isAuto);
        });

    centerLayout->addWidget(manualLabel);
    centerLayout->addWidget(toggle);
    centerLayout->addWidget(autoLabel);

    // ---- Right section ----
    auto* rightLayout = new QHBoxLayout();
    rightLayout->setSpacing(10);

    auto* permBadge = new QLabel(QStringLiteral("操作员"));
    permBadge->setStyleSheet(R"(
        background: #2f6f9f; color: white; padding: 1px 10px;
        border-radius: 12px; font-size: 12px; font-weight: 600;
        border: none;
    )");

    auto* ledHeartbeat = new QLabel();
    ledHeartbeat->setFixedSize(14, 14);
    ledHeartbeat->setStyleSheet(R"(
        background: #3bff7b; border-radius: 7px;
    )");

    auto* heartbeatLabel = new QLabel(QStringLiteral("心跳"));
    heartbeatLabel->setStyleSheet("color: #b0b9c7; font-size: 14px; font-weight: 500; background: transparent; border: none;");

    auto* ledEmergency = new QLabel();
    ledEmergency->setFixedSize(14, 14);
    ledEmergency->setStyleSheet(R"(
        background: #ff3b3b; border-radius: 7px;
    )");

    auto* emergencyLabel = new QLabel(QStringLiteral("急停"));
    emergencyLabel->setStyleSheet("color: #b0b9c7; font-size: 14px; font-weight: 500; background: transparent; border: none;");

    auto* statusItem1 = new QWidget();
    auto* si1 = new QHBoxLayout(statusItem1);
    si1->setContentsMargins(0, 0, 0, 0);
    si1->setSpacing(6);
    si1->addWidget(ledHeartbeat);
    si1->addWidget(heartbeatLabel);

    auto* statusItem2 = new QWidget();
    auto* si2 = new QHBoxLayout(statusItem2);
    si2->setContentsMargins(0, 0, 0, 0);
    si2->setSpacing(6);
    si2->addWidget(ledEmergency);
    si2->addWidget(emergencyLabel);

    clockLabel_ = new QLabel();
    clockLabel_->setStyleSheet(R"(
        color: #b8cce3; font-size: 15px; font-weight: 500;
        font-family: 'Consolas', monospace; letter-spacing: 0.5px;
        min-width: 80px; text-align: right; background: transparent; border: none;
    )");

    rightLayout->addStretch();
    rightLayout->addWidget(permBadge);
    rightLayout->addWidget(statusItem1);
    rightLayout->addWidget(statusItem2);
    rightLayout->addWidget(clockLabel_);

    // Build top bar
    auto* leftWidget = new QWidget();
    leftWidget->setLayout(leftSection);
    auto* centerWidget = new QWidget();
    centerWidget->setLayout(centerLayout);
    auto* rightWidget = new QWidget();
    rightWidget->setLayout(rightLayout);

    layout->addWidget(leftWidget, 1);
    layout->addWidget(centerWidget, 0);
    layout->addWidget(rightWidget, 1);

    return topBar;
}

QWidget* MainWindow::CreateNavSidebar()
{
    auto* navWidget = new QWidget();
    navWidget->setFixedWidth(160);
    navWidget->setStyleSheet("background: #1b1f26; border-right: 1px solid #2f3640;");

    auto* navLayout = new QVBoxLayout(navWidget);
    navLayout->setContentsMargins(8, 20, 8, 12);
    navLayout->setSpacing(4);

    navGroup_ = new QButtonGroup(this);
    navGroup_->setExclusive(true);

    struct NavInfo { QString text; };
    QVector<NavInfo> navItems = {
        { QStringLiteral("🌟 自动运行") },
        { QStringLiteral("\xF0\x9F\x9B\xA0\xEF\xB8\x8F 手动控制") },
        { QStringLiteral("\xF0\x9F\x93\x9D 工艺流程") },
        { QStringLiteral("\xF0\x9F\x8E\xA5 视觉检测") },
        { QStringLiteral("\xE2\x9A\x99\xEF\xB8\x8F 设备配置") },
    };

    for (int i = 0; i < navItems.size(); ++i)
    {
        auto* btn = new QPushButton(navItems[i].text);
        btn->setCheckable(true);
        btn->setObjectName(QStringLiteral("navBtn%1").arg(i));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(R"(
            QPushButton {
                background: transparent; color: #a5b1c2;
                font-weight: 600; font-size: 16px;
                padding: 14px 16px; border-radius: 10px;
                text-align: left; border: none;
            }
            QPushButton:hover {
                background: #2b313b; color: #d3deed;
            }
            QPushButton:checked {
                background: #2f6f9f; color: white;
            }
        )");
        navGroup_->addButton(btn, i);
        navLayout->addWidget(btn);
    }

    connect(navGroup_, &QButtonGroup::idClicked, this, &MainWindow::OnNavButtonClicked);

    navLayout->addStretch();

    auto* footer = new QLabel(QStringLiteral("— 系统就绪 —"));
    footer->setAlignment(Qt::AlignCenter);
    footer->setStyleSheet("color: #4a5a6e; font-size: 12px; background: transparent; border: none; padding-top: 12px; border-top: 1px solid #2f3640;");
    navLayout->addWidget(footer);

    auto* firstBtn = qobject_cast<QPushButton*>(navGroup_->button(0));
    if (firstBtn) firstBtn->setChecked(true);

    return navWidget;
}

void MainWindow::OnNavButtonClicked(int index)
{
    if (index >= 0 && index < stack_->count())
    {
        stack_->setCurrentIndex(index);
        SPDLOG_INFO("[MainWindow] 导航切换到页面 {}", index);
        qDebug() << "[MainWindow] Navigated to page" << index;
    }
}

void MainWindow::OnModeToggled(bool autoMode)
{
    SPDLOG_INFO("[MainWindow] 模式切换为 {}", autoMode ? "自动" : "手动");
    qDebug() << "[全局] 模式切换为:" << (autoMode ? "自动" : "手动");
}

void MainWindow::UpdateClock()
{
    if (clockLabel_)
        clockLabel_->setText(QDateTime::currentDateTime().toString("HH:mm:ss"));
}
