#include "ManualControlPage.h"

#include "HAL/HardwareManager.h"
#include "HAL/AxisMap.h"

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

    const int count = static_cast<int>(LogicalAxis::Count);
    alarmState_.fill(false, count);
    limitState_.fill(false, count);
    enabledState_.fill(false, count);
    runningState_.fill(false, count);
    homeDoneState_.fill(false, count);

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
        HardwareManager::instance().EmergencyStop();
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

    struct Coord { QString name; double value; QString unit; };
    QVector<Coord> coords = {
        { QStringLiteral("X"), 145.23, QString() },
        { QStringLiteral("Y"), 87.46,  QString() },
        { QStringLiteral("Z"), 32.81,  QString() },
        { QStringLiteral("R"), 12.50,  QStringLiteral("\xC2\xB0") },
    };

    for (const auto& c : coords)
    {
        auto* label = new QLabel(
            QStringLiteral("<span style='color:#7c9fc0;'>%1: </span>"
                           "<span style='color:#7ed6ff;font-weight:700;font-size:15px;'>%2%3</span>")
                .arg(c.name).arg(c.value, 0, 'f', 2).arg(c.unit));
        label->setStyleSheet("font-size: 15px; color: #b7d6ff; background: transparent; border: none;");
        coordLayout->addWidget(label);
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
    QVector<int> colStretches = { 85, 28, 80, 75, 90, 75, 85, 75, 60, 55, 34 };

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

        // Col 2: Speed
        auto* speedSpin = new QDoubleSpinBox();
        speedSpin->setRange(0, 99999.99);
        speedSpin->setDecimals(1);
        speedSpin->setValue(axis.speed);
        // 注意：不要在 QDoubleSpinBox 的样式表里写 font-size —— 会触发 Qt 样式引擎
        // 在 reparent/polish 时崩溃(Qt6 QSS 已知问题)。用 setFont 实现同等视觉效果。
        speedSpin->setFont([&](){ QFont f = speedSpin->font(); f.setPointSizeF(9.75); return f; }());
        speedSpin->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px; border-radius: 6px;");
        speedSpin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
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
        rowLayout->addWidget(speedSpin, colStretches[2]);

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

        // Col 6: Target position
        auto* targetSpin = new QDoubleSpinBox();
        targetSpin->setRange(-99999.99, 99999.99);
        targetSpin->setDecimals(1);
        targetSpin->setValue(axis.target);
        targetSpin->setFont([&](){ QFont f = targetSpin->font(); f.setPointSizeF(9.75); return f; }());
        targetSpin->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px; border-radius: 6px;");
        targetSpin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        targetSpins_.append(targetSpin);
        rowLayout->addWidget(targetSpin, colStretches[6]);

        // Col 7: Go button
        auto* goBtn = new QPushButton(QStringLiteral("Go"));
        QString goColor = "#2f6f8f";
        QString goHover = "#3a84a8";
        goBtn->setStyleSheet(QStringLiteral("QPushButton { background: %1; border: none; border-radius: 6px; color: white; padding: 6px 10px; font-weight: 600; font-size: 13px; } QPushButton:hover { background: %2; }").arg(goColor, goHover));
        goBtn->setCursor(Qt::PointingHandCursor);
        goBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        connect(goBtn, &QPushButton::clicked, this, [this, axisIdx]() { OnGoClicked(axisIdx); });
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
    hintLabel_->setStyleSheet("color: #7c8a9e; font-size: 13px; background: transparent; border: none; padding: 8px 4px;");
    layout->addWidget(hintLabel_);

    layout->addStretch();

    scrollArea->setWidget(content);
    mainLayout->addWidget(scrollArea);
}

void ManualControlPage::OnGlobalEnable()
{
    bool connected = HardwareManager::instance().IsMotionCardConnected()
                     || HardwareManager::instance().IsServoConnected();
    bool ok = HardwareManager::instance().EnableAll();
    if (!connected) SetHint(QStringLiteral("未连接硬件，命令可能未生效"));
    else if (!ok)   SetHint(QStringLiteral("部分轴使能失败"));
    else            SetHint(QStringLiteral("全局轴使能完成"));
    qDebug() << "全局轴使能";
}

void ManualControlPage::OnGlobalDisable()
{
    bool connected = HardwareManager::instance().IsMotionCardConnected()
                     || HardwareManager::instance().IsServoConnected();
    bool ok = HardwareManager::instance().DisableAll();
    if (!connected) SetHint(QStringLiteral("未连接硬件，命令可能未生效"));
    else if (!ok)   SetHint(QStringLiteral("部分轴断使能失败"));
    else            SetHint(QStringLiteral("全局断使能完成"));
    qDebug() << "全局断使能";
}

void ManualControlPage::OnGlobalHome()
{
    bool connected = HardwareManager::instance().IsMotionCardConnected()
                     || HardwareManager::instance().IsServoConnected();
    if (!connected) { SetHint(QStringLiteral("未连接硬件，无法回零")); return; }
    homingPending_ = true;
    HardwareManager::instance().HomeAll();
    SetHint(QStringLiteral("回零中..."));
    qDebug() << "一键回零";
}

void ManualControlPage::OnJogMinus(int axis)
{
    if (axis < 0 || axis >= static_cast<int>(LogicalAxis::Count)) return;
    double speed = (axis < speedSpins_.size()) ? speedSpins_[axis]->value() : 20.0;
    double maxSpeed = HardwareManager::instance().GetMaxSpeed(static_cast<LogicalAxis>(axis));
    if (maxSpeed > 0.0 && speed > maxSpeed) speed = maxSpeed;
    HardwareManager::instance().MoveJog(static_cast<LogicalAxis>(axis), speed, -1);
    runningState_[axis] = true;
    RefreshStatusDot(axis);
    qDebug() << "JOG - 轴" << axis;
    qDebug() << "速度" << speed;
}

