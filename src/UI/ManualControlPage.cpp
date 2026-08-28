#include "ManualControlPage.h"

#include "HAL/core/HardwareManager.h"
#include "HAL/core/AxisMap.h"
#include "KinematicsHelper.h"
#include "spdlog/spdlog.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QFont>
#include <QScrollArea>
#include <QScrollBar>
#include <QMessageBox>
#include <QDebug>
#include <QStringList>

ManualControlPage::ManualControlPage(QWidget* parent)
    : QWidget(parent)
{
    SetupUI();

    // 接线 HardwareManager 状态信号
    connect(&HardwareManager::instance(), &HardwareManager::stateUpdated,
            this, &ManualControlPage::OnStateUpdated);
    connect(&HardwareManager::instance(), &HardwareManager::servoStateUpdated,
            this, &ManualControlPage::OnServoStateUpdated);
    connect(&HardwareManager::instance(), &HardwareManager::connectionChanged,
            this, &ManualControlPage::OnConnectionChanged);
    connect(&HardwareManager::instance(), &HardwareManager::axisAlarm,
            this, &ManualControlPage::OnAxisAlarm);
    connect(&HardwareManager::instance(), &HardwareManager::limitTriggered,
            this, &ManualControlPage::OnLimitTriggered);
    connect(&HardwareManager::instance(), &HardwareManager::softLimitTriggered,
            this, &ManualControlPage::OnSoftLimit);
    connect(&HardwareManager::instance(), &HardwareManager::enableStateChanged,
            this, &ManualControlPage::OnEnableStateChanged);
    connect(&HardwareManager::instance(), &HardwareManager::axisMoveFinished,
            this, &ManualControlPage::OnAxisMoveFinished);

    const int count = static_cast<int>(LogicalAxis::Count);
    alarmState_.fill(false, count);
    limitState_.fill(false, count);
    softLimitDir_.fill(0, count);
    alarmDetail_.fill(QString(), count);
    enabledState_.fill(false, count);
    runningState_.fill(false, count);
    homeDoneState_.fill(false, count);
    homingAxes_.fill(false, count);

    // 初始连接状态
    OnConnectionChanged();
}

void ManualControlPage::OnConnectionChanged()
{
    if (!connStatusLabel_) return;
    bool cardOk = HardwareManager::instance().IsMotionCardConnected();
    bool servoOk = HardwareManager::instance().IsServoConnected();
    QString text = QStringLiteral("运动卡: %1 | 舵机: %2")
                       .arg(cardOk ? QStringLiteral("已连接") : QStringLiteral("未连接"),
                            servoOk ? QStringLiteral("已连接") : QStringLiteral("未连接"));
    connStatusLabel_->setText(text);
    connStatusLabel_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; font-weight: 600; background: transparent; border: none; padding: 0 6px;")
            .arg((cardOk || servoOk) ? QStringLiteral("#7ed67e") : QStringLiteral("#e0a520")));

    // 从配置回填每轴点动速度（Initialize 完成后 connectionChanged 会再次触发）
    for (int i = 0; i < speedSpins_.size(); ++i) {
        if (i >= static_cast<int>(LogicalAxis::Count)) break;
        double jogSpeed = HardwareManager::instance().GetJogSpeed(static_cast<LogicalAxis>(i));
        double maxSpeed = HardwareManager::instance().GetMaxSpeed(static_cast<LogicalAxis>(i));
        if (maxSpeed > 0.0 && jogSpeed > maxSpeed) jogSpeed = maxSpeed; // 不显示超限值
        speedSpins_[i]->blockSignals(true);
        speedSpins_[i]->setValue(jogSpeed);
        speedSpins_[i]->blockSignals(false);
    }
}

void ManualControlPage::OnEnableStateChanged()
{
    // 使能状态变化 → 刷新所有轴状态灯
    for (int i = 0; i < statusDots_.size(); ++i)
        RefreshStatusDot(i);
}

