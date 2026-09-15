#include "AutoRunPage.h"
#include "SequenceWorker.h"
#include "ProcessManager.h"

#include "HAL/core/HardwareManager.h"
#include "HAL/interfaces/ICamera.h"
#include "HAL/camera/FrameConverter.h"
#include "Core/Kinematics.h"
#include "Config/ConfigManager.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QTextEdit>
#include <QSizePolicy>
#include <QScrollBar>
#include <QPixmap>
#include <QImage>
#include <QDateTime>
#include <QShowEvent>
#include <QMessageBox>
#include <QSignalBlocker>
#include "spdlog/spdlog.h"

AutoRunPage::AutoRunPage(QWidget* parent)
    : QWidget(parent)
{
    SetupUI();
    LoadLoopConfigFromConfig();   // TR-091：循环控件按上次选择初始化（在 SetupUI 之后，控件已建）

    // HardwareManager 信号在构造函数即连接：连接状态/遥测/帧不依赖 m_worker。
    // 关键：connectionChanged 只在 Initialize 完成与运行中状态边沿时发一次，
    // 若等 SetSequenceWorker（MainWindow 在 HardwareManager::Initialize 之后才调用）
    // 会错过启动那次连接状态日志（2026-09-02 真机反馈：自动运行页日志框无连接事件）
    connect(&HardwareManager::instance(), &HardwareManager::stateUpdated,
            this, &AutoRunPage::OnStateUpdated);
    connect(&HardwareManager::instance(), &HardwareManager::servoStateUpdated,
            this, &AutoRunPage::OnServoStateUpdated);
    connect(&HardwareManager::instance(), &HardwareManager::frameReady,
            this, &AutoRunPage::OnFrameReady);
    connect(&HardwareManager::instance(), &HardwareManager::connectionChanged,
            this, [this]() {
        OnLogMessage(QStringLiteral("硬件连接状态变更：%1")
                         .arg(HardwareManager::instance().ConnectionStatus()));
    });
}

void AutoRunPage::SetSequenceWorker(SequenceWorker* worker)
{
    m_worker = worker;
    if (!m_worker) return;

    connect(m_worker, &SequenceWorker::logMessage, this, &AutoRunPage::OnLogMessage);
    connect(m_worker, &SequenceWorker::actionStarted, this, &AutoRunPage::OnActionStarted);
    connect(m_worker, &SequenceWorker::schemeFinished, this, &AutoRunPage::OnSchemeFinished);
    connect(m_worker, &SequenceWorker::interrupted, this, &AutoRunPage::OnInterrupted);
    connect(m_worker, &SequenceWorker::errorOccurred, this, &AutoRunPage::OnError);
    // 循环进度（TR-091）：每轮开始一次；schemeFinished 仍只在全部循环完成时发一次
    connect(m_worker, &SequenceWorker::cycleChanged, this, &AutoRunPage::OnCycleChanged);
    // 状态迁移 → 统一刷新按钮使能与状态标签（唯一出口，杜绝散点 setEnabled）
    connect(m_worker, &SequenceWorker::stateChanged,
            this, &AutoRunPage::OnWorkerStateChanged);
}

void AutoRunPage::SetAutoModeActive(bool active)
{
    if (m_autoModeActive == active) return;
    m_autoModeActive = active;
    UpdateControlsEnabled();
}

