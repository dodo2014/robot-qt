#include "ManualControlPage.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QDebug>

ManualControlPage::ManualControlPage(QWidget* parent)
    : QWidget(parent)
{
    SetupUI();
}

void ManualControlPage::SetupUI()
{
    setStyleSheet("background: #262c34;");

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(14, 14, 14, 14);
    mainLayout->setSpacing(12);

    // ==== Top bar ====
    auto* teachTop = new QWidget();
    teachTop->setStyleSheet("background: #1b222b; padding: 10px 16px; border-radius: 12px;");
    // 【新增这行】：禁止顶栏在垂直方向上被拉伸
    teachTop->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* topLayout = new QHBoxLayout(teachTop);
    topLayout->setContentsMargins(16, 10, 16, 10);
    topLayout->setSpacing(16);

    auto* enableBtn = new QPushButton(QStringLiteral("全局轴使能"));
    enableBtn->setStyleSheet("QPushButton { background: #1f9d4a; border: none; border-radius: 10px; padding: 10px 18px; font-weight: 600; font-size: 15px; color: white; } QPushButton:hover { background: #28b85a; }");
    enableBtn->setCursor(Qt::PointingHandCursor);
    connect(enableBtn, &QPushButton::clicked, this, &ManualControlPage::OnGlobalEnable);

    auto* disableBtn = new QPushButton(QStringLiteral("全局断使能"));
    disableBtn->setStyleSheet("QPushButton { background: #b13a3a; border: none; border-radius: 10px; padding: 10px 18px; font-weight: 600; font-size: 15px; color: white; } QPushButton:hover { background: #d14444; }");
    disableBtn->setCursor(Qt::PointingHandCursor);
    connect(disableBtn, &QPushButton::clicked, this, &ManualControlPage::OnGlobalDisable);

    topLayout->addWidget(enableBtn);
    topLayout->addWidget(disableBtn);
    topLayout->addStretch();

    // Coord panel in top bar
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

    topLayout->addWidget(coordPanel);

    mainLayout->addWidget(teachTop);

    // ==== Axis table ====
    // Use QGridLayout to simulate the table
    struct AxisInfo {
        QString label;
        QString typeIcon;
        QString typeStyle;
        double speed;
        double accel;
        double decel;
        QString pos;
        double target;
        bool isGripper;
        bool isExtrude;
    };

    QVector<AxisInfo> axes = {
        { QStringLiteral("轴1(J1)"), QStringLiteral("\xE2\x9A\x99"), R"(color: #6a9fc0; font-size: 18px;)", 100, 50, 50, QStringLiteral("12.5\xC2\xB0"), 30.0, false, false },
        { QStringLiteral("轴2(J2)"), QStringLiteral("\xF0\x9F\x94\xA7"), R"(color: #c0a06a; font-size: 18px;)", 80, 0, 0, QStringLiteral("-8.2\xC2\xB0"), 0.0, false, false },
        { QStringLiteral("轴3(Z)"),  QStringLiteral("\xE2\x9A\x99"), R"(color: #6a9fc0; font-size: 18px;)", 120, 60, 60, QStringLiteral("32.8 mm"), 25.0, false, false },
        { QStringLiteral("轴4(R)"),  QStringLiteral("\xF0\x9F\x94\xA7"), R"(color: #c0a06a; font-size: 18px;)", 90, 0, 0, QStringLiteral("15.0\xC2\xB0"), 10.0, false, false },
        { QStringLiteral("轴5(夹爪)"), QStringLiteral("\xE2\x9A\x99"), R"(color: #6a9fc0; font-size: 18px;)", 50, 30, 30, QStringLiteral("0.0 mm"), 5.0, true, false },
        { QStringLiteral("轴6(挤出)"), QStringLiteral("\xE2\x9A\x99"), R"(color: #6a9fc0; font-size: 18px;)", 60, 40, 40, QStringLiteral("0.0 mm"), 3.5, false, true },
    };


    // Header labels
    QStringList headers = {
        QStringLiteral("轴"), QStringLiteral("类型"), QStringLiteral("速度"),
        QStringLiteral("加速度"), QStringLiteral("减速度"), QStringLiteral("点动"),
        QStringLiteral("当前位置"), QStringLiteral("点动"), QStringLiteral("目标位置"),
        QStringLiteral("移动"), QStringLiteral("停止")
    };

    auto* tableWidget = new QWidget();
    tableWidget->setStyleSheet("background: transparent;");
    tableWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* tableLayout = new QVBoxLayout(tableWidget);
    tableLayout->setContentsMargins(0, 0, 0, 0);
    tableLayout->setSpacing(6);

    // 【核心修复1】：把固定宽度变成了“拉伸比例因子” (数值代表相对比例)
    QVector<int> colStretches = { 85, 40, 80, 80, 80, 95, 90, 95, 85, 95, 60 };

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
        speedSpin->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px; border-radius: 6px; font-size: 13px;");
        speedSpin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        rowLayout->addWidget(speedSpin, colStretches[2]);

        // Col 3/4: Accel/Decel
        for (int colIdx = 3; colIdx <= 4; ++colIdx)
        {
            double val = (colIdx == 3) ? axis.accel : axis.decel;
            if (val > 0) {
                auto* spin = new QDoubleSpinBox();
                spin->setRange(0, 99999.99);
                spin->setDecimals(1);
                spin->setValue(val);
                spin->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px; border-radius: 6px; font-size: 13px;");
                spin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
                rowLayout->addWidget(spin, colStretches[colIdx]);
            }
            else {
                auto* dash = new QLabel(QStringLiteral("—"));
                dash->setStyleSheet("color: #556677; background: transparent; border: none; padding: 0 4px;");
                dash->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                rowLayout->addWidget(dash, colStretches[colIdx]);
            }
        }

        // Col 5: Jog -
        auto* jogMinus = new QPushButton(axis.isExtrude ? QStringLiteral("\xE2\x88\x92 回抽") : (axis.isGripper ? QStringLiteral("\xE2\x88\x92 松开") : QStringLiteral("\xE2\x88\x92")));
        QString jogStyle = axis.isExtrude ? "#7f5f3f" : (axis.isGripper ? "#3f6f8f" : "#2f3845");
        QString jogHover = axis.isExtrude ? "#9f7f4f" : (axis.isGripper ? "#4a7f9f" : "#4a5a6e");
        jogMinus->setStyleSheet(QStringLiteral("QPushButton { background: %1; border: none; border-radius: 6px; color: white; padding: 6px 10px; font-weight: 700; font-size: 14px; } QPushButton:hover { background: %2; }").arg(jogStyle, jogHover));
        jogMinus->setCursor(Qt::PointingHandCursor);
        jogMinus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        int axisIdx = i;
        connect(jogMinus, &QPushButton::pressed, this, [this, axisIdx]() { OnJogMinus(axisIdx); });
        rowLayout->addWidget(jogMinus, colStretches[5]);

        // Col 6: Current position
        auto* posLabel = new QLabel(axis.pos);
        posLabel->setStyleSheet("background: #0d141c; padding: 4px 6px; border-radius: 6px; text-align: center; font-weight: 700; color: #cde2ff; font-size: 14px;");
        posLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        rowLayout->addWidget(posLabel, colStretches[6]);

        // Col 7: Jog +
        auto* jogPlus = new QPushButton(axis.isExtrude ? QStringLiteral("+ 挤出") : (axis.isGripper ? QStringLiteral("+ 夹紧") : QStringLiteral("+")));
        jogPlus->setStyleSheet(QStringLiteral("QPushButton { background: %1; border: none; border-radius: 6px; color: white; padding: 6px 10px; font-weight: 700; font-size: 14px; } QPushButton:hover { background: %2; }").arg(jogStyle, jogHover));
        jogPlus->setCursor(Qt::PointingHandCursor);
        jogPlus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        connect(jogPlus, &QPushButton::pressed, this, [this, axisIdx]() { OnJogPlus(axisIdx); });
        rowLayout->addWidget(jogPlus, colStretches[7]);

        // Col 8: Target position
        auto* targetSpin = new QDoubleSpinBox();
        targetSpin->setRange(-99999.99, 99999.99);
        targetSpin->setDecimals(1);
        targetSpin->setValue(axis.target);
        targetSpin->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px; border-radius: 6px; font-size: 13px;");
        targetSpin->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        rowLayout->addWidget(targetSpin, colStretches[8]);

        // Col 9: Go button
        auto* goBtn = new QPushButton(axis.isExtrude ? QStringLiteral("Go定量") : QStringLiteral("Go"));
        QString goColor = axis.isExtrude ? "#b57f2f" : "#2f6f8f";
        QString goHover = axis.isExtrude ? "#cc9a3a" : "#3a84a8";
        goBtn->setStyleSheet(QStringLiteral("QPushButton { background: %1; border: none; border-radius: 6px; color: white; padding: 6px 10px; font-weight: 600; font-size: 13px; } QPushButton:hover { background: %2; }").arg(goColor, goHover));
        goBtn->setCursor(Qt::PointingHandCursor);
        goBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        connect(goBtn, &QPushButton::clicked, this, [this, axisIdx]() { OnGoClicked(axisIdx); });
        rowLayout->addWidget(goBtn, colStretches[9]);

        // Col 10: Stop button
        auto* stopBtn = new QPushButton(QStringLiteral("停止"));
        stopBtn->setStyleSheet("QPushButton { background: #8f3f3f; border: none; border-radius: 6px; color: white; padding: 6px 10px; font-weight: 600; font-size: 13px; } QPushButton:hover { background: #aa4f4f; }");
        stopBtn->setCursor(Qt::PointingHandCursor);
        stopBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        connect(stopBtn, &QPushButton::clicked, this, [this, axisIdx]() { OnStopAxis(axisIdx); });
        rowLayout->addWidget(stopBtn, colStretches[10]);

        // 注意：这里也删除了 rowLayout->addStretch();
        tableLayout->addWidget(rowWidget);
    }

    mainLayout->addWidget(tableWidget);

    // ==== Bottom hint ====
    auto* hintLabel = new QLabel(QStringLiteral("提示：按住 +/- 按钮持续运动，松开停止"));
    hintLabel->setStyleSheet("color: #7c8a9e; font-size: 13px; background: transparent; border: none; padding: 8px 4px;");
    mainLayout->addWidget(hintLabel);

    // 【新增这行】：在 SetupUI() 的最后一行加入弹簧！
    mainLayout->addStretch();
}

void ManualControlPage::OnGlobalEnable()
{
    qDebug() << "按钮被点击: 全局轴使能";
}

void ManualControlPage::OnGlobalDisable()
{
    qDebug() << "按钮被点击: 全局断使能";
}

void ManualControlPage::OnJogMinus(int axis)
{
    qDebug() << "按钮被点击: JOG - 轴" << axis;
}

void ManualControlPage::OnJogPlus(int axis)
{
    qDebug() << "按钮被点击: JOG + 轴" << axis;
}

void ManualControlPage::OnGoClicked(int axis)
{
    qDebug() << "按钮被点击: Go 轴" << axis;
}

void ManualControlPage::OnStopAxis(int axis)
{
    qDebug() << "按钮被点击: 停止 轴" << axis;
}