void ManualControlPage::SetupUI()
{
    setStyleSheet("background: #262c34;");

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(14, 14, 14, 14);
    mainLayout->setSpacing(12);
    mainLayout->setSizeConstraint(QLayout::SetDefaultConstraint);

    // 内容放入 QScrollArea：窗口变窄时允许横向滚动，避免右侧坐标数值被遮挡
    auto* scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setStyleSheet(R"(
        QScrollArea { background: transparent; border: none; }
        QScrollArea > QWidget > QWidget { background: transparent; }
        QScrollBar:horizontal { background: #1b1f26; height: 10px; border-radius: 5px; }
        QScrollBar::handle:horizontal { background: #3a424e; border-radius: 5px; min-width: 40px; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
    )");

    auto* content = new QWidget();
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(12);

    auto* layout = contentLayout;

    // ==== Top bar ====
    auto* teachTop = new QWidget();
    teachTop->setStyleSheet("background: #1b222b; padding: 10px 16px; border-radius: 12px;");
    // 禁止顶栏在垂直方向上被拉伸
    teachTop->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* topLayout = new QVBoxLayout(teachTop);
    topLayout->setContentsMargins(16, 10, 16, 10);
    topLayout->setSpacing(10);

    // 行A：全局操作按钮 + 连接状态
    auto* btnRow = new QWidget();
    auto* btnLayout = new QHBoxLayout(btnRow);
    btnLayout->setContentsMargins(0, 0, 0, 0);
    btnLayout->setSpacing(16);

    auto* enableBtn = new QPushButton(QStringLiteral("全局轴使能"));
    enableBtn->setStyleSheet("QPushButton { background: #1f9d4a; border: none; border-radius: 10px; padding: 10px 18px; font-weight: 600; font-size: 15px; color: white; } QPushButton:hover { background: #28b85a; }");
    enableBtn->setCursor(Qt::PointingHandCursor);
    connect(enableBtn, &QPushButton::clicked, this, &ManualControlPage::OnGlobalEnable);

    auto* disableBtn = new QPushButton(QStringLiteral("全局断使能"));
    disableBtn->setStyleSheet("QPushButton { background: #b13a3a; border: none; border-radius: 10px; padding: 10px 18px; font-weight: 600; font-size: 15px; color: white; } QPushButton:hover { background: #d14444; }");
    disableBtn->setCursor(Qt::PointingHandCursor);
    connect(disableBtn, &QPushButton::clicked, this, &ManualControlPage::OnGlobalDisable);

    auto* homeAllBtn = new QPushButton(QStringLiteral("一键回零"));
    homeAllBtn->setStyleSheet("QPushButton { background: #2f8f5f; border: none; border-radius: 10px; padding: 10px 18px; font-weight: 600; font-size: 15px; color: white; } QPushButton:hover { background: #38a870; }");
    homeAllBtn->setCursor(Qt::PointingHandCursor);
    connect(homeAllBtn, &QPushButton::clicked, this, &ManualControlPage::OnGlobalHome);

    auto* estopBtn = new QPushButton(QStringLiteral("\xE2\x9A\xA0 急停"));
    estopBtn->setStyleSheet("QPushButton { background: #d53a3a; border: none; border-radius: 10px; padding: 10px 18px; font-weight: 700; font-size: 15px; color: white; } QPushButton:hover { background: #ef4a4a; }");
    estopBtn->setCursor(Qt::PointingHandCursor);
    connect(estopBtn, &QPushButton::clicked, this, [this]() {
        SPDLOG_INFO("[ManualControl] 急停 clicked");
        HardwareManager::instance().EmergencyStop();
        ResetAxisStates();
        SetHint(QStringLiteral("已触发急停，所有轴立即停止"));
    });

    btnLayout->addWidget(enableBtn);
    btnLayout->addWidget(disableBtn);
    btnLayout->addWidget(homeAllBtn);
    btnLayout->addWidget(estopBtn);

    connStatusLabel_ = new QLabel();
    connStatusLabel_->setStyleSheet("color: #8da3bb; font-size: 13px; font-weight: 600; background: transparent; border: none; padding: 0 6px;");
    btnLayout->addWidget(connStatusLabel_);

    btnLayout->addStretch();
    topLayout->addWidget(btnRow);

    // 行B：坐标面板（独立一行，缩窄窗口时不会被右侧裁剪）
    auto* coordRow = new QWidget();
    auto* coordRowLayout = new QHBoxLayout(coordRow);
    coordRowLayout->setContentsMargins(0, 0, 0, 0);
    coordRowLayout->setSpacing(0);

    // Coord panel
    auto* coordPanel = new QWidget();
    coordPanel->setStyleSheet("background: #0d141c; border-radius: 10px; padding: 4px 14px; border: 1px solid #2f7fb5;");
    auto* coordLayout = new QHBoxLayout(coordPanel);
    coordLayout->setSpacing(14);

    struct Coord { QString name; QString unit; };
    QVector<Coord> coords = {
        { QStringLiteral("X"), QString() },
        { QStringLiteral("Y"), QString() },
        { QStringLiteral("Z"), QString() },
        { QStringLiteral("R"), QStringLiteral("\xC2\xB0") },
    };

    for (const auto& c : coords)
    {
        // 初始 0.00，由 stateUpdated/servoStateUpdated 驱动 FK 实时刷新
        // （曾为硬编码假数据且从不更新）
        auto* label = new QLabel(
            QStringLiteral("<span style='color:#7c9fc0;'>%1: </span>"
                           "<span style='color:#7ed6ff;font-weight:700;font-size:15px;'>%2%3</span>")
                .arg(c.name).arg(0.0, 0, 'f', 2).arg(c.unit));
        label->setStyleSheet("font-size: 15px; color: #b7d6ff; background: transparent; border: none;");
        coordLayout->addWidget(label);
        coordLabels_.append(label);
    }

    coordRowLayout->addWidget(coordPanel);
    coordRowLayout->addStretch();
    topLayout->addWidget(coordRow);

    layout->addWidget(teachTop);

    // ==== Axis table ====
    // Use QGridLayout to simulate the table
    struct AxisInfo {
        QString label;
        QString typeIcon;
        QString typeStyle;
        double speed;
        QString pos;
        double target;
        bool isGripper;
        bool isExtrude;
    };

    QVector<AxisInfo> axes = {
        { QStringLiteral("轴1(J1)"), QStringLiteral("\xE2\x9A\x99"), R"(color: #6a9fc0; font-size: 18px;)", 100, QStringLiteral("12.5\xC2\xB0"), 30.0, false, false },
        { QStringLiteral("轴2(J2)"), QStringLiteral("\xF0\x9F\x94\xA7"), R"(color: #c0a06a; font-size: 18px;)", 80, QStringLiteral("-8.2\xC2\xB0"), 0.0, false, false },
        { QStringLiteral("轴3(Z)"),  QStringLiteral("\xE2\x9A\x99"), R"(color: #6a9fc0; font-size: 18px;)", 120, QStringLiteral("32.8 mm"), 25.0, false, false },
        { QStringLiteral("轴4(R)"),  QStringLiteral("\xF0\x9F\x94\xA7"), R"(color: #c0a06a; font-size: 18px;)", 90, QStringLiteral("15.0\xC2\xB0"), 10.0, false, false },
        { QStringLiteral("轴5(夹爪)"), QStringLiteral("\xE2\x9A\x99"), R"(color: #6a9fc0; font-size: 18px;)", 50, QStringLiteral("0.0 mm"), 5.0, true, false },
        { QStringLiteral("轴6(挤出)"), QStringLiteral("\xE2\x9A\x99"), R"(color: #6a9fc0; font-size: 18px;)", 60, QStringLiteral("0.0 mm"), 3.5, false, true },
    };


    // Header labels
    QStringList headers = {
        QStringLiteral("轴"), QStringLiteral("类型"), QStringLiteral("速度"),
        QStringLiteral("点动"), QStringLiteral("当前位置"), QStringLiteral("点动"),
        QStringLiteral("目标位置"), QStringLiteral("移动"), QStringLiteral("停止"), QStringLiteral("回零"),
        QStringLiteral("状态")
    };

    auto* tableWidget = new QWidget();
    tableWidget->setStyleSheet("background: transparent;");
    tableWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* tableLayout = new QVBoxLayout(tableWidget);
    tableLayout->setContentsMargins(0, 0, 0, 0);
    tableLayout->setSpacing(6);

    // 【核心修复1】：把固定宽度变成了“拉伸比例因子” (数值代表相对比例)
    // 列序：轴 / 类型图标 / 速度 / 点动- / 当前位置 / 点动+ / 目标位置 / Go / 停止 / 回零 / 状态
    // 2026-08: 类型图标列 40（图标留白，与轴名同列对齐）；速度列 110、目标列 100（容纳数值+框外单位 mm/s、°/s）
    QVector<int> colStretches = { 85, 40, 110, 75, 90, 75, 100, 75, 60, 55, 34 };

    // Header row
    auto* headerWidget = new QWidget();
    headerWidget->setStyleSheet("background: #242e3a; border-radius: 8px;");
    auto* headerGrid = new QHBoxLayout(headerWidget);
    headerGrid->setContentsMargins(12, 8, 12, 8);
    headerGrid->setSpacing(12);

    for (int i = 0; i < headers.size(); ++i)
    {
        auto* hdr = new QLabel(headers[i]);
        hdr->setStyleSheet("color: #9bb3cf; font-weight: 500; font-size: 13px; background: transparent; border: none; padding: 0 2px;");
        // 【核心修复2】：强制忽略控件自身尺寸，听从外层比例分配
        hdr->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        headerGrid->addWidget(hdr, colStretches[i]); // 传入比例因子
    }
    // 注意：删除了之前的 addStretch()，让内容自动铺满右侧
    tableLayout->addWidget(headerWidget);

    // Axis rows
    for (int i = 0; i < axes.size(); ++i)
    {
        const auto& axis = axes[i];

        // 单位：速度 mm/s 或 °/s；位置 mm 或 °（由轴类型决定，与 AxisUnit 一致）
        QString posUnit   = HardwareManager::instance().AxisUnit(static_cast<LogicalAxis>(i));
        QString speedUnit = posUnit.isEmpty() ? QString() : posUnit + QStringLiteral("/s");
        auto* rowWidget = new QWidget();
        rowWidget->setStyleSheet("background: #1a2129; border-radius: 8px;");

        auto* rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(12, 8, 12, 8);
        rowLayout->setSpacing(12);

        // Col 0: Axis label
        auto* axisLabel = new QLabel(axis.label);
        axisLabel->setStyleSheet("font-weight: 600; color: #a5c1e0; font-size: 14px; background: transparent; border: none; padding: 0 2px;");
        axisLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        rowLayout->addWidget(axisLabel, colStretches[0]);

        // Col 1: Type icon
        auto* typeIcon = new QLabel(axis.typeIcon);
        typeIcon->setStyleSheet(axis.typeStyle + QStringLiteral(" background: transparent; border: none; padding: 0 2px;"));
        typeIcon->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        rowLayout->addWidget(typeIcon, colStretches[1]);

        // Col 2: Speed（spin + 单位标签同列，单位在框外）
        auto* speedSpin = new QDoubleSpinBox();
        speedSpin->setRange(0, 99999.99);
        speedSpin->setDecimals(1);
        speedSpin->setValue(axis.speed);
        // 注意：不要在 QDoubleSpinBox 的样式表里写 font-size —— 会触发 Qt 样式引擎
        // 在 reparent/polish 时崩溃(Qt6 QSS 已知问题)。用 setFont 实现同等视觉效果。
        speedSpin->setFont([&](){ QFont f = speedSpin->font(); f.setPointSizeF(9.75); return f; }());
        speedSpin->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px; border-radius: 6px;");
        speedSpin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* speedUnitLabel = new QLabel(speedUnit);
        speedUnitLabel->setStyleSheet("color: #8da3bb; font-size: 13px; font-weight: 500; background: transparent; border: none; padding: 0 2px;");
        speedUnitLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        auto* speedCell = new QWidget();
        auto* speedCellLay = new QHBoxLayout(speedCell);
        speedCellLay->setContentsMargins(0, 0, 0, 0);
        speedCellLay->setSpacing(4);
        speedCellLay->addWidget(speedSpin, 1);
        speedCellLay->addWidget(speedUnitLabel);
        speedSpins_.append(speedSpin);
        int speedIdx = i;
        connect(speedSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, speedIdx](double v) {
            if (speedIdx >= 0 && speedIdx < static_cast<int>(LogicalAxis::Count)) {
                double maxSpeed = HardwareManager::instance().GetMaxSpeed(static_cast<LogicalAxis>(speedIdx));
                if (maxSpeed > 0.0 && v > maxSpeed) {
                    // 超过「设备与配置 - 电控与映射」中的最大速度：弹提示并回退到最大值
                    speedSpins_[speedIdx]->blockSignals(true);
                    speedSpins_[speedIdx]->setValue(maxSpeed);
                    speedSpins_[speedIdx]->blockSignals(false);
                    QMessageBox::warning(this, QStringLiteral("速度超限"),
                        QStringLiteral("速度不能超过最大值%1").arg(maxSpeed));
                    return;
                }
                HardwareManager::instance().SetJogSpeed(static_cast<LogicalAxis>(speedIdx), v);
            }
        });
        rowLayout->addWidget(speedCell, colStretches[2]);

        // Col 3: Jog -
        auto* jogMinus = new QPushButton(axis.isExtrude ? QStringLiteral("\xE2\x88\x92 回抽") : (axis.isGripper ? QStringLiteral("\xE2\x88\x92 松开") : QStringLiteral("\xE2\x88\x92")));
        QString jogStyle = axis.isExtrude ? "#dcc093" : (axis.isGripper ? "#9fc0dd" : "#c6d4e4");
        QString jogHover = axis.isExtrude ? "#e8d0a9" : (axis.isGripper ? "#b3d1ea" : "#dbe5f2");
        QString jogText = "#202b38";
        jogMinus->setStyleSheet(QStringLiteral("QPushButton { background: %1; border: none; border-radius: 6px; color: %3; padding: 6px 10px; font-weight: 700; font-size: 14px; } QPushButton:hover { background: %2; }").arg(jogStyle, jogHover, jogText));
        jogMinus->setCursor(Qt::PointingHandCursor);
        jogMinus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        int axisIdx = i;
        connect(jogMinus, &QPushButton::pressed, this, [this, axisIdx]() { OnJogMinus(axisIdx); });
        connect(jogMinus, &QPushButton::released, this, [this, axisIdx]() { OnJogStop(axisIdx); });
        rowLayout->addWidget(jogMinus, colStretches[3]);

        // Col 4: Current position
        auto* posLabel = new QLabel(axis.pos);
        posLabel->setStyleSheet("background: #0d141c; padding: 4px 6px; border-radius: 6px; text-align: center; font-weight: 700; color: #cde2ff; font-size: 14px;");
        posLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        posLabels_.append(posLabel);
        rowLayout->addWidget(posLabel, colStretches[4]);

        // Col 5: Jog +
        auto* jogPlus = new QPushButton(axis.isExtrude ? QStringLiteral("+ 挤出") : (axis.isGripper ? QStringLiteral("+ 夹紧") : QStringLiteral("+")));
        jogPlus->setStyleSheet(QStringLiteral("QPushButton { background: %1; border: none; border-radius: 6px; color: %3; padding: 6px 10px; font-weight: 700; font-size: 14px; } QPushButton:hover { background: %2; }").arg(jogStyle, jogHover, jogText));
        jogPlus->setCursor(Qt::PointingHandCursor);
        jogPlus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        connect(jogPlus, &QPushButton::pressed, this, [this, axisIdx]() { OnJogPlus(axisIdx); });
        connect(jogPlus, &QPushButton::released, this, [this, axisIdx]() { OnJogStop(axisIdx); });
        rowLayout->addWidget(jogPlus, colStretches[5]);

        // Col 6: Target position（spin + 单位标签同列，单位在框外）
        auto* targetSpin = new QDoubleSpinBox();
        targetSpin->setRange(-99999.99, 99999.99);
        targetSpin->setDecimals(1);
        targetSpin->setValue(axis.target);
        targetSpin->setFont([&](){ QFont f = targetSpin->font(); f.setPointSizeF(9.75); return f; }());
        targetSpin->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px; border-radius: 6px;");
        targetSpin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* targetUnitLabel = new QLabel(posUnit);
        targetUnitLabel->setStyleSheet("color: #8da3bb; font-size: 13px; font-weight: 500; background: transparent; border: none; padding: 0 2px;");
        targetUnitLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        auto* targetCell = new QWidget();
        auto* targetCellLay = new QHBoxLayout(targetCell);
        targetCellLay->setContentsMargins(0, 0, 0, 0);
        targetCellLay->setSpacing(4);
        targetCellLay->addWidget(targetSpin, 1);
        targetCellLay->addWidget(targetUnitLabel);
        targetSpins_.append(targetSpin);
        rowLayout->addWidget(targetCell, colStretches[6]);

        // Col 7: Go button
        auto* goBtn = new QPushButton(QStringLiteral("Go"));
        QString goColor = "#2f6f8f";
        QString goHover = "#3a84a8";
        goBtn->setStyleSheet(QStringLiteral("QPushButton { background: %1; border: none; border-radius: 6px; color: white; padding: 6px 10px; font-weight: 600; font-size: 13px; } QPushButton:hover { background: %2; }").arg(goColor, goHover));
        goBtn->setCursor(Qt::PointingHandCursor);
        goBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        connect(goBtn, &QPushButton::clicked, this, [this, axisIdx]() { OnGoClicked(axisIdx); });
        goButtons_.append(goBtn);
        rowLayout->addWidget(goBtn, colStretches[7]);

        // Col 8: Stop button
        auto* stopBtn = new QPushButton(QStringLiteral("停止"));
        stopBtn->setStyleSheet("QPushButton { background: #8f3f3f; border: none; border-radius: 6px; color: white; padding: 6px 10px; font-weight: 600; font-size: 13px; } QPushButton:hover { background: #aa4f4f; }");
        stopBtn->setCursor(Qt::PointingHandCursor);
        stopBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        connect(stopBtn, &QPushButton::clicked, this, [this, axisIdx]() { OnStopAxis(axisIdx); });
        rowLayout->addWidget(stopBtn, colStretches[8]);

        // Col 9: Home button
        auto* homeBtn = new QPushButton(QStringLiteral("回零"));
        homeBtn->setStyleSheet("QPushButton { background: #2f8f5f; border: none; border-radius: 6px; color: white; padding: 6px 10px; font-weight: 600; font-size: 13px; } QPushButton:hover { background: #38a870; }");
        homeBtn->setCursor(Qt::PointingHandCursor);
        homeBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        connect(homeBtn, &QPushButton::clicked, this, [this, axisIdx]() { OnHomeAxis(axisIdx); });
        rowLayout->addWidget(homeBtn, colStretches[9]);

        // Col 10: Status dot
        auto* statusDot = new QLabel();
        statusDot->setFixedSize(12, 12);
        statusDot->setStyleSheet("background: #5a6a7a; border-radius: 6px;");
        statusDot->setToolTip(QStringLiteral("未使能"));
        statusDots_.append(statusDot);
        rowLayout->addWidget(statusDot, colStretches[10], Qt::AlignCenter);

        // 注意：这里也删除了 rowLayout->addStretch();
        tableLayout->addWidget(rowWidget);
    }

    layout->addWidget(tableWidget);

    // ==== Bottom hint ====
    hintLabel_ = new QLabel(QStringLiteral("提示：按住 +/- 按钮持续运动，松开停止"));
    hintLabel_->setStyleSheet("color: #8fd4ff; font-size: 13px; font-weight: 600; background: transparent; border: none; padding: 8px 4px;");
    layout->addWidget(hintLabel_);

    layout->addStretch();

    scrollArea->setWidget(content);
    mainLayout->addWidget(scrollArea);
}

void ManualControlPage::OnGlobalEnable()
{
    SPDLOG_INFO("[ManualControl] 全局使能 clicked");
    bool connected = HardwareManager::instance().IsMotionCardConnected()
                     && HardwareManager::instance().IsServoConnected();
    bool ok = HardwareManager::instance().EnableAll();
    SPDLOG_INFO("[ManualControl] 全局使能 result: ok={} connected={}", ok, connected);
    if (!connected) SetHint(QStringLiteral("未连接硬件，命令可能无效"), "#e0a520");
    else if (!ok)   SetHint(QStringLiteral("部分轴使能失败"));
    else            SetHint(QStringLiteral("全局轴使能完成"));
    qDebug() << "全局轴使能";
}

void ManualControlPage::OnGlobalDisable()
{
    SPDLOG_INFO("[ManualControl] 全局断使能 clicked");
    bool connected = HardwareManager::instance().IsMotionCardConnected()
                     || HardwareManager::instance().IsServoConnected();
    bool ok = HardwareManager::instance().DisableAll();
    SPDLOG_INFO("[ManualControl] 全局断使能 result: ok={} connected={}", ok, connected);
    if (!connected) SetHint(QStringLiteral("未连接硬件，命令可能未生效"));
    else if (!ok)   SetHint(QStringLiteral("部分轴断使能失败"));
    else            SetHint(QStringLiteral("全局断使能完成"));
    qDebug() << "全局断使能";
}

void ManualControlPage::OnGlobalHome()
{
    SPDLOG_INFO("[ManualControl] 一键回零 clicked");
    bool connected = HardwareManager::instance().IsMotionCardConnected()
                     || HardwareManager::instance().IsServoConnected();
    // 需求3：全局轴使能未执行 / 全局断使能执行后，不允许一键回零
    if (!HardwareManager::instance().IsGlobalEnabled()) {
        SPDLOG_WARN("[ManualControl] 一键回零 rejected: 未全局使能");
        SetHint(QStringLiteral("请先执行全局轴使能，再进行一键回零"), "#e0a520");
        return;
    }
    if (!connected) { SPDLOG_WARN("[ManualControl] 一键回零 rejected: 未连接硬件"); SetHint(QStringLiteral("未连接硬件，无法回零")); return; }
    // 逐轴发起回零（HomeAll 的使能门禁已在上文校验），仅对真正启动回零的轴标记 homingAxes_；
    // 被拒轴（报警/忙/卡端 reject）逐轴列出，便于区分"回零中"与"未启动"
    bool anyStarted = false;
    QStringList notStarted;
    for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i) {
        if (HardwareManager::instance().HomeAxis(static_cast<LogicalAxis>(i))) {
            if (i < homingAxes_.size()) homingAxes_[i] = true;
            anyStarted = true;
        } else {
            notStarted << QStringLiteral("轴%1").arg(i + 1);
        }
    }
    if (!anyStarted) {
        SetHint(QStringLiteral("回零被拒（请先使能或检查轴状态）"), QStringLiteral("#e0a520"));
    } else if (!notStarted.isEmpty()) {
        SetHint(QStringLiteral("回零中...（%1 未启动）").arg(notStarted.join(QStringLiteral("、"))),
                QStringLiteral("#e0a520"));
    } else {
        SetHint(QStringLiteral("回零中..."));
    }
    qDebug() << "一键回零";
}