void AutoRunPage::SetupUI()
{
    setStyleSheet("background: #262c34;");

    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(14, 14, 14, 14);
    mainLayout->setSpacing(16);

    // ---- 左侧 (60%) ----
    auto* leftSide = new QWidget();
    leftSide->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* leftLayout = new QVBoxLayout(leftSide);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(10);

    const QString cameraStyle = R"(
        QWidget {
            background: #0b0d0f;
            border-radius: 12px;
            border: 1px solid #3a424e;
        }
    )";

    m_cameraRgbLabel = new QLabel(QStringLiteral("RGB 相机画面 (3D)"));
    m_cameraRgbLabel->setAlignment(Qt::AlignCenter);
    m_cameraRgbLabel->setStyleSheet(cameraStyle);
    m_cameraRgbLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_cameraRgbLabel->setScaledContents(true);

    m_cameraOverlayLabel = new QLabel(QStringLiteral("识别结果叠加图"));
    m_cameraOverlayLabel->setAlignment(Qt::AlignCenter);
    m_cameraOverlayLabel->setStyleSheet(cameraStyle);
    m_cameraOverlayLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_cameraOverlayLabel->setScaledContents(true);

    leftLayout->addWidget(m_cameraRgbLabel, 1);
    leftLayout->addWidget(m_cameraOverlayLabel, 1);

    // ---- 右侧 (40%) ----
    auto* rightSide = new QWidget();
    rightSide->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* rightLayout = new QVBoxLayout(rightSide);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(10);

    // LCD 行
    auto* lcdRow = new QHBoxLayout();
    lcdRow->setSpacing(16);
    struct LcdItem { QString label; QString value; };
    QVector<LcdItem> lcds = {
        { QStringLiteral("当前节拍 CT"), QStringLiteral("2.4s") },
        { QStringLiteral("今日产量"),    QStringLiteral("12,860") },
    };
    for (const auto& item : lcds)
    {
        auto* lcdWidget = new QWidget();
        lcdWidget->setStyleSheet("background: #0d1219; border-radius: 8px; padding: 6px;");
        lcdWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* lcdLayout = new QVBoxLayout(lcdWidget);
        lcdLayout->setAlignment(Qt::AlignCenter);
        lcdLayout->setSpacing(2);

        auto* lbl = new QLabel(item.label);
        lbl->setStyleSheet("color: #7c8a9e; font-size: 13px; background: transparent; border: none;");
        lbl->setAlignment(Qt::AlignCenter);

        auto* val = new QLabel(item.value);
        val->setStyleSheet("font-size: 32px; font-weight: 700; color: #cde2ff; font-family: 'Consolas', monospace; background: transparent; border: none;");
        val->setAlignment(Qt::AlignCenter);
        lcdLayout->addWidget(lbl);
        lcdLayout->addWidget(val);
        lcdRow->addWidget(lcdWidget);
    }
    rightLayout->addLayout(lcdRow);

    // 状态标签
    m_statusLabel = new QLabel(QStringLiteral("⏸ 待机"));
    m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #8da3bb; padding: 6px 0; background: transparent; border: none;");
    rightLayout->addWidget(m_statusLabel);

    // 坐标面板
    //m_coordPanel = new QLabel(QStringLiteral("X: 0.00 mm  Y: 0.00 mm  Z: 0.00 mm  R: 0.00°"));
    //m_coordPanel->setStyleSheet("background: #0d141c; border-radius: 10px; padding: 8px 16px; border: 1px solid #2f7fb5; color: #7ed6ff; font-size: 16px; font-weight: 600; font-family: 'Consolas', monospace;");
    //m_coordPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    //m_coordPanel->setFixedHeight(44);
    //rightLayout->addWidget(m_coordPanel);

    // 坐标面板
    m_coordPanel = new QLabel(QStringLiteral("X:0.00 mm  Y:0.00 mm  Z:0.00 mm  R:0.00"));

    // 【修改 1】：字号改为 14px，左右 padding 改为 6px，让出大量水平空间
    m_coordPanel->setStyleSheet(R"(
        background: #0d141c; border-radius: 10px; padding: 8px 6px; 
        border: 1px solid #2f7fb5; color: #7ed6ff; 
        font-size: 14px; font-weight: 600; font-family: 'Consolas', monospace;
    )");

    // 【修改 2】：允许垂直方向根据内容自动伸缩 ( Minimum 策略 )
    m_coordPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    m_coordPanel->setMinimumHeight(44);

    // 【修改 3】：开启自动换行，并居中显示
    m_coordPanel->setWordWrap(true);
    m_coordPanel->setAlignment(Qt::AlignCenter);

    rightLayout->addWidget(m_coordPanel);

    // 日志框
    m_logTextEdit = new QTextEdit();
    m_logTextEdit->setReadOnly(true);
    m_logTextEdit->document()->setMaximumBlockCount(1000);   // 日志行数上限，防长期运行内存膨胀
    m_logTextEdit->setStyleSheet(R"(
        QTextEdit {
            background: #12161c; border: 1px solid #3a424e; border-radius: 10px;
            color: #b8cce3; font-size: 12px; font-family: 'Consolas', monospace;
            padding: 8px 12px;
        }
        QScrollBar:vertical {
            background: #1b1f26; width: 8px; border-radius: 4px;
        }
        QScrollBar::handle:vertical {
            background: #3a424e; border-radius: 4px; min-height: 30px;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    )");
    m_logTextEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    rightLayout->addWidget(m_logTextEdit, 1);

    // 方案下拉
    auto* schemeRow = new QWidget();
    schemeRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* schemeLayout = new QHBoxLayout(schemeRow);
    schemeLayout->setContentsMargins(0, 0, 0, 0);
    schemeLayout->setSpacing(8);

    auto* schemeLabel = new QLabel(QStringLiteral("方案"));
    schemeLabel->setStyleSheet("color: #b8cce3; font-size: 14px; font-weight: 600; background: transparent; border: none;");

    m_schemeCombo = new QComboBox();
    m_schemeCombo->setStyleSheet(R"(
        QComboBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px 24px 4px 8px; border-radius: 6px; font-size: 13px; min-height: 22px; }
        QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 18px; border: none; }
        QComboBox::down-arrow { width: 0; height: 0; border-top: 5px solid #8da3bb; border-left: 4px solid transparent; border-right: 4px solid transparent; margin-right: 3px; }
        QComboBox QAbstractItemView { background: #1a2129; color: #dbe6f0; border: none; outline: 1px solid #3f4e5e; selection-background-color: #2f6f9f; }
    )");
    m_schemeCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_schemeCombo->setFixedHeight(32);

    RefreshSchemeCombo();

    schemeLayout->addWidget(schemeLabel);
    schemeLayout->addWidget(m_schemeCombo, 1);
    rightLayout->addWidget(schemeRow);

    // 循环生产行（TR-091）：模式 / 次数 / 回安全位 / 进度，**同一行**排布（布局待真机效果后定）。
    // 硬约束 = 10 寸触控屏 + 窗口缩到最小尺寸（1200×700）时不得折行/裁切/重叠；
    // 故控件均给固定宽高（高 32 ≥ 手指命中下限），余量交给 stretch + 右对齐进度标签吸收。
    auto* loopRow = new QWidget();
    loopRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* loopLayout = new QHBoxLayout(loopRow);
    loopLayout->setContentsMargins(0, 0, 0, 0);
    loopLayout->setSpacing(8);

    auto* loopLabel = new QLabel(QStringLiteral("循环"));
    loopLabel->setStyleSheet("color: #b8cce3; font-size: 14px; font-weight: 600; background: transparent; border: none;");

    // 模式下拉：index 与 LoopConfig::Mode 一一对应（0 单轮 / 1 指定次数 / 2 无限循环）
    m_loopModeCombo = new QComboBox();
    m_loopModeCombo->addItems({ QStringLiteral("单轮"),
                                QStringLiteral("指定次数"),
                                QStringLiteral("无限循环") });
    m_loopModeCombo->setStyleSheet(R"(
        QComboBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px 24px 4px 8px; border-radius: 6px; font-size: 13px; min-height: 22px; }
        QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 18px; border: none; }
        QComboBox::down-arrow { width: 0; height: 0; border-top: 5px solid #8da3bb; border-left: 4px solid transparent; border-right: 4px solid transparent; margin-right: 3px; }
        QComboBox QAbstractItemView { background: #1a2129; color: #dbe6f0; border: none; outline: 1px solid #3f4e5e; selection-background-color: #2f6f9f; }
        QComboBox:disabled { color: #7c8a9e; border-color: #333c47; }
    )");
    m_loopModeCombo->setFixedSize(100, 32);
    m_loopModeCombo->setToolTip(QStringLiteral("单轮 = 与历史行为一致；指定次数 = 跑 N 轮后自动结束；无限循环需二次确认"));

    // 次数框：仅「指定次数」可用（见 UpdateControlsEnabled 唯一出口）
    m_loopCountSpin = new QSpinBox();
    m_loopCountSpin->setRange(1, 9999);
    m_loopCountSpin->setValue(10);
    m_loopCountSpin->setAlignment(Qt::AlignCenter);
    m_loopCountSpin->setStyleSheet(R"(
        QSpinBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; font-size: 13px; padding: 2px 4px; }
        QSpinBox:disabled { color: #7c8a9e; border-color: #333c47; }
        QSpinBox::up-button, QSpinBox::down-button { width: 16px; background: #1a2129; border: none; }
    )");
    m_loopCountSpin->setFixedSize(78, 32);
    m_loopCountSpin->setToolTip(QStringLiteral("循环轮数（1 ~ 9999）"));

    // 「回安全位」= 进入下一轮前先回安全位（第 1 轮之前不回、最后一轮之后不回）。
    // 术语按 TR-082/D15：此处只决定循环间隔是否回；总开关是设备配置页的「启用安全位」。
    m_loopSafePosCheck = new QCheckBox(QStringLiteral("回安全位"));
    m_loopSafePosCheck->setChecked(true);
    m_loopSafePosCheck->setStyleSheet(R"(
        QCheckBox { color: #b8cce3; font-size: 13px; background: transparent; spacing: 6px; }
        QCheckBox:disabled { color: #7c8a9e; }
        QCheckBox::indicator { width: 18px; height: 18px; border: 1px solid #3f4e5e; border-radius: 4px; background: #111a22; }
        QCheckBox::indicator:checked { background: #2f6f9f; border-color: #2f7fb5; }
    )");
    m_loopSafePosCheck->setFixedHeight(32);
    m_loopSafePosCheck->setCursor(Qt::PointingHandCursor);
    m_loopSafePosCheck->setToolTip(QStringLiteral("每轮结束后先回安全位再进入下一轮，避免跨轮大位移横扫工作区（需先在设备配置页启用安全位）"));

    // 进度标签：常显（替代已裁决不做的常驻提示条——单槽提示必被其它 SetHint 覆盖）
    m_cycleLabel = new QLabel(QStringLiteral("单轮"));
    m_cycleLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_cycleLabel->setStyleSheet("color: #7ed6ff; font-size: 13px; font-weight: 600; font-family: 'Consolas', monospace; background: transparent; border: none;");

    loopLayout->addWidget(loopLabel);
    loopLayout->addWidget(m_loopModeCombo);
    loopLayout->addWidget(m_loopCountSpin);
    loopLayout->addWidget(m_loopSafePosCheck);
    loopLayout->addStretch(1);
    loopLayout->addWidget(m_cycleLabel);
    rightLayout->addWidget(loopRow);

    // 按钮行
    auto* btnRow = new QWidget();
    btnRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* btnLayout = new QHBoxLayout(btnRow);
    btnLayout->setContentsMargins(0, 0, 0, 0);
    btnLayout->setSpacing(10);

    // 5 按钮（2026-09-07 语义重构）：启动/继续、暂停、停止、清报警、初始化。
    // 按下标分派，不再靠 text.contains 匹配（改文案会静默错配）。
    struct BtnDef { QString text; QString bg; QString hover; QString fg; QString tip; };
    const QVector<BtnDef> btns = {
        { QStringLiteral("▶ 启动/继续"), QStringLiteral("#1f9d4a"), QStringLiteral("#28b85a"),
          QStringLiteral("white"),   QStringLiteral("启动新流程；暂停中则从暂停处继续") },
        { QStringLiteral("⏸ 暂停"),     QStringLiteral("#c78f1a"), QStringLiteral("#e0a520"),
          QStringLiteral("#1a1e24"), QStringLiteral("减速停止并保持上下文，点「启动/继续」恢复") },
        { QStringLiteral("■ 停止"),     QStringLiteral("#b13a3a"), QStringLiteral("#d14444"),
          QStringLiteral("white"),   QStringLiteral("立即减速停止，结束流程并废弃上下文") },
        { QStringLiteral("↺ 清报警"),   QStringLiteral("#5a6675"), QStringLiteral("#6b7889"),
          QStringLiteral("white"),   QStringLiteral("清除执行引擎故障锁存（驱动器报警请在手动页断使能后重新使能）") },
        { QStringLiteral("⟳ 初始化"),   QStringLiteral("#2f6f9f"), QStringLiteral("#3a84b8"),
          QStringLiteral("white"),   QStringLiteral("重新加载配置并连接硬件（不含使能/回零）") },
    };

    for (int i = 0; i < btns.size(); ++i)
    {
        const auto& b = btns[i];
        auto* btn = new QPushButton(b.text);
        QString style = QStringLiteral(
            "QPushButton { background: %1; border: none; border-radius: 8px; font-weight: 700; font-size: 12px; padding: 0 2px; min-width: 0; color: %2; }"
            "QPushButton:hover { background: %3; }"
            "QPushButton:disabled { background: #3a424e; color: #7c8a9e; }"
        ).arg(b.bg, b.fg, b.hover);
        btn->setStyleSheet(style);
        btn->setToolTip(b.tip);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        btn->setFixedHeight(46);

        switch (i) {
        case 0: m_btnStartOrResume = btn;
                connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnStartOrResumeClicked); break;
        case 1: m_btnPause = btn;
                connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnPauseClicked); break;
        case 2: m_btnStop = btn;
                connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnStopClicked); break;
        case 3: m_btnAlarmClear = btn;
                connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnAlarmClearClicked); break;
        default:m_btnInit = btn;
                connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnInitClicked); break;
        }

        btnLayout->addWidget(btn, 1);
    }
    rightLayout->addWidget(btnRow);

    // 急停已升舱到 MainWindow 顶栏（全局唯一入口）：本页仅响应信号做状态恢复
    connect(&HardwareManager::instance(), &HardwareManager::emergencyStopTriggered,
            this, &AutoRunPage::OnEmergencyTriggered);
    // 回零/使能变化 → 统一刷新按钮使能（不再直接 setEnabled(homed)，走唯一出口）
    connect(&HardwareManager::instance(), &HardwareManager::homeStateChanged,
            this, [this](bool /*homed*/) { UpdateControlsEnabled(); });
    connect(&HardwareManager::instance(), &HardwareManager::enableStateChanged,
            this, [this]() { UpdateControlsEnabled(); });

    // 底部提示已迁出（2026-09-15）：原 m_hintLabel 占用本页竖向空间，
    // 现由 SetHint → requestGlobalHint → MainWindow 顶栏全局横幅承载。
    // 横幅位于【手动/自动】滑块左侧，超长 Elide 截断、点击弹出全文。

    // 循环控件变更 → 写回 config（300ms 防抖）+ 刷新使能（唯一出口 UpdateControlsEnabled）
    connect(m_loopModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { PersistLoopConfig(); });
    connect(m_loopCountSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int) { PersistLoopConfig(); });
    connect(m_loopSafePosCheck, &QCheckBox::toggled,
            this, [this](bool) { PersistLoopConfig(); });

    // 组装主布局
    mainLayout->addWidget(leftSide, 6);
    mainLayout->addWidget(rightSide, 4);
}

void AutoRunPage::RefreshCoordPanel()
{
    double l1, l2, z0, h1, tcpX, tcpY, tcpZ;
    KinematicsHelper::ReadConfigParams(l1, l2, z0, h1, tcpX, tcpY, tcpZ);

    // 运动学参数仅在变化时重建一次，避免每 50ms 构造 Kinematics 刷日志
    if (!m_kinParamsLoaded || l1 != m_kinL1 || l2 != m_kinL2 || z0 != m_kinZ0 || h1 != m_kinH1
        || tcpX != m_kinTcpX || tcpY != m_kinTcpY || tcpZ != m_kinTcpZ) {
        m_kin = KinematicsHelper::FromConfig();
        m_kinL1 = l1; m_kinL2 = l2; m_kinZ0 = z0; m_kinH1 = h1;
        m_kinTcpX = tcpX; m_kinTcpY = tcpY; m_kinTcpZ = tcpZ;
        m_kinParamsLoaded = true;
    }

    Joints joints{m_j1, m_j2, m_z, m_r};
    Pose pose = m_kin.Forward(joints);

    QString text = QStringLiteral("X: %1 mm  Y: %2 mm  Z: %3 mm  R: %4°")
        .arg(pose.x, 0, 'f', 2).arg(pose.y, 0, 'f', 2)
        .arg(pose.z, 0, 'f', 2).arg(pose.r, 0, 'f', 2);
    if (text != m_lastCoordText) {
        m_coordPanel->setText(text);
        m_lastCoordText = text;
    }
}

void AutoRunPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    RefreshSchemeCombo();   // ProcessPage 增删改方案后，切回本页时下拉同步
}

