// AutoRunPage.cpp
#include "AutoRunPage.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QDebug>
#include <QSizePolicy>
#include "spdlog/spdlog.h"

AutoRunPage::AutoRunPage(QWidget* parent)
    : QWidget(parent)
{
    SetupUI();
}

void AutoRunPage::SetupUI()
{
    setStyleSheet("background: #262c34;");

    // ==== 主布局: 左右比例 6:4 ====
    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(14, 14, 14, 14);
    mainLayout->setSpacing(16);

    // ---- 左侧 (60%) ----
    auto* leftSide = new QWidget();
    // 关键: 设置 sizePolicy 使拉伸因子生效
    leftSide->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* leftLayout = new QVBoxLayout(leftSide);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(10);

    // 相机样式
    const QString cameraStyle = R"(
        QWidget {
            background: #0b0d0f;
            border-radius: 12px;
            border: 1px solid #3a424e;
        }
    )";
    const QString crossStyle = R"(
        QLabel {
            color: #1eff7a;
            font-size: 40px;
            opacity: 0.5;
            background: transparent;
            border: none;
        }
    )";
    const QString placeholderStyle = R"(
        QLabel {
            color: #556677;
            font-size: 16px;
            background: transparent;
            border: none;
        }
    )";

    // 两个相机画面各占 50% 高度
    QStringList placeholders = {
        QStringLiteral("RGB 相机画面 (3D)"),
        QStringLiteral("识别结果叠加图")
    };

    for (int i = 0; i < placeholders.size(); ++i)
    {
        auto* camBox = new QWidget();
        camBox->setStyleSheet(cameraStyle);
        camBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        auto* camLayout = new QVBoxLayout(camBox);
        camLayout->setAlignment(Qt::AlignCenter);
        camLayout->setSpacing(2);

        auto* cross = new QLabel(QStringLiteral("\xE2\x9C\x9B"));  // ✛
        cross->setStyleSheet(crossStyle);
        cross->setAlignment(Qt::AlignCenter);

        auto* placeholder = new QLabel(placeholders[i]);
        placeholder->setStyleSheet(placeholderStyle);
        placeholder->setAlignment(Qt::AlignCenter);

        camLayout->addWidget(cross);
        camLayout->addWidget(placeholder);

        leftLayout->addWidget(camBox, 1);  // 两个各占 50%
    }

    // ---- 右侧 (40%) ----
    auto* rightSide = new QWidget();
    rightSide->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* rightLayout = new QVBoxLayout(rightSide);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(10);

    // ---- LCD 行 (节拍 + 产量) ----
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
        val->setStyleSheet(R"(
            font-size: 32px; font-weight: 700; color: #cde2ff;
            font-family: 'Consolas', monospace;
            background: transparent; border: none;
        )");
        val->setAlignment(Qt::AlignCenter);

        lcdLayout->addWidget(lbl);
        lcdLayout->addWidget(val);
        lcdRow->addWidget(lcdWidget);
    }

    rightLayout->addLayout(lcdRow);

    // ---- 状态标签 ----
    m_statusLabel = new QLabel(QStringLiteral("\xE2\x96\xB6 运行中"));
    m_statusLabel->setStyleSheet("font-size: 28px; font-weight: 700; color: #7ed67e; padding: 6px 0; background: transparent; border: none;");
    rightLayout->addWidget(m_statusLabel);

    // ---- 坐标面板 ----
    auto* coordPanel = new QWidget();
    coordPanel->setStyleSheet("background: #0d141c; border-radius: 10px; padding: 8px 16px; border: 1px solid #2f7fb5;");
    coordPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* coordLayout = new QHBoxLayout(coordPanel);
    coordLayout->setSpacing(20);

    struct Coord { QString name; double value; QString unit; };
    QVector<Coord> coords = {
        { QStringLiteral("X"), 145.23, QStringLiteral("mm") },
        { QStringLiteral("Y"), 87.46,  QStringLiteral("mm") },
        { QStringLiteral("Z"), 32.81,  QStringLiteral("mm") },
        { QStringLiteral("R"), 12.50,  QStringLiteral("\xC2\xB0") },
    };

    for (const auto& c : coords)
    {
        auto* coordLabel = new QLabel(
            QStringLiteral("<span style='color:#7c9fc0;font-weight:400;'>%1: </span>"
                "<span style='color:#7ed6ff;font-weight:700;'>%2</span> %3")
            .arg(c.name).arg(c.value, 0, 'f', 2).arg(c.unit));
        coordLabel->setStyleSheet("font-weight: 600; font-size: 18px; color: #b7d6ff; background: transparent; border: none;");
        coordLayout->addWidget(coordLabel);
    }

    rightLayout->addWidget(coordPanel);

    // ---- 日志框 ----
    m_logBox = new QWidget();
    m_logBox->setStyleSheet("background: #12161c; border-radius: 10px; padding: 8px 12px; border: 1px solid #3a424e;");
    m_logBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* logLayout = new QVBoxLayout(m_logBox);
    logLayout->setSpacing(2);

    struct LogLine { QString text; QString color; };
    QVector<LogLine> logs = {
        { QStringLiteral("[10:23:15] 视觉定位完成"),    QStringLiteral("#8fcbff") },
        { QStringLiteral("[10:23:20] 灌装压力稳定"),    QStringLiteral("#f7c948") },
        { QStringLiteral("[10:22:50] 料盘到位超时"),    QStringLiteral("#ff5e6b") },
        { QStringLiteral("[10:23:40] 抓取放置成功"),    QStringLiteral("#8fcbff") },
    };

    for (const auto& log : logs)
    {
        auto* logLabel = new QLabel(log.text);
        logLabel->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 13px; font-family: 'Consolas', monospace; "
            "background: transparent; border: none;"
        ).arg(log.color));
        logLayout->addWidget(logLabel);
    }

    logLayout->addStretch();

    rightLayout->addWidget(m_logBox, 1);  // 日志区域可伸缩

    // ---- 按钮行 ----
    auto* btnRow = new QWidget();
    btnRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* btnLayout = new QHBoxLayout(btnRow);
    btnLayout->setContentsMargins(0, 0, 0, 0);
    btnLayout->setSpacing(10);

    struct BtnDef { QString text; QString bg; QString hover; QString extra; };
    QVector<BtnDef> btns = {
        { QStringLiteral("\xE2\x96\xB6 启动"),   QStringLiteral("#1f9d4a"), QStringLiteral("#28b85a"), QString() },
        { QStringLiteral("\xE2\x86\xBA 复位"),   QStringLiteral("#c78f1a"), QStringLiteral("#e0a520"), QStringLiteral("color: #1a1e24;") },
        { QStringLiteral("\xE2\x8F\xB9 停止"),   QStringLiteral("#b13a3a"), QStringLiteral("#d14444"), QString() },
        { QStringLiteral("\xE2\x9F\xB3 初始化"), QStringLiteral("#2f6f9f"), QStringLiteral("#3a84b8"), QString() },
        { QStringLiteral("\xE2\x9B\x94 急停"),   QStringLiteral("#cc2222"), QStringLiteral("#ee3333"), QString() },
    };

    for (const auto& b : btns)
    {
        auto* btn = new QPushButton(b.text);
        QString style = QStringLiteral(
            "QPushButton {"
            "  background: %1; border: none; border-radius: 8px;"
            "  font-weight: 700; font-size: 13px; padding: 0 4px; min-width: 0; color: white; %2"
            "}"
            "QPushButton:hover { background: %3; }"
        ).arg(b.bg, b.extra, b.hover);
        btn->setStyleSheet(style);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        btn->setFixedHeight(46);

        if (b.text.contains("启动"))   connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnStartClicked);
        else if (b.text.contains("复位"))   connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnResetClicked);
        else if (b.text.contains("停止"))   connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnStopClicked);
        else if (b.text.contains("初始化")) connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnInitClicked);
        else                                connect(btn, &QPushButton::clicked, this, &AutoRunPage::OnEmergencyClicked);

        btnLayout->addWidget(btn, 1);  // 等宽分配，最小化时不互相挤压
    }

    rightLayout->addWidget(btnRow);

    // ==== 组装主布局: 严格按照 6:4 比例 ====
    mainLayout->addWidget(leftSide, 6);
    mainLayout->addWidget(rightSide, 4);
}

void AutoRunPage::OnStartClicked()
{
    SPDLOG_INFO("[AutoRun] 启动 clicked (stub)");
    qDebug() << "按钮被点击: 启动";
}

void AutoRunPage::OnResetClicked()
{
    SPDLOG_INFO("[AutoRun] 复位 clicked (stub)");
    qDebug() << "按钮被点击: 复位";
}

void AutoRunPage::OnStopClicked()
{
    SPDLOG_INFO("[AutoRun] 停止 clicked (stub)");
    qDebug() << "按钮被点击: 停止";
}

void AutoRunPage::OnInitClicked()
{
    SPDLOG_INFO("[AutoRun] 初始化 clicked (stub)");
    qDebug() << "按钮被点击: 初始化";
}

void AutoRunPage::OnEmergencyClicked()
{
    SPDLOG_INFO("[AutoRun] 急停 clicked (stub)");
    qDebug() << "按钮被点击: 急停";
}