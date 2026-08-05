#include "ConfigPage.h"
#include "Config/ConfigManager.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QFormLayout>
#include <QScrollArea>
#include <QStackedWidget>
#include <QListWidget>
#include <QDebug>
#include <QMessageBox>
#include <QCoreApplication>
#include <QFileInfo>

#include <vector>
#include <algorithm>

#include <spdlog/spdlog.h>

#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

// ---------------------------------------------------------------------------
// Helper lambdas for config access (must be called after ConfigManager loaded)
// ---------------------------------------------------------------------------
static QString sVal(const char* path, const QString& fallback = {})
{
    auto& cfg = ConfigManager::instance();
    try { return QString::fromStdString(cfg.getValue<std::string>(path, fallback.toStdString())); }
    catch (...) { return fallback; }
}

static double dVal(const char* path, double fallback = 0.0)
{
    auto& cfg = ConfigManager::instance();
    try { return cfg.getValue<double>(path, fallback); }
    catch (...) { return fallback; }
}

static int iVal(const char* path, int fallback = 0)
{
    auto& cfg = ConfigManager::instance();
    try { return cfg.getValue<int>(path, fallback); }
    catch (...) { return fallback; }
}

// ---------------------------------------------------------------------------
ConfigPage::ConfigPage(QWidget* parent)
    : QWidget(parent)
{
    SetupUI();
}

// ---------------------------------------------------------------------------
//  Try to find & load config.json
// ---------------------------------------------------------------------------
static void ensureConfigLoaded()
{
    auto& cfg = ConfigManager::instance();
    if (!cfg.filePath().isEmpty() && QFileInfo::exists(cfg.filePath()))
        return; // already loaded

    QStringList candidates = {
#ifdef PROJECT_SOURCE_DIR
        QString::fromUtf8(PROJECT_SOURCE_DIR) + QStringLiteral("/config/config.json"),
#endif
        QCoreApplication::applicationDirPath() + QStringLiteral("/config.json"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../config/config.json"),
    };

    for (const auto& path : candidates) {
        if (QFileInfo::exists(path)) {
            cfg.load(path);
            return;
        }
    }
    qWarning() << "[ConfigPage] No config.json found in any candidate path";
    SPDLOG_INFO("[ConfigPage] No config.json found in any candidate path");
}

// ---------------------------------------------------------------------------
//  Tab utility: auto-save a QLineEdit -> config path (on editingFinished)
// ---------------------------------------------------------------------------
static void bindLineEdit(QLineEdit* edit, const char* path)
{
    ensureConfigLoaded();
    edit->setText(sVal(path));
    QObject::connect(edit, &QLineEdit::editingFinished, [edit, p = std::string(path)]() {
        ConfigManager::instance().set(p, edit->text().toStdString());
    });
}

static void bindDoubleSpin(QDoubleSpinBox* spin, const char* path, double min, double max, double def)
{
    ensureConfigLoaded();
    spin->setRange(min, max);
    spin->setValue(dVal(path, def));
    QObject::connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [p = std::string(path)](double v) {
        ConfigManager::instance().set(p, v);
    });
}

static void bindCombo(QComboBox* combo, const char* path, int def = 0)
{
    ensureConfigLoaded();
    combo->setCurrentIndex(iVal(path, def));
    QObject::connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), [p = std::string(path)](int idx) {
        ConfigManager::instance().set(p, idx);
    });
}