void AutoRunPage::RefreshSchemeCombo()
{
    if (!m_schemeCombo) return;
    QString cur = m_schemeCombo->currentText();
    m_schemeCombo->clear();
    const auto& schemes = ProcessManager::instance().schemes();
    for (const auto& s : schemes)
        m_schemeCombo->addItem(s.schemeName);
    if (m_schemeCombo->count() == 0)
        m_schemeCombo->addItem(QStringLiteral("（无方案）"));
    if (!cur.isEmpty()) {
        int idx = m_schemeCombo->findText(cur);
        if (idx >= 0) m_schemeCombo->setCurrentIndex(idx);
    }
}

void AutoRunPage::OnStartOrResumeClicked()
{
    if (!m_worker) { SetHint(QStringLiteral("错误：执行引擎未初始化"), QStringLiteral("#ff5e6b")); return; }

    using WS = SequenceWorker::WorkerState;
    const WS st = m_worker->GetState();

    // ① 暂停态 → 继续（用户暂停 → 解除挂起原地续走；单步等待 → 放行下一步）
    if (st == WS::Paused) {
        if (m_worker->Resume())
            SetHint(QStringLiteral("继续运行…"), QStringLiteral("#8fd4ff"));
        else
            SetHint(QStringLiteral("继续失败：引擎不在暂停态"), QStringLiteral("#f7c948"));
        return;
    }
    // ② 故障态 → 拒绝（必须先清除报警）
    if (st == WS::Fault) {
        SetHint(QStringLiteral("存在未清除的故障，请先点击「↺ 清报警」"), QStringLiteral("#f7c948"));
        return;
    }
    if (st == WS::Running) return;   // 运行中按钮应已置灰，防御

    // ③ 空闲 → 启动新流程（三层门禁不变：使能 / 方案存在 / 未运行）
    if (!HardwareManager::instance().IsGlobalEnabled()) {
        SetHint(QStringLiteral("请先使能所有轴（手动页点击「全部使能」）"), QStringLiteral("#f7c948"));
        return;
    }
    const auto& schemes = ProcessManager::instance().schemes();
    if (schemes.isEmpty()) {
        SetHint(QStringLiteral("尚无方案，请先在「工艺流程」页创建方案"), QStringLiteral("#f7c948"));
        return;
    }
    if (m_schemeCombo->currentIndex() < 0 || m_schemeCombo->currentText() == QStringLiteral("（无方案）")) return;
    const int idx = m_schemeCombo->currentIndex();
    if (idx >= schemes.size()) return;

    // ③-1 循环前置门禁（TR-091）：勾了「回安全位」且确实是循环（非单轮）时，安全位总开关
    //      必须已启用——否则第 1 轮结束无安全位可回，下一轮起点的跨轮大位移会横扫工作区。
    //      单轮不触发（不发生循环间隔回安全位）→ 8.18 零回归不受影响。
    const SequenceWorker::LoopConfig loop = BuildLoopConfig();
    using LM = SequenceWorker::LoopConfig::Mode;
    if (loop.returnToSafePos && loop.mode != LM::Single
        && !ConfigManager::instance().getValue<bool>("kinematics.safePos.enabled", false)) {
        SetHint(QStringLiteral("循环运行需先示教安全位（手动控制页「示教安全位」），或取消勾选「回安全位」"),
                QStringLiteral("#f7c948"));
        return;
    }
    // ③-2 无限循环二次确认（三挡中无人值守风险最高的一挡）
    if (loop.mode == LM::Infinite) {
        const auto ans = QMessageBox::warning(this, QStringLiteral("确认无限循环"),
            QStringLiteral("将无限循环执行「%1」，不会自动停止。\n\n"
                           "启动前请确认：\n"
                           "· 急停按钮在手边可及\n"
                           "· 工作区内无人员肢体\n"
                           "· 软限位与安全位已核对\n\n"
                           "确认开始无限循环？").arg(schemes[idx].schemeName),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ans != QMessageBox::Yes) {
            SetHint(QStringLiteral("已取消无限循环启动"), QStringLiteral("#8fd4ff"));
            return;
        }
    }

    ResetCycleLabel();
    m_worker->ReloadFromConfig();
    if (!m_worker->RunSequence(schemes[idx], loop)) {
        SetHint(QStringLiteral("启动失败：引擎忙或门禁未通过"), QStringLiteral("#f7c948"));
        return;
    }
    // 不在此改按钮：等 stateChanged(Running) 到达后由 UpdateControlsEnabled 统一处理
    SetHint(loop.mode == LM::Single
                ? QStringLiteral("方案执行中...")
                : QStringLiteral("循环执行中...（停止请点「■ 停止」）"),
            QStringLiteral("#8fd4ff"));
}