void ManualControlPage::OnJogPlus(int axis)
{
    if (axis < 0 || axis >= static_cast<int>(LogicalAxis::Count)) return;
    double speed = (axis < speedSpins_.size()) ? speedSpins_[axis]->value() : 20.0;
    double maxSpeed = HardwareManager::instance().GetMaxSpeed(static_cast<LogicalAxis>(axis));
    if (maxSpeed > 0.0 && speed > maxSpeed) speed = maxSpeed;
    HardwareManager::instance().MoveJog(static_cast<LogicalAxis>(axis), speed, 1);
    runningState_[axis] = true;
    RefreshStatusDot(axis);
    qDebug() << "JOG + 轴" << axis;
}

void ManualControlPage::OnJogStop(int axis)
{
    if (axis < 0 || axis >= static_cast<int>(LogicalAxis::Count)) return;
    HardwareManager::instance().StopJog(static_cast<LogicalAxis>(axis));
    runningState_[axis] = false;
    RefreshStatusDot(axis);
    qDebug() << "JOG 停止 轴" << axis;
}

void ManualControlPage::OnGoClicked(int axis)
{
    if (axis < 0 || axis >= static_cast<int>(LogicalAxis::Count)) return;
    if (axis < targetSpins_.size()) {
        HardwareManager::instance().MoveAbs(static_cast<LogicalAxis>(axis), targetSpins_[axis]->value());
    }
    qDebug() << "Go 轴" << axis;
}

void ManualControlPage::OnStopAxis(int axis)
{
    if (axis < 0 || axis >= static_cast<int>(LogicalAxis::Count)) return;
    HardwareManager::instance().StopAxis(static_cast<LogicalAxis>(axis));
    runningState_[axis] = false;
    RefreshStatusDot(axis);
    qDebug() << "停止 轴" << axis;
}

void ManualControlPage::OnHomeAxis(int axis)
{
    if (axis < 0 || axis >= static_cast<int>(LogicalAxis::Count)) return;
    runningState_[axis] = false;
    homeDoneState_[axis] = false;
    RefreshStatusDot(axis);
    HardwareManager::instance().HomeAxis(static_cast<LogicalAxis>(axis));
    SetHint(QStringLiteral("轴%1 回零中...").arg(axis + 1));
    qDebug() << "回零 轴" << axis;
}

void ManualControlPage::OnStateUpdated(const QVector<MotorStatus>& axes)
{
    for (const auto& st : axes) {
        if (st.axisId < 0 || st.axisId >= posLabels_.size()) continue;
        posLabels_[st.axisId]->setText(QStringLiteral("%1").arg(st.position, 0, 'f', 1));
        if (st.axisId < enabledState_.size()) {
            enabledState_[st.axisId]  = st.enabled;
            runningState_[st.axisId]  = st.running;
            homeDoneState_[st.axisId] = st.homeDone;
            alarmState_[st.axisId]    = st.alarm;
            limitState_[st.axisId]    = st.limitPositive || st.limitNegative;
            RefreshStatusDot(st.axisId);
        }
    }

    if (homingPending_) {
        bool allHome = true;
        for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i) {
            auto binding = AxisMap::Get(static_cast<LogicalAxis>(i));
            if (binding.type == AxisBinding::Type::Card && i < homeDoneState_.size()) {
                if (!homeDoneState_[i]) { allHome = false; break; }
            }
        }
        if (allHome) {
            homingPending_ = false;
            SetHint(QStringLiteral("回零完成"));
        }
    }
}

void ManualControlPage::OnServoStateUpdated(const QVector<ServoTelemetry>& servos)
{
    for (int i = 0; i < servos.size(); ++i) {
        int axis = (i == 0) ? static_cast<int>(LogicalAxis::J2) : static_cast<int>(LogicalAxis::R);
        if (axis >= 0 && axis < posLabels_.size()) {
            posLabels_[axis]->setText(QStringLiteral("%1°").arg(servos[i].angleDeg, 0, 'f', 1));
        }
        if (axis >= 0 && axis < enabledState_.size()) {
            enabledState_[axis] = servos[i].online;
            RefreshStatusDot(axis);
        }
    }
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

void ManualControlPage::RefreshStatusDot(int axis)
{
    if (axis < 0 || axis >= statusDots_.size()) return;
    QString color;
    QString tip;
    if (alarmState_[axis])        { color = "#ff5f5f"; tip = QStringLiteral("报警"); }
    else if (limitState_[axis])   { color = "#ffb347"; tip = QStringLiteral("限位"); }
    else if (runningState_[axis]) { color = "#4fb3ff"; tip = QStringLiteral("运行中"); }
    else if (enabledState_[axis]) { color = "#3bff7b"; tip = QStringLiteral("已使能"); }
    else                          { color = "#5a6a7a"; tip = QStringLiteral("未使能"); }
    statusDots_[axis]->setStyleSheet(QStringLiteral("background: %1; border-radius: 6px;").arg(color));
    statusDots_[axis]->setToolTip(tip);
}

void ManualControlPage::SetHint(const QString& text, const QString& color)
{
    if (!hintLabel_) return;
    if (color.isEmpty())
        hintLabel_->setStyleSheet("color: #7c8a9e; font-size: 13px; background: transparent; border: none; padding: 8px 4px;");
    else
        hintLabel_->setStyleSheet(QStringLiteral("color: %1; font-size: 13px; font-weight: 600; background: transparent; border: none; padding: 8px 4px;").arg(color));
    hintLabel_->setText(text);
}