void ManualControlPage::OnJogMinus(int axis)
{
    if (axis < 0 || axis >= static_cast<int>(LogicalAxis::Count)) return;
    double speed = (axis < speedSpins_.size()) ? speedSpins_[axis]->value() : 20.0;
    double maxSpeed = HardwareManager::instance().GetMaxSpeed(static_cast<LogicalAxis>(axis));
    if (maxSpeed > 0.0 && speed > maxSpeed) speed = maxSpeed;
    SPDLOG_INFO("[ManualControl] 点动- axis={} speed={}", axis, speed);
    if (HardwareManager::instance().MoveJog(static_cast<LogicalAxis>(axis), speed, -1)) {
        runningState_[axis] = true;
        if (axis < softLimitDir_.size()) softLimitDir_[axis] = 0;
        RefreshStatusDot(axis);
        RefreshSoftLimitHint();
    } else {
        // 使能门禁 / 报警 / 回零 / 软限位拒绝
        if (!HardwareManager::instance().IsAxisEnabled(static_cast<LogicalAxis>(axis)))
            SetHint(QStringLiteral("请先执行全局轴使能，再进行点动"), "#e0a520");
        else if (axis < alarmState_.size() && alarmState_[axis])
            SetHint(QStringLiteral("轴%1 处于报警状态，禁止点动").arg(axis + 1), "#e0a520");
        else if (HardwareManager::instance().IsAxisBusy(static_cast<LogicalAxis>(axis)))
            SetHint(QStringLiteral("轴%1 正在运动/回零，请先停止").arg(axis + 1), "#e0a520");
    }
    qDebug() << "JOG - 轴" << axis;
    qDebug() << "速度" << speed;
}