void AutoRunPage::OnPauseClicked()
{
    if (!m_worker) return;
    if (m_worker->GetState() != SequenceWorker::WorkerState::Running) {
        SetHint(QStringLiteral("仅在运行时可暂停"), QStringLiteral("#f7c948"));
        return;
    }
    if (m_worker->Pause())
        SetHint(QStringLiteral("暂停请求已下发（减速停止后挂起，点「▶ 启动/继续」恢复）"),
                QStringLiteral("#8fd4ff"));
    else
        SetHint(QStringLiteral("当前处于单步等待，点「▶ 启动/继续」即可放行下一步"),
                QStringLiteral("#f7c948"));
}

void AutoRunPage::OnStopClicked()
{
    if (!m_worker) return;
    // 立即停：cancel + 全轴减速停（保持使能）。不立即恢复按钮——
    // 等 interrupted / stateChanged(Idle) 到达后由 UpdateControlsEnabled 统一恢复；
    // 引擎本就空闲（StopImmediate 不发信号）时就地刷新，防永久置灰。
    m_worker->StopImmediate();
    if (m_worker->GetState() == SequenceWorker::WorkerState::Idle)
        UpdateControlsEnabled();
    else
        SetHint(QStringLiteral("正在安全停止…"), QStringLiteral("#8fd4ff"));
    m_statusLabel->setText(QStringLiteral("⏹ 停止中…"));
    m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #f7c948; padding: 6px 0; background: transparent; border: none;");
}