// ===========================================================================
//  SetupUI
// ===========================================================================
void ConfigPage::SetupUI()
{
    ensureConfigLoaded();

    setStyleSheet("background: #262c34;");

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(14, 14, 14, 14);

    auto* container = new QWidget();
    container->setStyleSheet("background: #1d252f; border-radius: 14px; padding: 12px;");
    auto* containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(12, 12, 12, 12);
    containerLayout->setSpacing(0);

    // Tab headers
    tabGroup_ = new QButtonGroup(this);
    tabGroup_->setExclusive(true);

    auto* tabHeaderWidget = new QWidget();
    tabHeaderWidget->setStyleSheet("border-bottom: 1px solid #384652; padding-bottom: 4px;");
    auto* tabHeaderLayout = new QHBoxLayout(tabHeaderWidget);
    tabHeaderLayout->setContentsMargins(0, 0, 0, 4);
    tabHeaderLayout->setSpacing(4);

    QStringList tabNames = {
        QStringLiteral("通信与连接"),
        QStringLiteral("运动学参数"),
        QStringLiteral("视觉与工艺参数"),
        QStringLiteral("TCP与标定"),
        QStringLiteral("电控与映射"),
    };

    for (int i = 0; i < tabNames.size(); ++i)
    {
        auto* tabBtn = new QPushButton(tabNames[i]);
        tabBtn->setCheckable(true);
        tabBtn->setObjectName(QStringLiteral("tabBtn%1").arg(i));
        tabBtn->setCursor(Qt::PointingHandCursor);
        tabBtn->setStyleSheet(R"(
            QPushButton {
                background: transparent; color: #8da3bb;
                padding: 6px 16px; font-weight: 500;
                border-radius: 6px 6px 0 0; border: none;
            }
            QPushButton:hover { background: #2c3a48; }
            QPushButton:checked { background: #2c3a48; color: white; }
        )");
        tabGroup_->addButton(tabBtn, i);
        tabHeaderLayout->addWidget(tabBtn);
    }
    tabHeaderLayout->addStretch();
    containerLayout->addWidget(tabHeaderWidget);

    // Tab panes
    tabStack_ = new QStackedWidget();
    tabStack_->setStyleSheet("background: transparent;");

    tabStack_->addWidget(CreateTab1Comm());
    tabStack_->addWidget(CreateTab2Kinematics());
    tabStack_->addWidget(CreateTab3Vision());
    tabStack_->addWidget(CreateTab4TCP());
    tabStack_->addWidget(CreateElecMapTab());
    tabStack_->setCurrentIndex(0);

    connect(tabGroup_, &QButtonGroup::idClicked, this, &ConfigPage::OnTabClicked);

    containerLayout->addWidget(tabStack_, 1);
    container->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    mainLayout->addWidget(container, 1);

    auto* firstTab = qobject_cast<QPushButton*>(tabGroup_->button(0));
    if (firstTab) firstTab->setChecked(true);
}

void ConfigPage::OnTabClicked(int index)
{
    if (index >= 0 && index < tabStack_->count())
    {
        tabStack_->setCurrentIndex(index);
        SPDLOG_INFO("[Config] Tab switched to {}", index);
    }
}

// ===========================================================================
//  Tab 1 — 通信与连接
// ===========================================================================
QWidget* ConfigPage::CreateTab1Comm()
{
    auto* tab = new QWidget();
    tab->setStyleSheet("background: transparent;");
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(12);

    auto makeLabel = [](const QString& text) {
        auto* l = new QLabel(text);
        l->setStyleSheet("color: #b8cce3; min-width: 90px; font-size: 13px; background: transparent; border: none;");
        return l;
    };
    auto makeEdit = [](int width = 130) {
        auto* e = new QLineEdit();
        e->setFixedWidth(width);
        e->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px 8px; border-radius: 6px; font-size: 13px;");
        return e;
    };

    // Row 1: 网口 IP + 端口
    {
        auto* row = new QWidget();
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(18);

        rl->addWidget(makeLabel(QStringLiteral("网口 IP")));
        auto* ipEdit = makeEdit(130);
        bindLineEdit(ipEdit, "communication.motionCard.ip");
        rl->addWidget(ipEdit);

        rl->addWidget(makeLabel(QStringLiteral("端口")));
        auto* portEdit = makeEdit(60);
        bindLineEdit(portEdit, "communication.motionCard.port");
        rl->addWidget(portEdit);

        rl->addStretch();
        layout->addWidget(row);
    }

    // Row 2: 舵机 COM + 波特率
    {
        auto* row = new QWidget();
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(18);

        rl->addWidget(makeLabel(QStringLiteral("舵机 COM")));
        auto* comCombo = new QComboBox();
        comCombo->addItems({ QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"), QStringLiteral("COM6") });
        comCombo->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px 8px; border-radius: 6px;");
        {
            QString current = sVal("communication.servo.port", "COM3");
            comCombo->blockSignals(true);
            for (int i = 0; i < comCombo->count(); ++i)
                if (comCombo->itemText(i) == current) { comCombo->setCurrentIndex(i); break; }
            comCombo->blockSignals(false);
        }
        QObject::connect(comCombo, &QComboBox::currentTextChanged, this, [](const QString& text) {
            ConfigManager::instance().set("communication.servo.port", text.toStdString());
        });
        rl->addWidget(comCombo);

        rl->addWidget(makeLabel(QStringLiteral("波特率")));
        auto* baudCombo = new QComboBox();
        baudCombo->addItems({ QStringLiteral("115200"), QStringLiteral("9600"), QStringLiteral("57600"), QStringLiteral("38400") });
        {
            QString current = sVal("communication.servo.baudRate");
            baudCombo->blockSignals(true);
            for (int i = 0; i < baudCombo->count(); ++i)
                if (baudCombo->itemText(i) == current) { baudCombo->setCurrentIndex(i); break; }
            baudCombo->blockSignals(false);
        }
        QObject::connect(baudCombo, &QComboBox::currentTextChanged, this, [](const QString& text) {
            ConfigManager::instance().set("communication.servo.baudRate", text.toStdString());
        });
        rl->addWidget(baudCombo);

        rl->addStretch();
        layout->addWidget(row);
    }

    // Row 3: 奥比中光相机 (no 初始化 button)
    {
        auto* row = new QWidget();
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(18);

        rl->addWidget(makeLabel(QStringLiteral("奥比中光相机")));
        auto* snEdit = makeEdit(140);
        bindLineEdit(snEdit, "communication.camera.sn");
        rl->addWidget(snEdit);

        rl->addStretch();
        layout->addWidget(row);
    }

    layout->addStretch();
    return tab;
}

// ===========================================================================
//  Tab 2 — 运动学参数
// ===========================================================================
QWidget* ConfigPage::CreateTab2Kinematics()
{
    auto* tab = new QWidget();
    tab->setStyleSheet("background: transparent;");
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(12);

    // Row: L1, L2, Z0
    {
        auto* row = new QWidget();
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(18);

        struct KinParam { QString label; const char* path; double def; };
        QVector<KinParam> params = {
            { QStringLiteral("L1 (大臂)"), "kinematics.links.l1", 285.0 },
            { QStringLiteral("L2 (小臂)"), "kinematics.links.l2", 215.0 },
            { QStringLiteral("Z0 基准"),   "kinematics.links.z0",  45.0 },
        };

        for (const auto& p : params)
        {
            auto* label = new QLabel(p.label);
            label->setStyleSheet("color: #b8cce3; min-width: 90px; font-size: 13px; background: transparent; border: none;");

            auto* input = new QLineEdit(QString::number(dVal(p.path, p.def)));
            input->setFixedWidth(70);
            input->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px 8px; border-radius: 6px;");
            QObject::connect(input, &QLineEdit::editingFinished, [input, p = std::string(p.path)]() {
                bool ok = false;
                double v = input->text().toDouble(&ok);
                if (ok) ConfigManager::instance().set(p, v);
            });

            rl->addWidget(label);
            rl->addWidget(input);
        }
        rl->addStretch();
        layout->addWidget(row);
    }

    // Axis parameter grid
    {
        auto* grid = new QWidget();
        grid->setStyleSheet("background: #141c26; padding: 8px; border-radius: 8px;");
        auto* gridLayout = new QHBoxLayout(grid);
        gridLayout->setSpacing(6);

        auto* colAxis = new QVBoxLayout();
        colAxis->setSpacing(6);
        auto* colPulse = new QVBoxLayout();
        colPulse->setSpacing(6);
        auto* colMin = new QVBoxLayout();
        colMin->setSpacing(6);
        auto* colMax = new QVBoxLayout();
        colMax->setSpacing(6);

        auto makeHdr = [](const QString& text) {
            auto* h = new QLabel(text);
            h->setStyleSheet("color: #9bb3cf; font-weight: 500; background: transparent; border: none;");
            return h;
        };
        colAxis->addWidget(makeHdr(QStringLiteral("轴")));
        colPulse->addWidget(makeHdr(QStringLiteral("脉冲当量")));
        colMin->addWidget(makeHdr(QStringLiteral("软限位Min")));
        colMax->addWidget(makeHdr(QStringLiteral("软限位Max")));

        const char* names[] = { "J1", "J2", "Z" };
        for (int i = 0; i < 3; ++i)
        {
            std::string prefix = std::string("kinematics.axisParams[") + std::to_string(i) + "]";

            auto* nameLbl = new QLabel(QString::fromStdString(names[i]));
            nameLbl->setStyleSheet("color: white; font-weight: 600; background: transparent; border: none;");
            colAxis->addWidget(nameLbl);

            auto* pulseInput = new QLineEdit(QString::number(dVal((prefix + ".pulsesPerUnit").c_str(), 0.0)));
            pulseInput->setFixedWidth(70);
            pulseInput->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px 8px; border-radius: 6px;");
            QObject::connect(pulseInput, &QLineEdit::editingFinished, [pulseInput, p = prefix + ".pulsesPerUnit"]() {
                bool ok = false; double v = pulseInput->text().toDouble(&ok);
                if (ok) ConfigManager::instance().set(p, v);
            });
            colPulse->addWidget(pulseInput);

            auto* minInput = new QLineEdit(QString::number(dVal((prefix + ".limitMin").c_str(), 0.0)));
            minInput->setFixedWidth(70);
            minInput->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px 8px; border-radius: 6px;");
            QObject::connect(minInput, &QLineEdit::editingFinished, [minInput, p = prefix + ".limitMin"]() {
                bool ok = false; double v = minInput->text().toDouble(&ok);
                if (ok) ConfigManager::instance().set(p, v);
            });
            colMin->addWidget(minInput);

            auto* maxInput = new QLineEdit(QString::number(dVal((prefix + ".limitMax").c_str(), 0.0)));
            maxInput->setFixedWidth(70);
            maxInput->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px 8px; border-radius: 6px;");
            QObject::connect(maxInput, &QLineEdit::editingFinished, [maxInput, p = prefix + ".limitMax"]() {
                bool ok = false; double v = maxInput->text().toDouble(&ok);
                if (ok) ConfigManager::instance().set(p, v);
            });
            colMax->addWidget(maxInput);
        }

        auto* c1 = new QWidget(); c1->setLayout(colAxis);
        auto* c2 = new QWidget(); c2->setLayout(colPulse);
        auto* c3 = new QWidget(); c3->setLayout(colMin);
        auto* c4 = new QWidget(); c4->setLayout(colMax);

        gridLayout->addWidget(c1);
        gridLayout->addWidget(c2);
        gridLayout->addWidget(c3);
        gridLayout->addWidget(c4);
        gridLayout->addStretch();

        layout->addWidget(grid);
    }

    layout->addStretch();
    return tab;
}

// ===========================================================================
//  Tab 3 — 视觉与工艺参数
// ===========================================================================
QWidget* ConfigPage::CreateTab3Vision()
{
    auto* tab = new QWidget();
    tab->setStyleSheet("background: transparent;");
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(12);

    auto makeLabel = [](const QString& text) {
        auto* l = new QLabel(text);
        l->setStyleSheet("color: #b8cce3; min-width: 90px; font-size: 13px; background: transparent; border: none;");
        return l;
    };
    auto makeEdit = [](int width = 70) {
        auto* e = new QLineEdit();
        e->setFixedWidth(width);
        e->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px 8px; border-radius: 6px;");
        return e;
    };

    // Row 1: 识别置信度阈值
    {
        auto* row = new QWidget();
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(18);

        rl->addWidget(makeLabel(QStringLiteral("识别置信度阈值")));
        auto* input = makeEdit(70);
        input->setText(QString::number(dVal("vision.confidenceThreshold", 0.85)));
        QObject::connect(input, &QLineEdit::editingFinished, [input]() {
            bool ok = false; double v = input->text().toDouble(&ok);
            if (ok) ConfigManager::instance().set("vision.confidenceThreshold", v);
        });
        rl->addWidget(input);
        rl->addStretch();
        layout->addWidget(row);
    }

    // Row 2: 深度过滤 Z min / Z max
    {
        auto* row = new QWidget();
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(18);

        rl->addWidget(makeLabel(QStringLiteral("深度过滤 Z min")));
        auto* minInput = makeEdit(70);
        minInput->setText(QString::number(dVal("vision.depthZMin", 10)));
        QObject::connect(minInput, &QLineEdit::editingFinished, [minInput]() {
            bool ok = false; double v = minInput->text().toDouble(&ok);
            if (ok) ConfigManager::instance().set("vision.depthZMin", v);
        });
        rl->addWidget(minInput);

        rl->addWidget(makeLabel(QStringLiteral("Z max")));
        auto* maxInput = makeEdit(70);
        maxInput->setText(QString::number(dVal("vision.depthZMax", 120)));
        QObject::connect(maxInput, &QLineEdit::editingFinished, [maxInput]() {
            bool ok = false; double v = maxInput->text().toDouble(&ok);
            if (ok) ConfigManager::instance().set("vision.depthZMax", v);
        });
        rl->addWidget(maxInput);

        rl->addStretch();
        layout->addWidget(row);
    }

    layout->addStretch();
    return tab;
}

// ===========================================================================
//  Tab 4 — TCP与标定
// ===========================================================================
QWidget* ConfigPage::CreateTab4TCP()
{
    auto* tab = new QWidget();
    tab->setStyleSheet("background: transparent;");
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(12);

    auto makeLabel = [](const QString& text) {
        auto* l = new QLabel(text);
        l->setStyleSheet("color: #b8cce3; min-width: 90px; font-size: 13px; background: transparent; border: none;");
        return l;
    };
    auto makeCoordWidget = [](const QString& coord, const char* path, double def) -> QWidget* {
        auto* w = new QWidget();
        auto* l = new QHBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(4);

        auto* lbl = new QLabel(coord);
        lbl->setStyleSheet("color: #b8cce3; font-size: 13px; background: transparent; border: none;");

        auto* input = new QLineEdit(QString::number(dVal(path, def)));
        input->setFixedWidth(60);
        input->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px 8px; border-radius: 6px;");
        QObject::connect(input, &QLineEdit::editingFinished, [input, p = std::string(path)]() {
            bool ok = false; double v = input->text().toDouble(&ok);
            if (ok) ConfigManager::instance().set(p, v);
        });

        l->addWidget(lbl);
        l->addWidget(input);
        return w;
    };

    // Row 1: 工具 XYZ 偏移
    {
        auto* row = new QWidget();
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(18);

        rl->addWidget(makeLabel(QStringLiteral("工具 XYZ 偏移")));
        rl->addWidget(makeCoordWidget(QStringLiteral("X"), "tcpCalibration.toolOffsetX", 12.5));
        rl->addWidget(makeCoordWidget(QStringLiteral("Y"), "tcpCalibration.toolOffsetY", -3.2));
        rl->addWidget(makeCoordWidget(QStringLiteral("Z"), "tcpCalibration.toolOffsetZ", 45.0));
        rl->addStretch();
        layout->addWidget(row);
    }

    // Row 2: calibration button + label
    {
        auto* row = new QWidget();
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(18);

        auto* calibBtn = new QPushButton(QStringLiteral("执行九点标定"));
        calibBtn->setStyleSheet("QPushButton { background: #7f5f3f; border: none; border-radius: 8px; padding: 8px 16px; font-weight: 600; font-size: 13px; color: white; } QPushButton:hover { background: #9f7f4f; }");
        calibBtn->setCursor(Qt::PointingHandCursor);
        QObject::connect(calibBtn, &QPushButton::clicked, this, []() {
                SPDLOG_INFO("[Calibration] 九点标定按钮 pressed (stub)");
        });

        auto* matrixLabel = new QLabel(QStringLiteral("外参矩阵 (4x4)"));
        matrixLabel->setStyleSheet("color: #7f9fc0; background: transparent; border: none;");

        rl->addWidget(calibBtn);
        rl->addWidget(matrixLabel);
        rl->addStretch();
        layout->addWidget(row);
    }

    // Row 3: matrix display
    {
        auto* matrixBox = new QWidget();
        matrixBox->setStyleSheet("background: #0d141c; padding: 8px; border-radius: 8px;");
        auto* ml = new QVBoxLayout(matrixBox);
        ml->setSpacing(2);

        auto makeRow = [](const QString& text) {
            auto* lbl = new QLabel(text);
            lbl->setStyleSheet("color: #799bbf; font-family: 'Consolas', monospace; font-size: 13px; background: transparent; border: none;");
            return lbl;
        };
        ml->addWidget(makeRow(QStringLiteral("[ 1.000  0.000  0.000  12.300 ]")));
        ml->addWidget(makeRow(QStringLiteral("[ 0.000  1.000  0.000  -5.700 ]")));
        ml->addWidget(makeRow(QStringLiteral("[ 0.000  0.000  1.000  38.100 ]")));
        ml->addWidget(makeRow(QStringLiteral("[ 0.000  0.000  0.000  1.000  ]")));

        layout->addWidget(matrixBox);
    }

    layout->addStretch();
    return tab;
}

// ===========================================================================
//  Tab 5 — 电控与映射
// ===========================================================================
QWidget* ConfigPage::CreateElecMapTab()
{
    auto* tab = new QWidget();
    tab->setStyleSheet("background: transparent;");
    auto* outer = new QHBoxLayout(tab);
    outer->setContentsMargins(0, 12, 0, 0);
    outer->setSpacing(12);

    // ── Left: axis list ────────────────────────
    axisList_ = new QListWidget();
    axisList_->setFixedWidth(180);
    axisList_->setStyleSheet(R"(
        QListWidget {
            background: #111a22; border: none; border-radius: 8px;
            padding: 4px; outline: none;
        }
        QListWidget::item {
            min-height: 36px; padding: 6px 12px;
            color: #b8cce3; border-radius: 6px;
        }
        QListWidget::item:selected { background: #2f6f9f; color: white; }
        QListWidget::item:hover:!selected { background: #1e2a36; }
    )");

    // Helper: check if a (hardwareType, portId) pair already exists on another axis
    auto hasDuplicate = [](int hwType, int portId, const std::string& excludeKey) -> bool {
        auto& cfg = ConfigManager::instance();
        if (!cfg.root().contains("axes") || !cfg.root()["axes"].is_object()) return false;
        for (const auto& [key, val] : cfg.root()["axes"].items()) {
            if (key == excludeKey) continue;
            if (!val.is_object()) continue;
            int otherType = val.value("hardwareType", -1);
            int otherPort = val.value("portId", -1);
            if (otherType == hwType && otherPort == portId) return true;
        }
        return false;
    };

    // Load axes from config object, sorted by sortOrder
    auto& cfg = ConfigManager::instance();
    if (cfg.root().contains("axes") && cfg.root()["axes"].is_object()) {
        struct AxisEntry { std::string key; QString name; int sortOrder; };
        std::vector<AxisEntry> entries;
        for (const auto& [key, val] : cfg.root()["axes"].items()) {
            QString name = QString::fromStdString(
                val.is_object() && val.contains("name")
                    ? val["name"].get<std::string>() : "?");
            int order = (val.is_object() && val.contains("sortOrder"))
                ? val["sortOrder"].get<int>() : 0;
            entries.push_back({key, name, order});
        }
        std::sort(entries.begin(), entries.end(),
            [](const AxisEntry& a, const AxisEntry& b) { return a.sortOrder < b.sortOrder; });
        for (const auto& e : entries) {
            auto* item = new QListWidgetItem(e.name);
            item->setData(Qt::UserRole, QString::fromStdString(e.key));
            axisList_->addItem(item);
        }
    }
    if (axisList_->count() > 0)
        axisList_->setCurrentRow(0);

    outer->addWidget(axisList_);

    // ── Right: QScrollArea ─────────────────────
    auto* scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setStyleSheet(R"(
        QScrollArea { background: transparent; }
        QScrollBar:vertical {
            width: 28px; background: #1a2430; border: none; margin: 0;
        }
        QScrollBar::handle:vertical {
            background: #ffffff; min-height: 50px; border-radius: 8px; margin: 2px 4px;
        }
        QScrollBar::handle:vertical:hover { background: #e0e8f0; }
        QScrollBar::handle:vertical:pressed { background: #c0d0e0; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; border: none; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: #1a2430; }
    )");

    auto* scrollWidget = new QWidget();
    scrollWidget->setStyleSheet("background: transparent;");
    auto* scrollLayout = new QVBoxLayout(scrollWidget);
    scrollLayout->setContentsMargins(0, 0, 0, 0);
    scrollLayout->setSpacing(10);

    // Title
    axisTitle_ = new QLabel();
    axisTitle_->setStyleSheet(R"(
        QLabel {
            background: #0d141c; border: 1px solid #2f7fb5; border-radius: 8px;
            padding: 8px 14px; font-size: 14px; font-weight: 700; color: #cde2ff;
        }
    )");
    scrollLayout->addWidget(axisTitle_);

    // ── Helper lambdas for form creation ──────
    auto makeLabel = [](const QString& text) -> QLabel* {
        auto* l = new QLabel(text);
        l->setMinimumSize(180, 32);
        l->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        l->setStyleSheet("QLabel { color: #b8cce3; background: transparent; border: none; font-size: 13px; }");
        return l;
    };

    auto makeInput = [](int width = 70) -> QLineEdit* {
        auto* e = new QLineEdit();
        e->setFixedWidth(width);
        e->setFixedHeight(32);
        e->setStyleSheet("QLineEdit { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; font-size: 13px; padding: 4px 8px; }");
        return e;
    };

    auto makeDoubleSpin = [](double min, double max, int width = 80) -> QDoubleSpinBox* {
        auto* s = new QDoubleSpinBox();
        s->setRange(min, max);
        s->setFixedWidth(width);
        s->setDecimals(2);
        s->setFixedHeight(32);
        s->setStyleSheet("QDoubleSpinBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; font-size: 13px; padding: 4px 8px; }");
        return s;
    };

    auto makeCombo = [](const QStringList& items) -> QComboBox* {
        auto* c = new QComboBox();
        c->addItems(items);
        c->setFixedHeight(32);
        c->setStyleSheet(R"(
            QComboBox {
                background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0;
                padding: 4px 24px 4px 8px; border-radius: 6px; font-size: 13px;
            }
            QComboBox::drop-down {
                subcontrol-origin: padding; subcontrol-position: top right;
                width: 20px; border: none;
            }
            QComboBox::down-arrow {
                image: none;
                border-left: 5px solid transparent;
                border-right: 5px solid transparent;
                border-top: 6px solid #8da3bb;
                margin-right: 4px;
            }
            QComboBox:hover { border: 1px solid #4f7faf; }
            QAbstractItemView {
                background: #1a222b; outline: none;
                selection-background-color: #2f6f9f; selection-color: white;
                color: #dbe6f0;
            }
        )");
        c->setCursor(Qt::PointingHandCursor);
        return c;
    };

    auto makeForm = [](QWidget* parent) -> QFormLayout* {
        auto* f = new QFormLayout(parent);
        f->setVerticalSpacing(10);
        f->setHorizontalSpacing(18);
        f->setContentsMargins(8, 18, 8, 8);
        f->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        f->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        f->setRowWrapPolicy(QFormLayout::WrapLongRows);
        return f;
    };

    const auto groupBoxStyle = R"(
        QGroupBox {
            font-weight:600;
            border:1px solid #2f6f9f;
            border-radius:8px;
            margin-top:14px;
            color:#b8cce3;
            font-size:13px;
        }
        QGroupBox::title {
            subcontrol-origin: margin; subcontrol-position: top left;
            padding: 0 10px; color: #7fbfdf; font-size: 13px;
        }
    )";

    // ── Build the right-side widgets (pointers stored for axis-switching) ──
    // We'll group them in a container widget and rebuild values on selection

    // Group 1: 硬件映射
    auto* grp1 = new QGroupBox(QStringLiteral("硬件映射"));
    grp1->setStyleSheet(groupBoxStyle);
    auto* g1g = makeForm(grp1);
    g1g->setContentsMargins(8, 22, 8, 8);

    auto* hwTypeCombo = makeCombo({ QStringLiteral("运动控制卡 (发脉冲)"), QStringLiteral("串口总线舵机") });
    auto* portSpin = new QSpinBox();
    portSpin->setRange(0, 255);
    portSpin->setFixedWidth(70);
    portSpin->setFixedHeight(32);
    portSpin->setStyleSheet("QSpinBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; font-size: 13px; padding: 4px 8px; }");
    auto* dirCombo = makeCombo({ QStringLiteral("正向 (Normal)"), QStringLiteral("反向 (Inverted)") });

    g1g->addRow(makeLabel(QStringLiteral("硬件类型")), hwTypeCombo);
    g1g->addRow(makeLabel(QStringLiteral("物理端口ID")), portSpin);
    g1g->addRow(makeLabel(QStringLiteral("电机方向")), dirCombo);

    // Group 2: 安全与运动限制
    auto* grp2 = new QGroupBox(QStringLiteral("安全与运动限制"));
    grp2->setStyleSheet(groupBoxStyle);
    auto* g2g = makeForm(grp2);
    g2g->setContentsMargins(8, 22, 8, 8);

    auto* maxSpeedSpin = makeDoubleSpin(0.0, 10000.0);
    auto* maxAccelSpin = makeDoubleSpin(0.0, 100000.0);
    auto* jogSpeedSpin = makeDoubleSpin(0.0, 10000.0);
    auto* limitMinSpin = makeDoubleSpin(-100000.0, 0.0);
    auto* limitMaxSpin = makeDoubleSpin(0.0, 100000.0);
    auto* homeSpin = makeDoubleSpin(-100000.0, 100000.0);

    g2g->addRow(makeLabel(QStringLiteral("最大速度 (Max Speed)")),   maxSpeedSpin);
    g2g->addRow(makeLabel(QStringLiteral("最大加速度 (Max Accel)")), maxAccelSpin);
    g2g->addRow(makeLabel(QStringLiteral("点动速度 (Jog Speed)")),   jogSpeedSpin);
    g2g->addRow(makeLabel(QStringLiteral("软限位 Min (Limit Min)")), limitMinSpin);
    g2g->addRow(makeLabel(QStringLiteral("软限位 Max (Limit Max)")), limitMaxSpin);
    g2g->addRow(makeLabel(QStringLiteral("原点偏移 (Home Pos)")),    homeSpin);

    // Group 3: 传动与换算参数
    auto* grp3 = new QGroupBox(QStringLiteral("传动与换算参数"));
    grp3->setStyleSheet(groupBoxStyle);
    auto* g3l = new QVBoxLayout(grp3);
    g3l->setContentsMargins(0, 0, 0, 0);
    g3l->setSpacing(0);

    transStack_ = new QStackedWidget();
    transStack_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    transStack_->setMinimumHeight(260);
    transStack_->setStyleSheet("background: transparent;");

    // Page A — 控制卡
    auto* pageA = new QWidget();
    auto* pa = makeForm(pageA);
    pa->setContentsMargins(8, 22, 8, 8);
    auto* encoderEdit = makeInput();
    auto* microStepEdit = makeInput();
    auto* gearEdit = makeInput();
    auto* leadEdit = makeInput();
    pa->addRow(makeLabel(QStringLiteral("编码器分辨率 (Pulse/Rev)")), encoderEdit);
    pa->addRow(makeLabel(QStringLiteral("细分数 (MicroSteps)")),      microStepEdit);
    pa->addRow(makeLabel(QStringLiteral("减速比 (Gear Ratio)")),      gearEdit);
    pa->addRow(makeLabel(QStringLiteral("导程 (Lead)")),              leadEdit);
    transStack_->addWidget(pageA);

    // Page B — 舵机
    auto* pageB = new QWidget();
    auto* pb = makeForm(pageB);
    pb->setContentsMargins(8, 22, 8, 8);
    auto* minPulseEdit = makeInput();
    auto* maxPulseEdit = makeInput();
    auto* minAngleEdit = makeInput();
    auto* maxAngleEdit = makeInput();
    pb->addRow(makeLabel(QStringLiteral("最小控制值 (Min Pulse)")), minPulseEdit);
    pb->addRow(makeLabel(QStringLiteral("最大控制值 (Max Pulse)")), maxPulseEdit);
    pb->addRow(makeLabel(QStringLiteral("最小物理角度 (Min Angle)")), minAngleEdit);
    pb->addRow(makeLabel(QStringLiteral("最大物理角度 (Max Angle)")), maxAngleEdit);
    transStack_->addWidget(pageB);

    transStack_->setCurrentIndex(0);
    g3l->addWidget(transStack_);

    // ── Function to load axis data by key ───────────────────
    auto loadAxis = [this, hwTypeCombo, portSpin, dirCombo, maxSpeedSpin, maxAccelSpin,
                     jogSpeedSpin, limitMinSpin, limitMaxSpin, homeSpin, encoderEdit,
                     microStepEdit, gearEdit, leadEdit, minPulseEdit, maxPulseEdit,
                     minAngleEdit, maxAngleEdit](const QString& key) {
        try {
            if (key.isEmpty()) {
                SPDLOG_INFO("[ElecMap] loadAxis: empty key");
                return;
            }
            std::string p = "axes." + key.toStdString();
            SPDLOG_INFO("[ElecMap] loadAxis key {}", key.toStdString());

            auto& cfg = ConfigManager::instance();
            auto name = cfg.getValue<std::string>(p + ".name", "?");
            axisTitle_->setText(QStringLiteral("当前编辑：%1").arg(QString::fromStdString(name)));

            int hwType = cfg.getValue<int>(p + ".hardwareType", 0);
            hwTypeCombo->blockSignals(true);
            hwTypeCombo->setCurrentIndex(hwType);
            hwTypeCombo->blockSignals(false);

            portSpin->blockSignals(true);
            portSpin->setValue(cfg.getValue<int>(p + ".portId", 1));
            portSpin->blockSignals(false);

            dirCombo->blockSignals(true);
            dirCombo->setCurrentIndex(cfg.getValue<int>(p + ".direction", 0));
            dirCombo->blockSignals(false);

            maxSpeedSpin->blockSignals(true);
            maxSpeedSpin->setValue(cfg.getValue<double>(p + ".maxSpeed", 150.0));
            maxSpeedSpin->blockSignals(false);

            maxAccelSpin->blockSignals(true);
            maxAccelSpin->setValue(cfg.getValue<double>(p + ".maxAccel", 500.0));
            maxAccelSpin->blockSignals(false);

            jogSpeedSpin->blockSignals(true);
            jogSpeedSpin->setValue(cfg.getValue<double>(p + ".jogSpeed", 100.0));
            jogSpeedSpin->blockSignals(false);

            limitMinSpin->blockSignals(true);
            limitMinSpin->setValue(cfg.getValue<double>(p + ".limitMin", -180.0));
            limitMinSpin->blockSignals(false);

            limitMaxSpin->blockSignals(true);
            limitMaxSpin->setValue(cfg.getValue<double>(p + ".limitMax", 180.0));
            limitMaxSpin->blockSignals(false);

            homeSpin->blockSignals(true);
            homeSpin->setValue(cfg.getValue<double>(p + ".homeOffset", 0.0));
            homeSpin->blockSignals(false);

            std::string tp = p + ".transmission";
            encoderEdit->setText(QString::number(cfg.getValue<int>(tp + ".encoderResolution", 131072)));
            microStepEdit->setText(QString::number(cfg.getValue<int>(tp + ".microSteps", 512)));
            gearEdit->setText(QString::number(cfg.getValue<int>(tp + ".gearRatio", 50)));
            leadEdit->setText(QString::number(cfg.getValue<double>(tp + ".lead", 20.0)));
            minPulseEdit->setText(QString::number(cfg.getValue<int>(tp + ".minPulse", 500)));
            maxPulseEdit->setText(QString::number(cfg.getValue<int>(tp + ".maxPulse", 2500)));
            minAngleEdit->setText(QString::number(cfg.getValue<int>(tp + ".minAngle", 0)));
            maxAngleEdit->setText(QString::number(cfg.getValue<int>(tp + ".maxAngle", 180)));

            transStack_->setCurrentIndex(hwType);

            SPDLOG_INFO("[ElecMap] loadAxis key {} done", key.toStdString());
        } catch (const std::exception& e) {
            SPDLOG_INFO("[ElecMap] loadAxis exception: {}", e.what());
        } catch (...) {
            SPDLOG_INFO("[ElecMap] loadAxis unknown exception");
        }
    };

    // ── Connect axis list selection ─────────────────────────
    connect(axisList_, &QListWidget::currentRowChanged, this, [this, loadAxis](int row) {
        auto* item = axisList_->item(row);
        if (item) {
            QString key = item->data(Qt::UserRole).toString();
            loadAxis(key);
        }
    });

    // ── Connect auto-save for all widgets ───────────────────
    auto pathFor = [this](const char* field) -> std::string {
        auto* item = axisList_->currentItem();
        if (!item) return "";
        QString key = item->data(Qt::UserRole).toString();
        if (key.isEmpty()) return "";
        return "axes." + key.toStdString() + "." + field;
    };

    connect(hwTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
        [this, portSpin, hasDuplicate, hwTypeCombo](int idx) {
        auto* item = axisList_->currentItem();
        if (!item) return;
        QString key = item->data(Qt::UserRole).toString();
        int port = portSpin->value();
        if (hasDuplicate(idx, port, key.toStdString())) {
            auto& cfg = ConfigManager::instance();
            std::string p = "axes." + key.toStdString();
            int prevHwType = cfg.getValue<int>(p + ".hardwareType", 0);
            hwTypeCombo->blockSignals(true);
            hwTypeCombo->setCurrentIndex(prevHwType);
            hwTypeCombo->blockSignals(false);
            QMessageBox::warning(this,
                QStringLiteral("端口冲突"),
                QStringLiteral("同类型硬件端口 %1 已被占用，请选择其他端口。").arg(port));
            return;
        }
        ConfigManager::instance().set("axes." + key.toStdString() + ".hardwareType", idx);
        transStack_->setCurrentIndex(idx);
    });

    connect(portSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
        [this, hwTypeCombo, hasDuplicate, portSpin](int v) {
        auto* item = axisList_->currentItem();
        if (!item) return;
        QString key = item->data(Qt::UserRole).toString();
        int hwType = hwTypeCombo->currentIndex();
        if (hasDuplicate(hwType, v, key.toStdString())) {
            auto& cfg = ConfigManager::instance();
            std::string p = "axes." + key.toStdString();
            int prevPort = cfg.getValue<int>(p + ".portId", 1);
            portSpin->blockSignals(true);
            portSpin->setValue(prevPort);
            portSpin->blockSignals(false);
            QMessageBox::warning(this,
                QStringLiteral("端口冲突"),
                QStringLiteral("同类型硬件端口 %1 已被占用，请选择其他端口。").arg(v));
            return;
        }
        ConfigManager::instance().set("axes." + key.toStdString() + ".portId", v);
    });

    connect(dirCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, pathFor](int idx) {
        auto p = pathFor("direction");
        if (!p.empty()) ConfigManager::instance().set(p, idx);
    });

    connect(maxSpeedSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, pathFor](double v) {
        auto p = pathFor("maxSpeed");
        if (!p.empty()) ConfigManager::instance().set(p, v);
    });

    connect(maxAccelSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, pathFor](double v) {
        auto p = pathFor("maxAccel");
        if (!p.empty()) ConfigManager::instance().set(p, v);
    });

    connect(jogSpeedSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, pathFor](double v) {
        auto p = pathFor("jogSpeed");
        if (!p.empty()) ConfigManager::instance().set(p, v);
    });

    connect(limitMinSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, pathFor](double v) {
        auto p = pathFor("limitMin");
        if (!p.empty()) ConfigManager::instance().set(p, v);
    });

    connect(limitMaxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, pathFor](double v) {
        auto p = pathFor("limitMax");
        if (!p.empty()) ConfigManager::instance().set(p, v);
    });

    connect(homeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, pathFor](double v) {
        auto p = pathFor("homeOffset");
        if (!p.empty()) ConfigManager::instance().set(p, v);
    });

    auto connectTransEdit = [this, pathFor](QLineEdit* edit, const char* field) {
        connect(edit, &QLineEdit::editingFinished, this, [this, edit, p = std::string(field), pathFor]() {
            auto path = pathFor(p.c_str());
            if (path.empty()) return;
            bool ok = false; double v = edit->text().toDouble(&ok);
            if (ok) ConfigManager::instance().set(path, v);
        });
    };
    connectTransEdit(encoderEdit, "transmission.encoderResolution");
    connectTransEdit(microStepEdit, "transmission.microSteps");
    connectTransEdit(gearEdit, "transmission.gearRatio");
    connectTransEdit(leadEdit, "transmission.lead");
    connectTransEdit(minPulseEdit, "transmission.minPulse");
    connectTransEdit(maxPulseEdit, "transmission.maxPulse");
    connectTransEdit(minAngleEdit, "transmission.minAngle");
    connectTransEdit(maxAngleEdit, "transmission.maxAngle");

    scrollLayout->addWidget(axisTitle_);
    grp1->setMinimumHeight(180);
    scrollLayout->addWidget(grp1);
    grp2->setMinimumHeight(260);
    scrollLayout->addWidget(grp2);
    scrollLayout->addWidget(grp3);
    scrollLayout->addStretch();

    scrollArea->setWidget(scrollWidget);
    outer->addWidget(scrollArea, 1);

    if (axisList_->count() > 0) {
        auto* firstItem = axisList_->item(0);
        if (firstItem)
            loadAxis(firstItem->data(Qt::UserRole).toString());
    }

    return tab;
}