void ManualControlPage::OnJogPlus(int axis)
{
    if (axis < 0 || axis >= static_cast<int>(LogicalAxis::Count)) return;
    double speed = (axis < speedSpins_.size()) ? speedSpins_[axis]->value() : 20.0;
    double maxSpeed = HardwareManager::instance().GetMaxSpeed(static_cast<LogicalAxis>(axis));
    if (maxSpeed > 0.0 && speed > maxSpeed) speed = maxSpeed;
    SPDLOG_INFO("[ManualControl] 点动+ axis={} speed={}", axis, speed);
    if (HardwareManager::instance().MoveJog(static_cast<LogicalAxis>(axis), speed, 1)) {
        runningState_[axis] = true;
        if (axis < softLimitDir_.size()) softLimitDir_[axis] = 0;
        RefreshStatusDot(axis);
        RefreshSoftLimitHint();
    } else {
        if (!HardwareManager::instance().IsAxisEnabled(static_cast<LogicalAxis>(axis)))
            SetHint(QStringLiteral("请先执行全局轴使能，再进行点动"), "#e0a520");
        else if (axis < alarmState_.size() && alarmState_[axis])
            SetHint(QStringLiteral("轴%1 处于报警状态，禁止点动").arg(axis + 1), "#e0a520");
        else if (HardwareManager::instance().IsAxisBusy(static_cast<LogicalAxis>(axis)))
            SetHint(QStringLiteral("轴%1 正在运动/回零，请先停止").arg(axis + 1), "#e0a520");
    }
    qDebug() << "JOG + 轴" << axis;
}