void AutoRunPage::OnAlarmClearClicked()
{
    if (!m_worker) return;
    using WS = SequenceWorker::WorkerState;
    const WS st = m_worker->GetState();
    if (st == WS::Running || st == WS::Paused) {
        SetHint(QStringLiteral("请先「■ 停止」结束流程，再清除报警"), QStringLiteral("#f7c948"));
        return;
    }
    if (st == WS::Fault) {
        m_worker->ClearFault();   // 仅清引擎锁存，不动硬件使能（"程序不自动使能"是既有安全原则）
        m_logTextEdit->append(QStringLiteral("[%1] 清除报警：%2")
            .arg(QDateTime::currentDateTime().toString("HH:mm:ss"),
                 m_lastError.isEmpty() ? QStringLiteral("故障锁存已清除") : m_lastError));
        SetHint(QStringLiteral("故障锁存已清除。若驱动器仍报警，请到【手动控制】页断使能后重新使能，再执行一键回零"),
                QStringLiteral("#8fd4ff"));
    } else {
        SetHint(QStringLiteral("当前无故障锁存"), QStringLiteral("#8fd4ff"));
    }
    UpdateControlsEnabled();
}

void AutoRunPage::OnInitClicked()
{
    auto& hw = HardwareManager::instance();
    if (m_worker && m_worker->GetState() != SequenceWorker::WorkerState::Idle) {
        SetHint(QStringLiteral("流程执行中，禁止初始化（请先停止）"), QStringLiteral("#f7c948"));
        return;
    }
    if (!hw.Initialize()) {
        // TR-093：Initialize 返回值 = 硬件是否真正就绪（原实现无条件 return true，
        // 未连接也报「初始化完成 / 硬件已连接」）。此处给出具体哪一路未就绪；
        // 「再点一次重试」不是空话——Initialize 二次进入只补做连接（不重建对象、不重启轮询）。
        const QString status = hw.ConnectionStatus();
        SetHint(QStringLiteral("初始化失败：%1。请检查设备配置页的连接参数，接好硬件后再点一次「⟳ 初始化」重试")
                    .arg(status),
                QStringLiteral("#ff5e6b"));
        m_logTextEdit->append(QStringLiteral("[%1] 初始化失败：%2")
                                  .arg(QDateTime::currentDateTime().toString("HH:mm:ss"), status));
        UpdateControlsEnabled();
        return;
    }
    m_logTextEdit->append(QStringLiteral("[%1] 初始化完成").arg(QDateTime::currentDateTime().toString("HH:mm:ss")));
    // 分级引导（回零归属手动控制页；"程序不自动使能"是既有安全原则）
    if (!hw.IsGlobalEnabled())
        SetHint(QStringLiteral("硬件已连接。请到【手动控制】页「全部使能」→「一键回零」后方可启动"),
                QStringLiteral("#8fd4ff"));
    else if (!hw.IsSystemHomed())
        SetHint(QStringLiteral("已使能但未回零。请到【手动控制】页执行「一键回零」"),
                QStringLiteral("#8fd4ff"));
    else
        SetHint(QStringLiteral("初始化完成，可以启动"), QStringLiteral("#8fd4ff"));
    UpdateControlsEnabled();
}

