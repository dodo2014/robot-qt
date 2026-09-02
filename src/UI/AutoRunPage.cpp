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
#include <QTextEdit>
#include <QSizePolicy>
#include <QScrollBar>
#include <QPixmap>
#include <QImage>
#include <QDateTime>
#include <QShowEvent>
#include "spdlog/spdlog.h"

AutoRunPage::AutoRunPage(QWidget* parent)
    : QWidget(parent)
{
    SetupUI();

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
        { QStringLiteral("今日产量"),    QStringLiteral("1,286") },
    };
    for (const auto& item : lcds)
    {
        auto* lcdWidget = new QWidget();
        lcdWidget->setStyleSheet("background: #0d1219; border-radius: 10px; padding: 10px;");
        lcdWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* lcdLayout = new QVBoxLayout(lcdWidget);
        lcdLayout->setAlignment(Qt::AlignCenter);
        lcdLayout->setSpacing(4);
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
    m_coordPanel = new QLabel(QStringLiteral("X: 0.00 mm  Y: 0.00 mm  Z: 0.00 mm  R: 0.00°"));
    m_coordPanel->setStyleSheet("background: #0d141c; border-radius: 10px; padding: 8px 16px; border: 1px solid #2f7fb5; color: #7ed6ff; font-size: 16px; font-weight: 600; font-family: 'Consolas', monospace;");
    m_coordPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_coordPanel->setFixedHeight(44);
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

    auto* schemeLabel = new QLabel(QStringLiteral("运行方案:"));
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

    // 按钮行
    auto* btnRow = new QWidget();
    btnRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* btnLayout = new QHBoxLayout(btnRow);
    btnLayout->setContentsMargins(0, 0, 0, 0);
    btnLayout->setSpacing(10);

    struct BtnDef { QString text; QString bg; QString hover; QString extra; };
    QVector<BtnDef> btns = {
        { QStringLiteral("▶ 启动"),   QStringLiteral("#1f9d4a"), QStringLiteral("#28b85a"), QString() },
        { QStringLiteral("↺ 复位"),   QStringLiteral("#c78f1a"), QStringLiteral("#e0a520"), QStringLiteral("color: #1a1e24;") },
        { QStringLiteral("⏹ 停止"),   QStringLiteral("#b13a3a"), QStringLiteral("#d14444"), QString() },
        { QStringLiteral("⟳ 初始化"), QStringLiteral("#2f6f9f"), QStringLiteral("#3a84b8"), QString() },
    };

    for (const auto& b : btns)
    {
        auto* btn = new QPushButton(b.text);
        QString style = QStringLiteral(
            "QPushButton { background: %1; border: none; border-radius: 8px; font-weight: 700; font-size: 13px; padding: 0 4px; min-width: 0; color: white; %2 }"
            "QPushButton:hover { background: %3; }"
        ).arg(b.bg, b.extra, b.hover);
        btn->setStyleSheet(style);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        btn->setFixedHeight(46);

        if (b.text.contains("启动")) {
            m_btnStart = btn;
            // 回零互锁：未回零禁止启动自动运行（开环步进断电丢坐标），回零完成后激活
            m_btnStart->setEnabled(HardwareManager::instance().IsSystemHomed());
            connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnStartClicked);
        }
        else if (b.text.contains("复位"))   connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnResetClicked);
        else if (b.text.contains("停止"))   connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnStopClicked);
        else                                connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnInitClicked);

        btnLayout->addWidget(btn, 1);
    }
    rightLayout->addWidget(btnRow);

    // 急停已升舱到 MainWindow 顶栏（全局唯一入口）：本页仅响应信号做状态恢复
    connect(&HardwareManager::instance(), &HardwareManager::emergencyStopTriggered,
            this, &AutoRunPage::OnEmergencyTriggered);
    // 回零互锁联动：全轴回零成功后激活启动按钮（HomeAll 成功发 homeStateChanged(true)）
    connect(&HardwareManager::instance(), &HardwareManager::homeStateChanged,
            this, [this](bool homed) {
        if (m_btnStart) m_btnStart->setEnabled(homed);
    });

    // 底部提示
    m_hintLabel = new QLabel(QStringLiteral("提示：选择方案后点击「启动」开始运行"));
    m_hintLabel->setStyleSheet("color: #8fd4ff; font-size: 12px; background: transparent; border: none; padding: 2px 0;");
    rightLayout->addWidget(m_hintLabel);

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