void ManualControlPage::OnJogStop(int axis)
{
    if (axis < 0 || axis >= static_cast<int>(LogicalAxis::Count)) return;
    SPDLOG_INFO("[ManualControl] 点动停止 axis={}", axis);
    HardwareManager::instance().StopJog(static_cast<LogicalAxis>(axis));
    runningState_[axis] = false;
    if (axis < goButtons_.size()) goButtons_[axis]->setEnabled(true);
    RefreshStatusDot(axis);
    qDebug() << "JOG 停止 轴" << axis;
}

void ManualControlPage::OnGoClicked(int axis)
{
    if (axis < 0 || axis >= static_cast<int>(LogicalAxis::Count)) return;
    if (axis < targetSpins_.size()) {
        double target = targetSpins_[axis]->value();
        SPDLOG_INFO("[ManualControl] Go clicked axis={} target={}", axis, target);
        // 软限位：目标超出范围直接拒绝（HardwareManager::MoveAbs 也会再拦一道）
        double lo = HardwareManager::instance().GetLimitMin(static_cast<LogicalAxis>(axis));
        double hi = HardwareManager::instance().GetLimitMax(static_cast<LogicalAxis>(axis));
        if (lo < hi && (target < lo - 1e-6 || target > hi + 1e-6)) {
            SetHint(QStringLiteral("目标位置超出软限位范围（%1 ~ %2）").arg(lo).arg(hi), "#e0a520");
            return;
        }
        // 移动速度使用界面速度输入框（与点动一致），超 maxSpeed 时截断
        double goSpeed = (axis < speedSpins_.size()) ? speedSpins_[axis]->value() : 20.0;
        double maxSpeed = HardwareManager::instance().GetMaxSpeed(static_cast<LogicalAxis>(axis));
        if (maxSpeed > 0.0 && goSpeed > maxSpeed) goSpeed = maxSpeed;
        if (HardwareManager::instance().MoveAbs(static_cast<LogicalAxis>(axis), target, goSpeed)) {
            if (axis < softLimitDir_.size()) softLimitDir_[axis] = 0;
            // 运动中置灰 Go，防连点打断/指令覆盖（曾引发舵机突然加速）
            if (axis < goButtons_.size()) goButtons_[axis]->setEnabled(false);
            RefreshStatusDot(axis);
            RefreshSoftLimitHint();
        } else {
            if (!HardwareManager::instance().IsAxisEnabled(static_cast<LogicalAxis>(axis)))
                SetHint(QStringLiteral("请先执行全局轴使能，再进行移动"), "#e0a520");
            else if (axis < alarmState_.size() && alarmState_[axis])
                SetHint(QStringLiteral("轴%1 处于报警状态，禁止移动").arg(axis + 1), "#e0a520");
            else if (HardwareManager::instance().IsAxisBusy(static_cast<LogicalAxis>(axis)))
                SetHint(QStringLiteral("轴%1 正在运动/回零，请先停止").arg(axis + 1), "#e0a520");
        }
    }
    qDebug() << "Go 轴" << axis;
}