void AutoRunPage::OnEmergencyTriggered()
{
    // 仅 UI 响应：硬件断使能与 worker 中断由全局急停触发点（MainWindow）完成；
    // 急停锁存在 HardwareManager（IsEStopPending），gate 已置 false → 五钮全灭，
    // 解锁须等急停后全轴回零完成（MarkAxisHomed → estopPending_ 清除）。
    UpdateControlsEnabled();
    SetHint(QStringLiteral(
        "急停已触发，系统已锁定。请切换至【手动控制】模式，依次执行【全局轴使能】→【一键回零】，"
        "回零完成后急停锁定自动解除，方可恢复生产"),
        QStringLiteral("#ff5e6b"));
    m_statusLabel->setText(QStringLiteral("⛔ 急停"));
    m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #ff5e6b; padding: 6px 0; background: transparent; border: none;");
    m_logTextEdit->append(QStringLiteral("[%1] 急停触发").arg(QDateTime::currentDateTime().toString("HH:mm:ss")));
}

void AutoRunPage::OnFrameReady(const CameraFrame& frame)
{
    QImage rgbImg = FrameConverter::ColorToQImage(frame);
    if (rgbImg.isNull()) return;

    QPixmap rgbPm = QPixmap::fromImage(rgbImg);
    m_cameraRgbLabel->setPixmap(rgbPm);

    // 无检测结果时不再整幅深拷贝 + 空跑 DrawOverlays，两个标签共享同一 QPixmap
    m_cameraOverlayLabel->setPixmap(rgbPm);
}

void AutoRunPage::OnLogMessage(const QString& msg)
{
    m_logTextEdit->append(QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), msg));
    QScrollBar* sb = m_logTextEdit->verticalScrollBar();
    if (sb) sb->setValue(sb->maximum());
}

void AutoRunPage::OnStateUpdated(const QVector<MotorStatus>& axes)
{
    // stateUpdated 已含每轴逻辑位置（卡轴），直接复用，避免 50ms 实时 GetPosition 读卡
    for (const auto& st : axes) {
        if (st.axisId == static_cast<int>(LogicalAxis::J1)) m_j1 = st.position;
        else if (st.axisId == static_cast<int>(LogicalAxis::Z)) m_z = st.position;
    }
    RefreshCoordPanel();
}

void AutoRunPage::OnServoStateUpdated(const QVector<ServoTelemetry>& servos)
{
    // servos[0]=J2, servos[1]=R（HardwareManager 已把角度转为逻辑坐标）
    if (servos.size() > 0) m_j2 = servos[0].angleDeg;
    if (servos.size() > 1) m_r  = servos[1].angleDeg;
    RefreshCoordPanel();
}