void AutoRunPage::OnStartClicked()
{
    if (!m_worker) {
        m_hintLabel->setText(QStringLiteral("错误：执行引擎未初始化"));
        m_hintLabel->setStyleSheet("color: #ff5e6b; font-size: 12px; background: transparent; border: none;");
        return;
    }
    if (!HardwareManager::instance().IsGlobalEnabled()) {
        m_hintLabel->setText(QStringLiteral("请先使能所有轴（手动页点击「全部使能」）"));
        m_hintLabel->setStyleSheet("color: #f7c948; font-size: 12px; background: transparent; border: none;");
        return;
    }
    const auto& schemes = ProcessManager::instance().schemes();
    if (schemes.isEmpty()) {
        m_hintLabel->setText(QStringLiteral("尚无方案，请先在「工艺流程」页创建方案"));
        m_hintLabel->setStyleSheet("color: #f7c948; font-size: 12px; background: transparent; border: none;");
        return;
    }
    if (m_schemeCombo->currentIndex() < 0 || m_schemeCombo->currentText() == QStringLiteral("（无方案）")) return;
    int idx = m_schemeCombo->currentIndex();
    if (idx >= schemes.size()) return;

    m_worker->ReloadFromConfig();
    bool ok = m_worker->RunSequence(schemes[idx]);
    if (ok) {
        m_btnStart->setEnabled(false);
        m_statusLabel->setText(QStringLiteral("▶ 运行中"));
        m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #7ed67e; padding: 6px 0; background: transparent; border: none;");
        m_hintLabel->setText(QStringLiteral("方案执行中..."));
        m_hintLabel->setStyleSheet("color: #8fd4ff; font-size: 12px; background: transparent; border: none;");
    } else {
        m_hintLabel->setText(QStringLiteral("启动失败：已在运行中"));
        m_hintLabel->setStyleSheet("color: #f7c948; font-size: 12px; background: transparent; border: none;");
    }
}

void AutoRunPage::OnResetClicked()
{
    auto& hw = HardwareManager::instance();
    if (!hw.IsGlobalEnabled()) {
        m_hintLabel->setText(QStringLiteral("请先使能所有轴"));
        m_hintLabel->setStyleSheet("color: #f7c948; font-size: 12px; background: transparent; border: none;");
        return;
    }
    bool ok = hw.HomeAll();
    if (ok) {
        m_logTextEdit->append(QStringLiteral("[%1] 复位（回零）开始").arg(QDateTime::currentDateTime().toString("HH:mm:ss")));
    } else {
        m_hintLabel->setText(QStringLiteral("复位失败：请检查轴连接/回零配置"));
        m_hintLabel->setStyleSheet("color: #ff5e6b; font-size: 12px; background: transparent; border: none;");
    }
}

void AutoRunPage::OnStopClicked()
{
    if (m_worker) m_worker->Stop();
    m_btnStart->setEnabled(true);
    m_statusLabel->setText(QStringLiteral("⏸ 已停止"));
    m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #f7c948; padding: 6px 0; background: transparent; border: none;");
}

void AutoRunPage::OnInitClicked()
{
    auto& hw = HardwareManager::instance();
    bool ok = hw.Initialize();
    if (ok) {
        m_logTextEdit->append(QStringLiteral("[%1] 初始化完成").arg(QDateTime::currentDateTime().toString("HH:mm:ss")));
    } else {
        m_hintLabel->setText(QStringLiteral("初始化失败：请检查硬件连接与配置"));
        m_hintLabel->setStyleSheet("color: #ff5e6b; font-size: 12px; background: transparent; border: none;");
    }
}

void AutoRunPage::OnEmergencyTriggered()
{
    // 仅 UI 响应：硬件断使能与 worker 中断由全局急停触发点（MainWindow）完成
    m_btnStart->setEnabled(true);
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
    m_statusLabel->setText(QStringLiteral("▶ %1").arg(name));
    m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #7ed67e; padding: 6px 0; background: transparent; border: none;");
}

void AutoRunPage::OnSchemeFinished()
{
    m_btnStart->setEnabled(true);
    m_statusLabel->setText(QStringLiteral("✅ 完成"));
    m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #7ed67e; padding: 6px 0; background: transparent; border: none;");
    m_hintLabel->setText(QStringLiteral("方案执行完成"));
    m_hintLabel->setStyleSheet("color: #8fd4ff; font-size: 12px; background: transparent; border: none;");
}

void AutoRunPage::OnInterrupted(const QString& reason)
{
    m_btnStart->setEnabled(true);
    m_statusLabel->setText(QStringLiteral("⏸ 中断"));
    m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #f7c948; padding: 6px 0; background: transparent; border: none;");
    m_logTextEdit->append(QStringLiteral("[%1] 中断: %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), reason));
}

void AutoRunPage::OnError(const QString& message)
{
    m_logTextEdit->append(QStringLiteral("[%1] 错误: %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), message));
    // 方案失败后恢复启动按钮与状态标签（否则永久置灰）
    if (m_btnStart) m_btnStart->setEnabled(true);
    m_statusLabel->setText(QStringLiteral("⛔ 错误"));
    m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #ff5e6b; padding: 6px 0; background: transparent; border: none;");
}