void ManualControlPage::OnStopAxis(int axis)
{
    if (axis < 0 || axis >= static_cast<int>(LogicalAxis::Count)) return;
    SPDLOG_INFO("[ManualControl] 停止 axis={}", axis);
    HardwareManager::instance().StopAxis(static_cast<LogicalAxis>(axis));
    runningState_[axis] = false;
    if (axis < goButtons_.size()) goButtons_[axis]->setEnabled(true);
    // 停止也可能中止回零：清除本轴回零标记，避免提示一直卡在"回零中"
    if (axis >= 0 && axis < homingAxes_.size() && homingAxes_[axis]) {
        homingAxes_[axis] = false;
        bool anyRemaining = false;
        for (int i = 0; i < homingAxes_.size(); ++i) {
            if (homingAxes_[i]) { anyRemaining = true; break; }
        }
        SetHint(anyRemaining ? QStringLiteral("回零中...") : QStringLiteral("回零已停止"));
    }
    RefreshStatusDot(axis);
    qDebug() << "停止 轴" << axis;
}

void ManualControlPage::OnAxisMoveFinished(int axis)
{
    // Go 到位（或被打断/停止）→ 恢复 Go 按钮
    if (axis >= 0 && axis < goButtons_.size())
        goButtons_[axis]->setEnabled(true);
    // 回零完成：axisMoveFinished 由 HardwareManager 在回零超时/到位时发出（卡轴与舵机轴统一经此信号）。
    // 清除本轴回零标记；全部轴完成后提示"回零完成"，否则保持"回零中..."
    if (axis >= 0 && axis < homingAxes_.size() && homingAxes_[axis]) {
        homingAxes_[axis] = false;
        bool anyRemaining = false;
        for (int i = 0; i < homingAxes_.size(); ++i) {
            if (homingAxes_[i]) { anyRemaining = true; break; }
        }
        SetHint(anyRemaining ? QStringLiteral("回零中...") : QStringLiteral("回零完成"));
    }
}