void AutoRunPage::OnActionStarted(int /*index*/, const QString& name)
{
    // TR-088：记录是否安全位临时会话——此刻 safeSession 仍在、可靠；完成槽（OnSchemeFinished）
    // 被跨线程排队时 safeSession 可能已被 StartExecution 清零，不能届时直接查
    m_safePosSessionActive = m_worker && m_worker->IsSafePosSession();
    m_statusLabel->setText(QStringLiteral("▶ %1").arg(name));
    m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #7ed67e; padding: 6px 0; background: transparent; border: none;");
}

void AutoRunPage::OnSchemeFinished()
{
    UpdateControlsEnabled();
    if (m_safePosSessionActive) {
        // TR-088：安全位是临时方案（不在方案列表），显示"方案执行完成"会误导操作员
        m_statusLabel->setText(QStringLiteral("✅ 已回安全位"));
        m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #7ed67e; padding: 6px 0; background: transparent; border: none;");
        SetHint(QStringLiteral("已返回安全位，可启动方案"), QStringLiteral("#8fd4ff"));
    } else {
        m_statusLabel->setText(QStringLiteral("✅ 完成"));
        m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #7ed67e; padding: 6px 0; background: transparent; border: none;");
        SetHint(QStringLiteral("方案执行完成"), QStringLiteral("#8fd4ff"));
    }
    m_safePosSessionActive = false;
}