void ManualControlPage::OnHomeAxis(int axis)
{
    if (axis < 0 || axis >= static_cast<int>(LogicalAxis::Count)) return;
    SPDLOG_INFO("[ManualControl] 回零 clicked axis={}", axis);
    if (!HardwareManager::instance().IsAxisEnabled(static_cast<LogicalAxis>(axis))) {
        SetHint(QStringLiteral("请先执行全局轴使能，再进行回零"), "#e0a520");
        return;
    }
    runningState_[axis] = false;
    homeDoneState_[axis] = false;
    bool ok = HardwareManager::instance().HomeAxis(static_cast<LogicalAxis>(axis));
    if (axis < homingAxes_.size()) homingAxes_[axis] = ok;
    if (ok)
        SetHint(QStringLiteral("轴%1 回零中...").arg(axis + 1));
    else
        SetHint(QStringLiteral("轴%1 回零被拒（请先使能或检查轴状态）").arg(axis + 1), "#e0a520");
    RefreshStatusDot(axis);
    qDebug() << "回零 轴" << axis;
}

void ManualControlPage::OnStateUpdated(const QVector<MotorStatus>& axes)
{
    for (const auto& st : axes) {
        if (st.axisId < 0 || st.axisId >= posLabels_.size()) continue;
        QString unit = HardwareManager::instance().AxisUnit(static_cast<LogicalAxis>(st.axisId));
        posLabels_[st.axisId]->setText(QStringLiteral("%1%2").arg(st.position, 0, 'f', 1).arg(unit));
        // FK 关节缓存：J1/Z 为卡轴（position 已是逻辑坐标）
        if (st.axisId == static_cast<int>(LogicalAxis::J1)) cachedJ1_ = st.position;
        else if (st.axisId == static_cast<int>(LogicalAxis::Z)) cachedZ_ = st.position;
        if (st.axisId < enabledState_.size()) {
            enabledState_[st.axisId]  = st.enabled;
            runningState_[st.axisId]  = st.running;
            homeDoneState_[st.axisId] = st.homeDone;
            // 告警 = 驱动器报警 / 跟随误差(失步) / 急停（硬软限位另算为"限位"）
            alarmState_[st.axisId]    = st.alarm || st.followError || st.estop;
            limitState_[st.axisId]    = st.limitPositive || st.limitNegative
                                        || st.softLimitPositive || st.softLimitNegative;
            // 组装告警详情 tooltip
            QStringList reasons;
            if (st.alarm)             reasons << QStringLiteral("驱动器报警");
            if (st.followError)       reasons << QStringLiteral("跟随误差(失步)");
            if (st.estop)             reasons << QStringLiteral("急停");
            if (st.limitPositive)     reasons << QStringLiteral("正硬限位");
            if (st.limitNegative)     reasons << QStringLiteral("负硬限位");
            if (st.softLimitPositive) reasons << QStringLiteral("正软限位");
            if (st.softLimitNegative) reasons << QStringLiteral("负软限位");
            alarmDetail_[st.axisId]   = reasons.join(QStringLiteral("、"));
            RefreshStatusDot(st.axisId);
        }
    }
    RefreshCoordPanel();
}

void ManualControlPage::OnServoStateUpdated(const QVector<ServoTelemetry>& servos)
{
    for (int i = 0; i < servos.size(); ++i) {
        int axis = (i == 0) ? static_cast<int>(LogicalAxis::J2) : static_cast<int>(LogicalAxis::R);
        if (axis >= 0 && axis < posLabels_.size()) {
            posLabels_[axis]->setText(QStringLiteral("%1°").arg(servos[i].angleDeg, 0, 'f', 1));
        }
        // FK 关节缓存：servos[0]=J2、servos[1]=R（angleDeg 已是逻辑角）
        if (i == 0) cachedJ2_ = servos[i].angleDeg;
        else if (i == 1) cachedR_ = servos[i].angleDeg;
    }
    RefreshCoordPanel();
    // 舵机离线/重连边沿提示：重连后扭矩归零、使能标志已复位，必须重新手动使能
    // （点动/Go 走 SET_ANGLE 与扭矩无关，但门禁要求人工确认，防止机械突然得电）
    bool allOnline = true;
    for (const auto& s : servos) if (!s.online) allOnline = false;
    if (!servoAllOnlinePrev_ && allOnline) {
        bool anyServoDisabled = !HardwareManager::instance().IsAxisEnabled(LogicalAxis::J2)
                             || !HardwareManager::instance().IsAxisEnabled(LogicalAxis::R);
        if (anyServoDisabled)
            SetHint(QStringLiteral("舵机已重连（扭矩已释放），请重新执行全局轴使能"), "#e0a520");
    } else if (servoAllOnlinePrev_ && !allOnline) {
        SetHint(QStringLiteral("舵机通信异常，自动重连中..."), "#e0a520");
    }
    servoAllOnlinePrev_ = allOnline;
    // 使能灯以 HardwareManager::IsAxisEnabled 为准（见 OnEnableStateChanged）
    OnEnableStateChanged();
}

void ManualControlPage::OnAxisAlarm(int axis, bool alarm)
{
    if (axis >= 0 && axis < alarmState_.size()) {
        alarmState_[axis] = alarm;
        RefreshStatusDot(axis);
    }
}

void ManualControlPage::OnLimitTriggered(int axis, bool positive, bool negative)
{
    if (axis >= 0 && axis < limitState_.size()) {
        limitState_[axis] = positive || negative;
        RefreshStatusDot(axis);
    }
}

void ManualControlPage::OnSoftLimit(int axis, bool positive)
{
    if (axis < 0 || axis >= softLimitDir_.size()) return;
    softLimitDir_[axis] = positive ? 1 : -1;
    runningState_[axis]  = false;
    RefreshStatusDot(axis);
    RefreshSoftLimitHint();
}

void ManualControlPage::RefreshStatusDot(int axis)
{
    if (axis < 0 || axis >= statusDots_.size()) return;
    QString color;
    QString tip;
    bool enabled = HardwareManager::instance().IsAxisEnabled(static_cast<LogicalAxis>(axis));
    if (alarmState_[axis]) {
        color = "#ff5f5f";
        QString detail = (axis < alarmDetail_.size()) ? alarmDetail_[axis] : QString();
        tip = detail.isEmpty() ? QStringLiteral("报警") : detail;
    }
    else if (limitState_[axis] || (axis < softLimitDir_.size() && softLimitDir_[axis] != 0))
                                  { color = "#ffb347"; tip = QStringLiteral("限位"); }
    else if (axis < homingAxes_.size() && homingAxes_[axis])
                                  { color = "#c08cff"; tip = QStringLiteral("回零中"); }
    else if (runningState_[axis]) { color = "#4fb3ff"; tip = QStringLiteral("运行中"); }
    else if (enabled)             { color = "#3bff7b"; tip = QStringLiteral("已使能"); }
    else                          { color = "#5a6a7a"; tip = QStringLiteral("未使能"); }
    statusDots_[axis]->setStyleSheet(QStringLiteral("background: %1; border-radius: 6px;").arg(color));
    statusDots_[axis]->setToolTip(tip);
}

void ManualControlPage::SetHint(const QString& text, const QString& color)
{
    if (!hintLabel_) return;
    if (color.isEmpty())
        hintLabel_->setStyleSheet("color: #8fd4ff; font-size: 13px; font-weight: 600; background: transparent; border: none; padding: 8px 4px;");
    else
        hintLabel_->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 600; background: transparent; border: none; padding: 8px 4px;").arg(color));
    hintLabel_->setText(text);
}

void ManualControlPage::RefreshSoftLimitHint()
{
    QStringList parts;
    for (int i = 0; i < softLimitDir_.size(); ++i) {
        if (softLimitDir_[i] == 0) continue;
        bool positive = softLimitDir_[i] > 0;
        double pos = positive
            ? HardwareManager::instance().GetLimitMax(static_cast<LogicalAxis>(i))
            : HardwareManager::instance().GetLimitMin(static_cast<LogicalAxis>(i));
        parts << QStringLiteral("轴%1 到达软限位（%2位置 %3）")
                    .arg(i + 1)
                    .arg(positive ? QStringLiteral("最大") : QStringLiteral("最小"))
                    .arg(pos);
    }
    if (parts.isEmpty())
        SetHint(QStringLiteral("提示：按住 +/- 按钮持续运动，松开停止"));
    else
        SetHint(parts.join(QStringLiteral(" / ")), QStringLiteral("#ffb347"));
}

void ManualControlPage::RefreshCoordPanel()
{
    if (coordLabels_.size() < 4) return;

    // 运动学参数仅在变化时重建一次（对齐 AutoRunPage，避免高频构造刷日志）
    double l1, l2, z0, h1, tcpX, tcpY, tcpZ;
    KinematicsHelper::ReadConfigParams(l1, l2, z0, h1, tcpX, tcpY, tcpZ);
    if (!coordKinLoaded_ || l1 != kinL1_ || l2 != kinL2_ || z0 != kinZ0_ || h1 != kinH1_
        || tcpX != kinTcpX_ || tcpY != kinTcpY_ || tcpZ != kinTcpZ_) {
        coordKin_ = KinematicsHelper::FromConfig();
        kinL1_ = l1; kinL2_ = l2; kinZ0_ = z0; kinH1_ = h1;
        kinTcpX_ = tcpX; kinTcpY_ = tcpY; kinTcpZ_ = tcpZ;
        coordKinLoaded_ = true;
    }

    Pose pose = coordKin_.Forward(Joints{cachedJ1_, cachedJ2_, cachedZ_, cachedR_});

    const struct { const char* name; double value; const char* unit; } items[4] = {
        { "X", pose.x, "" },
        { "Y", pose.y, "" },
        { "Z", pose.z, "" },
        { "R", pose.r, "\xC2\xB0" },
    };
    for (int i = 0; i < 4; ++i) {
        coordLabels_[i]->setText(
            QStringLiteral("<span style='color:#7c9fc0;'>%1: </span>"
                           "<span style='color:#7ed6ff;font-weight:700;font-size:15px;'>%2%3</span>")
                .arg(items[i].name).arg(items[i].value, 0, 'f', 2).arg(items[i].unit));
    }
}

void ManualControlPage::ResetAxisStates()
{
    // 急停后使能复位，但 UI 状态数组残留：RefreshStatusDot 优先级 alarm→限位→回零→运行→使能→未使能，
    // "限位"高于"未使能"，导致停在软限位边界的轴误显示橙色"限位"（曾复现轴1/3）。急停时全清再刷新。
    alarmState_.fill(false);
    limitState_.fill(false);
    enabledState_.fill(false);
    runningState_.fill(false);
    homeDoneState_.fill(false);
    homingAxes_.fill(false);
    softLimitDir_.fill(0);
    for (auto& d : alarmDetail_) d.clear();
    for (auto* b : goButtons_) if (b) b->setEnabled(true);
    for (int i = 0; i < statusDots_.size(); ++i) RefreshStatusDot(i);
    RefreshSoftLimitHint();
}