void AutoRunPage::OnInterrupted(const QString& reason)
{
    UpdateControlsEnabled();
    m_statusLabel->setText(QStringLiteral("⏸ 中断"));
    m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #f7c948; padding: 6px 0; background: transparent; border: none;");
    m_logTextEdit->append(QStringLiteral("[%1] 中断: %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), reason));
}

void AutoRunPage::OnError(const QString& message)
{
    m_lastError = message;           // 供「清报警」提示（避免跨线程读 worker 内部字符串）
    m_logTextEdit->append(QStringLiteral("[%1] 错误: %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), message));
    // 故障锁存（引擎已进 Fault）→ 统一出口恢复按钮可用性（仅清报警/初始化可点）
    UpdateControlsEnabled();
    m_statusLabel->setText(QStringLiteral("⛔ 错误"));
    m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #ff5e6b; padding: 6px 0; background: transparent; border: none;");
    SetHint(QStringLiteral("执行出错：%1（清除报警后可重新启动）").arg(message), QStringLiteral("#ff5e6b"));
}

// 循环进度（TR-091）：每轮开始时由引擎发一次；index 从 1 起，total < 0 = 无限循环。
// 终态不清空（便于取证），下次启动前由 ResetCycleLabel() 复位。
void AutoRunPage::OnCycleChanged(int index, int total)
{
    if (!m_cycleLabel) return;
    m_cycleLabel->setText(total > 0
                              ? QStringLiteral("循环 %1/%2").arg(index).arg(total)
                              : QStringLiteral("循环 %1/∞").arg(index));
}

// ---- 循环生产控件（TR-091）：读 config / 写 config / 组装 LoopConfig ----

void AutoRunPage::LoadLoopConfigFromConfig()
{
    if (!m_loopModeCombo || !m_loopCountSpin || !m_loopSafePosCheck) return;
    auto& cfg = ConfigManager::instance();
    const int  mode  = qBound(0, cfg.getValue<int>("production.loop.mode", 0), 2);
    const int  count = qBound(1, cfg.getValue<int>("production.loop.count", 10), 9999);
    const bool safe  = cfg.getValue<bool>("production.loop.returnToSafePos", true);

    // 屏蔽信号：初始化赋值不得回写 config——旧 config 首次启动不该平白多出 production 节点
    {
        const QSignalBlocker b1(m_loopModeCombo);
        const QSignalBlocker b2(m_loopCountSpin);
        const QSignalBlocker b3(m_loopSafePosCheck);
        m_loopModeCombo->setCurrentIndex(mode);
        m_loopCountSpin->setValue(count);
        m_loopSafePosCheck->setChecked(safe);
    }
    ResetCycleLabel();
    UpdateControlsEnabled();
}

void AutoRunPage::PersistLoopConfig()
{
    auto& cfg = ConfigManager::instance();
    cfg.set("production.loop.mode", m_loopModeCombo->currentIndex());
    cfg.set("production.loop.count", m_loopCountSpin->value());
    cfg.set("production.loop.returnToSafePos", m_loopSafePosCheck->isChecked());
    // 刻意不 emit paramsChanged：其唯一消费者是 ReloadFromConfig（重建 Kinematics），
    // 而循环参数与运动学无关，emit 只会造成无谓重建。
    UpdateControlsEnabled();
}

SequenceWorker::LoopConfig AutoRunPage::BuildLoopConfig() const
{
    using LM = SequenceWorker::LoopConfig::Mode;
    SequenceWorker::LoopConfig loop;
    loop.mode            = static_cast<LM>(qBound(0, m_loopModeCombo->currentIndex(), 2));
    loop.count           = m_loopCountSpin->value();
    loop.returnToSafePos = m_loopSafePosCheck->isChecked();
    // 重试口子本次不进 UI（仅 config 手改，见方案 §七 / D4）
    auto& cfg = ConfigManager::instance();
    loop.retryCount   = qMax(0, cfg.getValue<int>("production.loop.retryCount", 0));
    loop.retryDelayMs = qMax(0, cfg.getValue<int>("production.loop.retryDelayMs", 500));
    return loop;
}

void AutoRunPage::ResetCycleLabel()
{
    using LM = SequenceWorker::LoopConfig::Mode;
    const int idx = m_loopModeCombo->currentIndex();
    if (idx == static_cast<int>(LM::Count))
        m_cycleLabel->setText(QStringLiteral("循环 0/%1").arg(m_loopCountSpin->value()));
    else if (idx == static_cast<int>(LM::Infinite))
        m_cycleLabel->setText(QStringLiteral("循环 0/∞"));
    else
        m_cycleLabel->setText(QStringLiteral("单轮"));
}

void AutoRunPage::OnWorkerStateChanged(SequenceWorker::WorkerState state,
                                       SequenceWorker::PauseReason reason)
{
    UpdateStatusByState(state, reason);
    UpdateControlsEnabled();
}

// ---- 互锁唯一出口与工具（2026-09-07 5 按钮重构） ----

void AutoRunPage::UpdateControlsEnabled()
{
    if (!m_btnStartOrResume || !m_btnPause || !m_btnStop || !m_btnAlarmClear || !m_btnInit) return;

    using WS = SequenceWorker::WorkerState;
    const WS st = m_worker ? m_worker->GetState() : WS::Idle;
    const bool homed = HardwareManager::instance().IsSystemHomed();
    // 互锁门禁（TR-075）：自动模式 且 无急停待恢复锁存（全局硬件级状态，HardwareManager 自持）
    const bool gate = m_autoModeActive && !HardwareManager::instance().IsEStopPending();

    const bool busy    = (st == WS::Running || st == WS::Paused);   // 会话持有中
    const bool faulted = (st == WS::Fault);

    // 方案下拉 / 循环控件：仅运行/暂停期禁止切换；手动模式与急停锁存期保留预选能力（下次启动才读取所选方案）
    m_schemeCombo->setEnabled(!busy);
    if (m_loopModeCombo && m_loopCountSpin && m_loopSafePosCheck) {
        using LM = SequenceWorker::LoopConfig::Mode;
        m_loopModeCombo->setEnabled(!busy);
        m_loopSafePosCheck->setEnabled(!busy);
        // 次数框只在「指定次数」下可用（且运行/暂停期一律置灰）
        m_loopCountSpin->setEnabled(!busy &&
            m_loopModeCombo->currentIndex() == static_cast<int>(LM::Count));
    }

    // 非自动模式 / 急停待恢复 → 五钮全灭（恢复通道：手动页使能/回零 + 顶栏急停永不禁用）
    if (!gate) {
        m_btnStartOrResume->setEnabled(false);
        m_btnPause->setEnabled(false);
        m_btnStop->setEnabled(false);
        m_btnAlarmClear->setEnabled(false);
        m_btnInit->setEnabled(false);
        return;
    }

    // 启动/继续：空闲时受回零互锁（未回零禁启）；暂停态即可点（继续）
    m_btnStartOrResume->setEnabled(faulted ? false : (st == WS::Idle ? homed : st == WS::Paused));
    m_btnPause->setEnabled(st == WS::Running);           // 仅运行中可暂停（单步等待期禁用防误放行）
    m_btnStop->setEnabled(busy);                          // 停止仅在会话持有中可用
    m_btnAlarmClear->setEnabled(!busy);                   // Fault/Idle 可点（Idle 时提示无故障）
    m_btnInit->setEnabled(!busy);                         // 运行/暂停期禁止重初始化
}

void AutoRunPage::SetHint(const QString& text, const QString& color)
{
    // 2026-09-15：本页不再自绘提示，统一转发给 MainWindow 顶栏全局横幅。
    // 保留 SetHint 作为唯一出口——30 余处调用点零改动。
    emit requestGlobalHint(text, color);
}

void AutoRunPage::UpdateStatusByState(SequenceWorker::WorkerState st,
                                      SequenceWorker::PauseReason reason)
{
    using WS = SequenceWorker::WorkerState;
    using PR = SequenceWorker::PauseReason;
    const char* kStatusStyle =
        "font-size: 28px; font-weight: 700; color: %1; padding: 6px 0; background: transparent; border: none;";
    switch (st) {
    case WS::Running:
        m_statusLabel->setText(QStringLiteral("▶ 运行中"));
        m_statusLabel->setStyleSheet(QString(kStatusStyle).arg(QStringLiteral("#7ed67e")));
        break;
    case WS::Paused:
        m_statusLabel->setText(reason == PR::Step ? QStringLiteral("⏸ 单步暂停")
                                                  : QStringLiteral("⏸ 已暂停"));
        m_statusLabel->setStyleSheet(QString(kStatusStyle).arg(QStringLiteral("#f7c948")));
        break;
    case WS::Fault:
        m_statusLabel->setText(QStringLiteral("⛔ 故障"));
        m_statusLabel->setStyleSheet(QString(kStatusStyle).arg(QStringLiteral("#ff5e6b")));
        break;
    case WS::Idle:
        // 终态（完成/停止/急停/错误）已由对应槽先行设置，这里不覆盖
        break;
    }
}

void AutoRunPage::RequestHomeAll()
{
    auto& hw = HardwareManager::instance();
    if (!hw.IsGlobalEnabled()) {
        SetHint(QStringLiteral("请先使能所有轴（手动页点击「全部使能」）"), QStringLiteral("#f7c948"));
        return;
    }
    if (hw.HomeAll())
        m_logTextEdit->append(QStringLiteral("[%1] 一键回零开始")
                                  .arg(QDateTime::currentDateTime().toString("HH:mm:ss")));
    else
        SetHint(QStringLiteral("回零失败：请检查轴连接/回零配置"), QStringLiteral("#ff5e6b"));
}