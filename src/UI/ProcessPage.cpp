#include "ProcessPage.h"
#include "ProcessManager.h"
#include "KinematicsHelper.h"
#include "HAL/core/HardwareManager.h"
#include "Logic/SequenceWorker.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QFont>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QDebug>
#include <QPainter>
#include <QPixmap>
#include <QIcon>

#include <spdlog/spdlog.h>

namespace {

QIcon makeActionIcon(int kind)
{
    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor("#ffffff"), 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    switch (kind) {
    case 0: // plus
        p.drawLine(QPointF(10, 3.5), QPointF(10, 16.5));
        p.drawLine(QPointF(3.5, 10), QPointF(16.5, 10));
        break;
    case 1: // minus
        p.drawLine(QPointF(3.5, 10), QPointF(16.5, 10));
        break;
    case 2: // pen
        p.drawLine(QPointF(5, 15), QPointF(13, 7));
        p.drawLine(QPointF(13, 7), QPointF(16.5, 3.5));
        p.drawLine(QPointF(13, 7), QPointF(8.5, 8.5));
        break;
    case 3: // up arrow
        p.drawLine(QPointF(10, 16), QPointF(10, 4));
        p.drawLine(QPointF(4, 9), QPointF(10, 3));
        p.drawLine(QPointF(10, 3), QPointF(16, 9));
        break;
    case 4: // down arrow
        p.drawLine(QPointF(10, 4), QPointF(10, 16));
        p.drawLine(QPointF(4, 11), QPointF(10, 17));
        p.drawLine(QPointF(10, 17), QPointF(16, 11));
        break;
    case 5: { // play triangle（运行/执行）
        QPolygonF tri;
        tri << QPointF(7.0, 4.5) << QPointF(15.5, 10.0) << QPointF(7.0, 15.5);
        p.setBrush(Qt::white);
        p.drawPolygon(tri);
        break;
    }
    }
    p.end();
    return QIcon(pm);
}

} // namespace

ProcessPage::ProcessPage(QWidget* parent)
    : QWidget(parent)
{
    SetupUI();
    ProcessManager::instance().load();
    if (!ProcessManager::instance().schemes().isEmpty()) {
        m_currentSchemeIdx = 0;
        m_schemeNameEdit->setText(ProcessManager::instance().schemes()[0].schemeName);
        RefreshSchemeCombo();
        RefreshActionList();
    }
}

void ProcessPage::SetupUI()
{
    setStyleSheet("background: #262c34;");

    auto makeBtnStyle = [](const QString& bg, const QString& hover) {
        return QStringLiteral("QPushButton { background: %1; border: none; border-radius: 8px; padding: 8px 16px; font-weight: 600; font-size: 14px; color: white; } QPushButton:hover { background: %2; }").arg(bg, hover);
    };

    auto makeFormPage = [](QWidget* parent) -> QFormLayout* {
        auto* f = new QFormLayout(parent);
        f->setVerticalSpacing(10);
        f->setHorizontalSpacing(18);
        f->setContentsMargins(14, 18, 14, 8);
        f->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        f->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        return f;
    };

    auto makeLabel = [](const QString& text) -> QLabel* {
        auto* l = new QLabel(text);
        l->setMinimumSize(100, 28);
        l->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        l->setStyleSheet("color: #b8cce3; background: transparent; border: none; font-size: 13px;");
        return l;
    };

    auto makeFieldEdit = []() -> QLineEdit* {
        auto* e = new QLineEdit();
        e->setFixedHeight(30);
        e->setStyleSheet("QLineEdit { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; font-size: 13px; padding: 4px 8px; }");
        return e;
    };

    auto saveBtnStyle = makeBtnStyle("#2f7f5f", "#3f9f7f");

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(14, 14, 14, 14);
    mainLayout->setSpacing(12);

    // Top bar：两行布局——第一行方案管理，第二行执行控制 + 执行状态
    auto* topBar = new QWidget();
    topBar->setStyleSheet("background: #1b222b; border-radius: 12px;");
    topBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* topLayout = new QVBoxLayout(topBar);
    topLayout->setContentsMargins(16, 8, 16, 8);
    topLayout->setSpacing(8);

    auto* schemeRow = new QHBoxLayout();
    schemeRow->setSpacing(14);
    auto* execRow = new QHBoxLayout();
    execRow->setSpacing(14);

    auto* newSchemeBtn = new QPushButton(QStringLiteral("新增方案"));
    newSchemeBtn->setStyleSheet(makeBtnStyle("#2f6f9f", "#3a84b8"));
    newSchemeBtn->setCursor(Qt::PointingHandCursor);
    connect(newSchemeBtn, &QPushButton::clicked, this, &ProcessPage::OnNewScheme);

    auto* label = new QLabel(QStringLiteral("当前方案"));
    label->setStyleSheet("color: #b8cce3; font-size: 13px; font-weight: 500; background: transparent; border: none;");

    m_schemeNameEdit = new QLineEdit();
    m_schemeNameEdit->setFixedWidth(150);
    m_schemeNameEdit->setFixedHeight(30);
    m_schemeNameEdit->setStyleSheet("QLineEdit { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px 8px; border-radius: 6px; font-size: 13px; }");
    connect(m_schemeNameEdit, &QLineEdit::editingFinished, this, &ProcessPage::OnSchemeNameEdited);

    m_schemeCombo = new QComboBox();
    m_schemeCombo->setFixedWidth(140);
    m_schemeCombo->setFixedHeight(30);
    m_schemeCombo->setStyleSheet(R"(
        QComboBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; padding: 4px 8px; font-size: 13px; }
        QComboBox::drop-down { width: 20px; border: none; }
        QComboBox:hover { border: 1px solid #4f7faf; }
        QAbstractItemView { background: #1a222b; outline: none; selection-background-color: #2f6f9f; selection-color: white; color: #dbe6f0; }
    )");

    auto* confirmBtn = new QPushButton(QStringLiteral("切换方案"));
    confirmBtn->setStyleSheet(makeBtnStyle("#2f6f9f", "#3a84b8"));
    confirmBtn->setCursor(Qt::PointingHandCursor);
    connect(confirmBtn, &QPushButton::clicked, this, &ProcessPage::OnConfirmSwitch);

    auto* deleteSchemeBtn = new QPushButton(QStringLiteral("删除方案"));
    deleteSchemeBtn->setStyleSheet(makeBtnStyle("#8f4f4f", "#aa5f5f"));
    deleteSchemeBtn->setCursor(Qt::PointingHandCursor);
    connect(deleteSchemeBtn, &QPushButton::clicked, this, &ProcessPage::OnDeleteScheme);

    m_stepBtn = new QPushButton(QStringLiteral("单步执行"));
    m_stepBtn->setStyleSheet(makeBtnStyle("#8f7f3f", "#9f8f4f"));
    m_stepBtn->setCursor(Qt::PointingHandCursor);
    connect(m_stepBtn, &QPushButton::clicked, this, &ProcessPage::OnStepExecute);

    // 单步调试的就地"刹车"：防走错轨迹时瞬间停止（后续接 SequenceWorker::Stop()，
    // 届时由 MainWindow 注入 worker；全局急停仍在 MainWindow 顶栏，与此业务停止区分）
    auto* stopBtn = new QPushButton(QStringLiteral("停止"));
    stopBtn->setStyleSheet(makeBtnStyle("#b13a3a", "#d14444"));
    stopBtn->setCursor(Qt::PointingHandCursor);
    connect(stopBtn, &QPushButton::clicked, this, [this]() {
        if (m_worker) m_worker->Stop();
        ResetStepSession();
        // 立刻覆盖上一次的终态（如「✅ 完成」），避免下次点击前误导。
        // 若 worker 正在运行，后续 interrupted 会给出更准确的终态并覆盖此处。
        SetStatusText(QStringLiteral("■ 已停止（保持使能）"), QStringLiteral("#b8cce3"));
        SPDLOG_INFO("[ProcessPage] 停止按钮：业务停止（保持使能，可重新单步）");
    });

    // 执行状态标签：stateChanged 由 worker 线程 emit，此处仅在主线程更新 UI
    m_statusLabel = new QLabel(QStringLiteral("● 空闲"));
    m_statusLabel->setMinimumWidth(130);
    m_statusLabel->setStyleSheet("color: #b8cce3; font-size: 13px; font-weight: 600; background: transparent; border: none;");

    schemeRow->addWidget(label);
    schemeRow->addWidget(m_schemeNameEdit);
    schemeRow->addWidget(m_schemeCombo);
    schemeRow->addWidget(newSchemeBtn);
    schemeRow->addWidget(confirmBtn);
    schemeRow->addWidget(deleteSchemeBtn);
    schemeRow->addStretch();

    auto* execLabel = new QLabel(QStringLiteral("方案调试"));
    execLabel->setStyleSheet("color: #b8cce3; font-size: 13px; font-weight: 500; background: transparent; border: none;");

    execRow->addWidget(execLabel);
    execRow->addWidget(m_stepBtn);
    execRow->addWidget(stopBtn);
    execRow->addWidget(m_statusLabel);
    execRow->addStretch();

    topLayout->addLayout(schemeRow);
    topLayout->addLayout(execRow);

    mainLayout->addWidget(topBar);

    // Split
    auto* splitWidget = new QWidget();
    splitWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* splitLayout = new QHBoxLayout(splitWidget);
    splitLayout->setContentsMargins(0, 0, 0, 0);
    splitLayout->setSpacing(16);

    // Left
    auto* leftPanel = new QWidget();
    leftPanel->setStyleSheet("background: #1b222b; border-radius: 12px; border: 1px solid #384550;");
    leftPanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(10, 10, 10, 10);
    leftLayout->setSpacing(8);

    auto* actionHeader = new QLabel(QStringLiteral("动作列表"));
    actionHeader->setStyleSheet("color: #b8cce3; font-size: 13px; font-weight: 500; background: transparent; border: none;");

    m_actionList = new QListWidget();
    m_actionList->setStyleSheet(R"(
        QListWidget { background: #121a22; border: none; border-radius: 8px; padding: 4px; outline: none; }
        QListWidget::item { min-height: 32px; padding: 4px 10px; color: #b8cce3; border-radius: 4px; font-size: 13px; }
        QListWidget::item:selected { background: #2f6f9f; color: white; }
        QListWidget::item:hover:!selected { background: #1e2a36; }
        QScrollBar:vertical { width: 28px; background: #1a2430; border: none; margin: 0; }
        QScrollBar::handle:vertical { background: #ffffff; min-height: 50px; border-radius: 8px; margin: 2px 4px; }
        QScrollBar::handle:vertical:hover { background: #e0e8f0; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; border: none; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: #1a2430; }
    )");
    connect(m_actionList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && m_currentSchemeIdx >= 0 &&
            m_currentSchemeIdx < ProcessManager::instance().schemes().size()) {
            m_currentActionIdx = row;
            RefreshActionDetail(row);
        }
    });

    auto* actionBtnRow = new QHBoxLayout();
    actionBtnRow->setSpacing(8);

    auto* newActionBtn = new QPushButton();
    newActionBtn->setIcon(makeActionIcon(0));
    newActionBtn->setIconSize(QSize(18, 18));
    newActionBtn->setToolTip(QStringLiteral("新建动作"));
    newActionBtn->setStyleSheet(makeBtnStyle("#3a6f8f", "#4a7f9f"));
    newActionBtn->setCursor(Qt::PointingHandCursor);
    newActionBtn->setFixedSize(40, 32);
    connect(newActionBtn, &QPushButton::clicked, this, &ProcessPage::OnNewAction);

    auto* editActionBtn = new QPushButton();
    editActionBtn->setIcon(makeActionIcon(2));
    editActionBtn->setIconSize(QSize(18, 18));
    editActionBtn->setToolTip(QStringLiteral("编辑动作"));
    editActionBtn->setStyleSheet(makeBtnStyle("#4f5f6f", "#5f6f7f"));
    editActionBtn->setCursor(Qt::PointingHandCursor);
    editActionBtn->setFixedSize(40, 32);
    connect(editActionBtn, &QPushButton::clicked, this, &ProcessPage::OnEditAction);

    auto* deleteActionBtn = new QPushButton();
    deleteActionBtn->setIcon(makeActionIcon(1));
    deleteActionBtn->setIconSize(QSize(18, 18));
    deleteActionBtn->setToolTip(QStringLiteral("删除动作"));
    deleteActionBtn->setStyleSheet(makeBtnStyle("#8f4f4f", "#aa5f5f"));
    deleteActionBtn->setCursor(Qt::PointingHandCursor);
    deleteActionBtn->setFixedSize(40, 32);
    connect(deleteActionBtn, &QPushButton::clicked, this, &ProcessPage::OnDeleteAction);

    auto* upActionBtn = new QPushButton();
    upActionBtn->setIcon(makeActionIcon(3));
    upActionBtn->setIconSize(QSize(18, 18));
    upActionBtn->setToolTip(QStringLiteral("上移"));
    upActionBtn->setStyleSheet(makeBtnStyle("#4f6f5f", "#5f8f6f"));
    upActionBtn->setCursor(Qt::PointingHandCursor);
    upActionBtn->setFixedSize(40, 32);
    connect(upActionBtn, &QPushButton::clicked, this, &ProcessPage::OnMoveActionUp);

    auto* downActionBtn = new QPushButton();
    downActionBtn->setIcon(makeActionIcon(4));
    downActionBtn->setIconSize(QSize(18, 18));
    downActionBtn->setToolTip(QStringLiteral("下移"));
    downActionBtn->setStyleSheet(makeBtnStyle("#4f6f5f", "#5f8f6f"));
    downActionBtn->setCursor(Qt::PointingHandCursor);
    downActionBtn->setFixedSize(40, 32);
    connect(downActionBtn, &QPushButton::clicked, this, &ProcessPage::OnMoveActionDown);

    // 选中动作单独执行：不整跑方案，只跑当前选中动作（含其全部点位），绿色运行图标
    m_runSelectedBtn = new QPushButton();
    m_runSelectedBtn->setIcon(makeActionIcon(5));
    m_runSelectedBtn->setIconSize(QSize(18, 18));
    m_runSelectedBtn->setToolTip(QStringLiteral("执行选中动作"));
    m_runSelectedBtn->setStyleSheet(makeBtnStyle("#2f8f5f", "#3aa06f"));
    m_runSelectedBtn->setCursor(Qt::PointingHandCursor);
    m_runSelectedBtn->setFixedSize(40, 32);
    connect(m_runSelectedBtn, &QPushButton::clicked, this, &ProcessPage::OnRunSelectedAction);

    actionBtnRow->addWidget(newActionBtn);
    actionBtnRow->addWidget(editActionBtn);
    actionBtnRow->addWidget(deleteActionBtn);
    actionBtnRow->addWidget(upActionBtn);
    actionBtnRow->addWidget(downActionBtn);
    actionBtnRow->addWidget(m_runSelectedBtn);
    actionBtnRow->addStretch();

    leftLayout->addWidget(actionHeader);
    leftLayout->addWidget(m_actionList, 1);
    leftLayout->addLayout(actionBtnRow);

    splitLayout->addWidget(leftPanel, 3);

    // Right stack
    auto* rightPanel = new QWidget();
    rightPanel->setStyleSheet("background: #1b222b; border-radius: 12px; border: 1px solid #384550;");
    rightPanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(10, 10, 10, 10);
    rightLayout->setSpacing(8);

    m_currentActionLabel = new QLabel(QStringLiteral("当前动作: (无)"));
    m_currentActionLabel->setStyleSheet("color: #b8cce3; font-size: 15px; font-weight: 500; background: transparent; border: none;");

    m_detailStack = new QStackedWidget();
    m_detailStack->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_detailStack->setStyleSheet("background: transparent;");

    // Page 0: placeholder
    auto* placeholderPage = new QWidget();
    auto* placeholderLayout = new QVBoxLayout(placeholderPage);
    auto* placeholderLabel = new QLabel(QStringLiteral("请先选择或创建一个动作"));
    placeholderLabel->setAlignment(Qt::AlignCenter);
    placeholderLabel->setStyleSheet("color: #5a7a8a; font-size: 15px; background: transparent; border: none;");
    placeholderLayout->addWidget(placeholderLabel);
    m_detailStack->addWidget(placeholderPage);

    // Page 1: Move
    auto* movePage = new QWidget();
    auto* moveLayout = new QVBoxLayout(movePage);
    moveLayout->setContentsMargins(0, 0, 0, 0);
    moveLayout->setSpacing(8);

    m_pointTable = new QTableWidget(0, 6);
    m_pointTable->setHorizontalHeaderLabels({
        QStringLiteral("点名称"), QStringLiteral("X"), QStringLiteral("Y"),
        QStringLiteral("Z"), QStringLiteral("R"), QStringLiteral("姿态")
    });
    m_pointTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pointTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pointTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_pointTable->verticalHeader()->setVisible(false);
    for (int c = 0; c < 6; ++c)
        m_pointTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Stretch);
    m_pointTable->setStyleSheet(R"(
        QTableWidget { background: #192029; border: 1px solid #2f3d4d; border-radius: 8px; }
        QTableWidget::item { padding: 4px 6px; color: #cddef0; border-top: 1px solid #2b3542; }
        QTableWidget::item:selected { background: #2f6f9f; color: #ffffff; }
        QTableWidget::item:selected:active { background: #2f6f9f; color: #ffffff; }
        QTableWidget QLineEdit { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; selection-background-color: #2f6f9f; }
        QHeaderView::section { background: #242e3a; color: #b0c7df; padding: 6px; border: none; }
    )");
    moveLayout->addWidget(m_pointTable, 1);

    auto* pointBtnRow = new QHBoxLayout();
    pointBtnRow->setSpacing(8);

    struct PointBtn { QString text; QString bg; QString hover; void (ProcessPage::*slot)(); };
    QVector<PointBtn> pointBtns = {
        { QStringLiteral("添加点位"),     "#2f6f8f", "#3a84a8", &ProcessPage::OnAddPoint },
        { QStringLiteral("删除选中点"),   "#8f4f4f", "#aa5f5f", &ProcessPage::OnDeletePoint },
        { QStringLiteral("上移"),         "#4f4f6f", "#5f5f8f", &ProcessPage::OnMoveUp },
        { QStringLiteral("下移"),         "#4f4f6f", "#5f5f8f", &ProcessPage::OnMoveDown },
        { QStringLiteral("读取当前坐标(示教)"), "#7f7f3f", "#9f9f4f", &ProcessPage::OnTeachRead },
    };
    for (const auto& pb : pointBtns) {
        auto* btn = new QPushButton(pb.text);
        btn->setStyleSheet(makeBtnStyle(pb.bg, pb.hover));
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QPushButton::clicked, this, pb.slot);
        pointBtnRow->addWidget(btn);
    }
    pointBtnRow->addStretch();

    auto* moveSaveBtn = new QPushButton(QStringLiteral("保存动作"));
    moveSaveBtn->setStyleSheet(saveBtnStyle);
    moveSaveBtn->setCursor(Qt::PointingHandCursor);
    connect(moveSaveBtn, &QPushButton::clicked, this, &ProcessPage::OnSaveAction);
    pointBtnRow->addWidget(moveSaveBtn);
    moveLayout->addLayout(pointBtnRow);
    m_detailStack->addWidget(movePage);

    // Page 2: Vision
    auto* visionPage = new QWidget();
    auto* vf = makeFormPage(visionPage);
    m_visionTypeCombo = new QComboBox();
    m_visionTypeCombo->addItems({ QStringLiteral("CCD"), QStringLiteral("深度"), QStringLiteral("激光") });
    m_visionTypeCombo->setFixedHeight(30);
    m_visionTypeCombo->setStyleSheet(R"(
        QComboBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; padding: 4px 8px; font-size: 13px; }
        QComboBox::drop-down { width: 20px; border: none; }
        QComboBox:hover { border: 1px solid #4f7faf; }
        QAbstractItemView { background: #1a222b; outline: none; selection-background-color: #2f6f9f; selection-color: white; color: #dbe6f0; }
    )");
    vf->addRow(makeLabel(QStringLiteral("识别类型")), m_visionTypeCombo);
    m_exposureEdit = makeFieldEdit();
    vf->addRow(makeLabel(QStringLiteral("曝光时间")), m_exposureEdit);
    m_templateEdit = makeFieldEdit();
    vf->addRow(makeLabel(QStringLiteral("匹配模板")), m_templateEdit);
    m_thresholdEdit = makeFieldEdit();
    vf->addRow(makeLabel(QStringLiteral("置信度阈值")), m_thresholdEdit);
    auto* visionSaveLayout = new QHBoxLayout();
    visionSaveLayout->addStretch();
    auto* visionSaveBtn = new QPushButton(QStringLiteral("保存动作"));
    visionSaveBtn->setStyleSheet(saveBtnStyle);
    visionSaveBtn->setCursor(Qt::PointingHandCursor);
    connect(visionSaveBtn, &QPushButton::clicked, this, &ProcessPage::OnSaveAction);
    visionSaveLayout->addWidget(visionSaveBtn);
    vf->addRow(visionSaveLayout);
    m_detailStack->addWidget(visionPage);

    // Page 3: Extrude
    auto* extrudePage = new QWidget();
    auto* ef = makeFormPage(extrudePage);
    m_extrudeAmountEdit = makeFieldEdit();
    ef->addRow(makeLabel(QStringLiteral("挤出量")), m_extrudeAmountEdit);
    m_extrudeSpeedEdit = makeFieldEdit();
    ef->addRow(makeLabel(QStringLiteral("挤出速度")), m_extrudeSpeedEdit);
    m_suckBackAmountEdit = makeFieldEdit();
    ef->addRow(makeLabel(QStringLiteral("回抽量")), m_suckBackAmountEdit);
    m_suckBackSpeedEdit = makeFieldEdit();
    ef->addRow(makeLabel(QStringLiteral("回抽速度")), m_suckBackSpeedEdit);
    auto* extrudeSaveLayout = new QHBoxLayout();
    extrudeSaveLayout->addStretch();
    auto* extrudeSaveBtn = new QPushButton(QStringLiteral("保存动作"));
    extrudeSaveBtn->setStyleSheet(saveBtnStyle);
    extrudeSaveBtn->setCursor(Qt::PointingHandCursor);
    connect(extrudeSaveBtn, &QPushButton::clicked, this, &ProcessPage::OnSaveAction);
    extrudeSaveLayout->addWidget(extrudeSaveBtn);
    ef->addRow(extrudeSaveLayout);
    m_detailStack->addWidget(extrudePage);

    // Page 4: Delay
    auto* delayPage = new QWidget();
    auto* df = makeFormPage(delayPage);
    m_delaySpin = new QSpinBox();
    m_delaySpin->setRange(0, 999999);
    m_delaySpin->setFixedHeight(30);
    m_delaySpin->setFixedWidth(120);
    m_delaySpin->setSuffix(QStringLiteral(" ms"));
    m_delaySpin->setStyleSheet("QSpinBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; font-size: 13px; padding: 4px 8px; }");
    df->addRow(makeLabel(QStringLiteral("延时时间")), m_delaySpin);
    auto* delaySaveLayout = new QHBoxLayout();
    delaySaveLayout->addStretch();
    auto* delaySaveBtn = new QPushButton(QStringLiteral("保存动作"));
    delaySaveBtn->setStyleSheet(saveBtnStyle);
    delaySaveBtn->setCursor(Qt::PointingHandCursor);
    connect(delaySaveBtn, &QPushButton::clicked, this, &ProcessPage::OnSaveAction);
    delaySaveLayout->addWidget(delaySaveBtn);
    df->addRow(delaySaveLayout);
    m_detailStack->addWidget(delayPage);

    // Page 5: Gripper
    auto* gripperPage = new QWidget();
    auto* gf = makeFormPage(gripperPage);
    m_gripperCombo = new QComboBox();
    m_gripperCombo->addItems({ QStringLiteral("闭合"), QStringLiteral("张开") });
    m_gripperCombo->setFixedHeight(30);
    m_gripperCombo->setStyleSheet(R"(
        QComboBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; padding: 4px 8px; font-size: 13px; }
        QComboBox::drop-down { width: 20px; border: none; }
        QComboBox:hover { border: 1px solid #4f7faf; }
        QAbstractItemView { background: #1a222b; outline: none; selection-background-color: #2f6f9f; selection-color: white; color: #dbe6f0; }
    )");
    // 行程输入框 = 轴5 绝对目标坐标（mm），与手动控制页轴5 Go 语义一致：
    // 0 = 夹紧、负值 = 松开，填什么值执行什么值（执行层仍按轴5 软限位硬性拦截越界）。
    // 注意：QDoubleSpinBox 样式表禁止写 font-size（Qt6 polish 崩溃），字号用 setFont()。
    m_gripperTargetSpin = new QDoubleSpinBox();
    m_gripperTargetSpin->setRange(-5.0, 0.0);
    m_gripperTargetSpin->setDecimals(2);
    m_gripperTargetSpin->setSingleStep(0.5);
    m_gripperTargetSpin->setFixedHeight(30);
    m_gripperTargetSpin->setFixedWidth(110);
    m_gripperTargetSpin->setAlignment(Qt::AlignRight);
    {
        QFont f = m_gripperTargetSpin->font();
        f.setPointSizeF(9.75);
        m_gripperTargetSpin->setFont(f);
    }
    m_gripperTargetSpin->setStyleSheet("QDoubleSpinBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; padding: 4px 8px; }");
    m_gripperUnitLabel = new QLabel(QStringLiteral("mm"));
    m_gripperUnitLabel->setStyleSheet("color: #8da3bb; font-size: 13px; font-weight: 500; background: transparent; border: none;");
    m_gripperUnitLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto* gripperField = new QWidget();
    auto* gripperFieldLay = new QHBoxLayout(gripperField);
    gripperFieldLay->setContentsMargins(0, 0, 0, 0);
    gripperFieldLay->setSpacing(6);
    gripperFieldLay->addWidget(m_gripperCombo);
    gripperFieldLay->addWidget(m_gripperTargetSpin);
    gripperFieldLay->addWidget(m_gripperUnitLabel);
    gripperFieldLay->addStretch();
    gf->addRow(makeLabel(QStringLiteral("夹爪动作/行程")), gripperField);
    m_gripperHintLabel = new QLabel();
    m_gripperHintLabel->setStyleSheet("color: #6a7a8a; font-size: 12px; background: transparent; border: none;");
    gf->addRow(m_gripperHintLabel);
    RefreshGripperLimitRange();
    auto* gripperSaveLayout = new QHBoxLayout();
    gripperSaveLayout->addStretch();
    auto* gripperSaveBtn = new QPushButton(QStringLiteral("保存动作"));
    gripperSaveBtn->setStyleSheet(saveBtnStyle);
    gripperSaveBtn->setCursor(Qt::PointingHandCursor);
    connect(gripperSaveBtn, &QPushButton::clicked, this, &ProcessPage::OnSaveAction);
    gripperSaveLayout->addWidget(gripperSaveBtn);
    gf->addRow(gripperSaveLayout);
    m_detailStack->addWidget(gripperPage);

    m_detailStack->setCurrentIndex(0);

    rightLayout->addWidget(m_currentActionLabel);

    // 动作运行速度百分比（仅移动/挤压/夹爪动作显示）
    m_speedPercentRow = new QWidget();
    m_speedPercentRow->setFixedHeight(40);
    auto* speedRowLayout = new QHBoxLayout(m_speedPercentRow);
    speedRowLayout->setContentsMargins(8, 0, 0, 0);
    speedRowLayout->setSpacing(10);

    auto* speedLabel = new QLabel(QStringLiteral("动作运行速度:"));
    speedLabel->setStyleSheet("color: #b8cce3; font-size: 13px; font-weight: 500; background: transparent; border: none;");

    m_speedPercentSpin = new QSpinBox();
    m_speedPercentSpin->setRange(1, 100);
    m_speedPercentSpin->setValue(100);
    m_speedPercentSpin->setSuffix(QStringLiteral(" %"));
    m_speedPercentSpin->setFixedHeight(30);
    m_speedPercentSpin->setFixedWidth(110);
    m_speedPercentSpin->setStyleSheet("QSpinBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; font-size: 13px; padding: 4px 8px; }");

    m_speedPercentSlider = new QSlider(Qt::Horizontal);
    m_speedPercentSlider->setRange(1, 100);
    m_speedPercentSlider->setValue(100);
    m_speedPercentSlider->setFixedHeight(30);
    m_speedPercentSlider->setFixedWidth(240);
    m_speedPercentSlider->setStyleSheet(
        "QSlider { background: transparent; border: none; }"
        "QSlider:focus { border: none; outline: none; }"
        "QSlider::groove:horizontal { background: #2a3340; height: 6px; border-radius: 3px; }"
        "QSlider::sub-page:horizontal { background: #2f7fb5; border-radius: 3px; }"
        "QSlider::handle:horizontal { background: #cfe0f5; width: 16px; margin: -5px 0; border-radius: 8px; }");

    auto* speedHint = new QLabel(QStringLiteral("(1-100)"));
    speedHint->setStyleSheet("color: #6a7a8a; font-size: 12px; background: transparent; border: none;");

    speedRowLayout->addWidget(speedLabel);
    speedRowLayout->addWidget(m_speedPercentSpin);
    speedRowLayout->addWidget(m_speedPercentSlider);
    speedRowLayout->addWidget(speedHint);
    speedRowLayout->addStretch();
    m_speedPercentRow->setVisible(false);

    // 输入框 ↔ 滑动条双向同步（各自 blockSignals 防回环）；
    // 同时实时写回当前编辑动作的 speedPercent——拖动即生效（执行/单步直接读内存值），
    // 不必先点「保存动作」（正式落盘仍由保存按钮负责，两处写同一字段无冲突）。
    // 曾只有 OnSaveAction 才写 action.speedPercent，改滑条后直接执行仍用旧值（2026-08-31 用户反馈"速度没变化"）。
    connect(m_speedPercentSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int v) {
        m_speedPercentSlider->blockSignals(true);
        m_speedPercentSlider->setValue(v);
        m_speedPercentSlider->blockSignals(false);
        ApplySpeedPercentToCurrentAction(v);
    });
    connect(m_speedPercentSlider, &QSlider::valueChanged,
            this, [this](int v) {
        m_speedPercentSpin->blockSignals(true);
        m_speedPercentSpin->setValue(v);
        m_speedPercentSpin->blockSignals(false);
        ApplySpeedPercentToCurrentAction(v);
    });

    // 夹爪行程实时写回当前动作（改即生效，不必先点「保存动作」；落盘仍由保存按钮负责）
    connect(m_gripperTargetSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &ProcessPage::ApplyGripperTargetToCurrentAction);
    // 切换张开/闭合：把当前值记入旧方向，再恢复另一方向的最近值（首次为该方向默认：闭合 0.00 / 张开 -3.00）
    connect(m_gripperCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) {
        const bool nowOpen = (idx == 1);
        const double cur = m_gripperTargetSpin->value();
        if (nowOpen) m_lastCloseTarget = cur;
        else         m_lastOpenTarget = cur;
        m_gripperTargetSpin->setValue(nowOpen ? m_lastOpenTarget : m_lastCloseTarget);
    });

    rightLayout->addWidget(m_speedPercentRow);
    rightLayout->addWidget(m_detailStack, 1);

    splitLayout->addWidget(rightPanel, 7);
    mainLayout->addWidget(splitWidget, 1);
}

void ProcessPage::RefreshSchemeCombo()
{
    m_schemeCombo->blockSignals(true);
    m_schemeCombo->clear();
    const auto& s = ProcessManager::instance().schemes();
    for (const auto& scheme : s)
        m_schemeCombo->addItem(scheme.schemeName);
    if (m_currentSchemeIdx >= 0 && m_currentSchemeIdx < s.size())
        m_schemeCombo->setCurrentIndex(m_currentSchemeIdx);
    m_schemeCombo->blockSignals(false);
}

void ProcessPage::RefreshActionList()
{
    m_actionList->blockSignals(true);
    m_actionList->clear();

    if (m_currentSchemeIdx >= 0 && m_currentSchemeIdx < ProcessManager::instance().schemes().size()) {
        const auto& actions = ProcessManager::instance().schemes()[m_currentSchemeIdx].actions;
        for (int i = 0; i < actions.size(); ++i) {
            const auto& a = actions[i];
            QString text = QStringLiteral("[%1] (%2) %3")
                               .arg(i + 1).arg(ProcessManager::actionTypeName(a.type)).arg(a.name);
            m_actionList->addItem(text);
        }
    }

    if (m_currentActionIdx >= 0 && m_currentActionIdx < m_actionList->count())
        m_actionList->setCurrentRow(m_currentActionIdx);
    else if (m_actionList->count() > 0)
        m_actionList->setCurrentRow(0);
    else
        m_currentActionIdx = -1;

    m_actionList->blockSignals(false);

    int row = m_actionList->currentRow();
    if (row >= 0) {
        m_currentActionIdx = row;
        RefreshActionDetail(row);
    } else {
        m_currentActionIdx = -1;
        m_detailStack->setCurrentIndex(0);
        m_currentActionLabel->setText(QStringLiteral("当前动作: (无)"));
    }
}

void ProcessPage::RefreshActionDetail(int idx)
{
    const auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx < 0 || m_currentSchemeIdx >= schemes.size() ||
        idx < 0 || idx >= schemes[m_currentSchemeIdx].actions.size())
    {
        m_currentActionLabel->setText(QStringLiteral("当前动作: (无)"));
        m_detailStack->setCurrentIndex(0);
        return;
    }

    const auto& action = schemes[m_currentSchemeIdx].actions[idx];
    m_currentActionLabel->setText(QStringLiteral("当前动作: [%1] %2").arg(idx + 1).arg(action.name));
    m_currentActionIdx = idx;

    int stackIdx = static_cast<int>(action.type) + 1;
    m_detailStack->setCurrentIndex(stackIdx);

    bool hasSpeedPercent = (action.type == ActionType::Move ||
                            action.type == ActionType::Extrude ||
                            action.type == ActionType::Gripper);
    m_speedPercentRow->setVisible(hasSpeedPercent);
    if (hasSpeedPercent) {
        m_speedPercentSpin->setValue(action.speedPercent);
        m_speedPercentSlider->setValue(action.speedPercent);
    }

    auto makePoseCombo = [](const QString& text) -> QComboBox* {
        auto* c = new QComboBox();
        c->addItems({ QStringLiteral("elbow_up"), QStringLiteral("elbow_down") });
        c->setCurrentText(text);
        c->setFixedHeight(24);
        c->setStyleSheet(R"(
            QComboBox { background: #192029; border: 1px solid #3f4e5e; border-radius: 4px;
                        color: #cddef0; padding: 0 4px; font-size: 12px; }
            QComboBox::drop-down { width: 16px; border: none; }
            QComboBox::down-arrow { image: none; border-left: 4px solid transparent;
                                    border-right: 4px solid transparent;
                                    border-top: 5px solid #8da3bb; margin-right: 2px; }
            QComboBox:hover { border: 1px solid #4f7faf; }
            QAbstractItemView { background: #1a222b; outline: none;
                                selection-background-color: #2f6f9f; selection-color: white;
                                color: #dbe6f0; }
        )");
        c->setCursor(Qt::PointingHandCursor);
        return c;
    };

    switch (action.type) {
    case ActionType::Move:
        while (m_pointTable->rowCount() > 0)
            m_pointTable->removeRow(0);
        for (int r = 0; r < action.points.size(); ++r) {
            const auto& pt = action.points[r];
            m_pointTable->insertRow(r);
            m_pointTable->setItem(r, 0, new QTableWidgetItem(pt.name));
            m_pointTable->setItem(r, 1, new QTableWidgetItem(QString::number(pt.x, 'f', 2)));
            m_pointTable->setItem(r, 2, new QTableWidgetItem(QString::number(pt.y, 'f', 2)));
            m_pointTable->setItem(r, 3, new QTableWidgetItem(QString::number(pt.z, 'f', 2)));
            m_pointTable->setItem(r, 4, new QTableWidgetItem(QString::number(pt.r, 'f', 2)));
            m_pointTable->setCellWidget(r, 5, makePoseCombo(pt.posture));
            m_pointTable->setRowHeight(r, 40);
        }
        break;
    case ActionType::Vision:
        m_visionTypeCombo->setCurrentText(action.visionType);
        m_exposureEdit->setText(QString::number(action.exposure));
        m_templateEdit->setText(action.templateName);
        m_thresholdEdit->setText(QString::number(action.threshold));
        break;
    case ActionType::Extrude:
        m_extrudeAmountEdit->setText(QString::number(action.extrudeAmount));
        m_extrudeSpeedEdit->setText(QString::number(action.extrudeSpeed));
        m_suckBackAmountEdit->setText(QString::number(action.suckBackAmount));
        m_suckBackSpeedEdit->setText(QString::number(action.suckBackSpeed));
        break;
    case ActionType::Delay:
        m_delaySpin->setValue(action.delayMs);
        break;
    case ActionType::Gripper:
        RefreshGripperLimitRange();
        m_gripperCombo->blockSignals(true);
        m_gripperCombo->setCurrentIndex(action.isGripperOpen ? 1 : 0);
        m_gripperCombo->blockSignals(false);
        m_gripperTargetSpin->setValue(action.gripperTarget);
        if (action.isGripperOpen) m_lastOpenTarget = action.gripperTarget;
        else                      m_lastCloseTarget = action.gripperTarget;
        break;
    }
}

void ProcessPage::OnNewScheme()
{
    SPDLOG_INFO("[Process] 新增方案 clicked");
    auto& schemes = ProcessManager::instance().schemes();
    QString name = ProcessManager::generateUniqueSchemeName(schemes);
    SchemeData sd;
    sd.schemeName = name;
    schemes.push_back(sd);
    m_currentSchemeIdx = schemes.size() - 1;
    RefreshSchemeCombo();
    RefreshActionList();
    ProcessManager::instance().save();
}

void ProcessPage::OnDeleteScheme()
{
    SPDLOG_INFO("[Process] 删除方案 clicked");
    const auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx < 0 || schemes.isEmpty()) return;

    // 单步执行进行中禁止删除方案：worker 跑的是启动时值拷贝的 scheme，
    // 删除后继续点「单步执行」会按旧拷贝继续推进，与界面的方案列表不一致
    if (IsExecutionActive()) {
        SPDLOG_WARN("[Process] 删除方案被拒：单步执行进行中 idx={}", m_currentSchemeIdx);
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("单步执行尚未结束，请先点击「停止」结束当前调试，再删除方案"));
        return;
    }

    auto reply = QMessageBox::question(this,
        QStringLiteral("删除方案"),
        QStringLiteral("确定要删除方案 \"%1\" 吗？\n此操作不可撤销。").arg(schemes[m_currentSchemeIdx].schemeName),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

    auto& s = ProcessManager::instance().schemes();
    s.removeAt(m_currentSchemeIdx);

    if (s.isEmpty()) {
        m_currentSchemeIdx = -1;
        m_currentActionIdx = -1;
        m_schemeNameEdit->clear();
    } else {
        m_currentSchemeIdx = 0;
        m_currentActionIdx = -1;
    }
    RefreshSchemeCombo();
    RefreshActionList();
    ProcessManager::instance().save();
}

void ProcessPage::OnConfirmSwitch()
{
    int idx = m_schemeCombo->currentIndex();
    SPDLOG_INFO("[Process] 确认切换方案 clicked idx={}", idx);
    const auto& schemes = ProcessManager::instance().schemes();
    if (idx < 0 || idx >= schemes.size()) return;
    if (idx == m_currentSchemeIdx) return;

    // 单步执行进行中禁止切换方案：SequenceWorker 跑的是启动时值拷贝的 scheme，
    // 切换后继续点「单步执行」会按旧拷贝继续推进，与界面显示的方案不一致
    if (IsExecutionActive()) {
        SPDLOG_WARN("[Process] 切换方案被拒：单步执行进行中 idx={}", idx);
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("单步执行尚未结束，请先点击「停止」结束当前调试，再切换方案"));
        return;
    }

    if (m_currentSchemeIdx >= 0) {
        auto reply = QMessageBox::question(this,
            QStringLiteral("确认切换"),
            QStringLiteral("确定从 \"%1\" 切换到 \"%2\" 吗？")
                .arg(schemes[m_currentSchemeIdx].schemeName).arg(schemes[idx].schemeName),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
    }

    m_currentSchemeIdx = idx;
    m_currentActionIdx = -1;
    m_schemeNameEdit->setText(schemes[idx].schemeName);
    RefreshActionList();
}

void ProcessPage::SetSequenceWorker(SequenceWorker* worker)
{
    m_worker = worker;
    if (!m_worker) return;
    connect(m_worker, &SequenceWorker::logMessage, this, [this](const QString& msg) {
        SPDLOG_INFO("[Process] worker: {}", msg.toStdString());
    });

    // 执行状态标签（顶栏第二行，单步执行的唯一界面反馈）
    auto setFinalStatus = [this](const QString& text, const QString& color) {
        m_finalStatus = text;
        m_finalColor  = color;
        SetStatusText(text, color);
    };
    // 单步按钮只在「单步暂停」期可点，动作执行期间一律禁用（D2 修复）：
    // stepGo 是 bool 非计数，执行期点击会预置放行，使下一个暂停点被跳过、两个动作连着跑，
    // 单步调试本该有的逐步确认机会随之丢失。禁用后每次点击必然落在暂停期，语义收敛为
    // 「走一步 → 停 → 确认 → 再走一步」。
    auto setStepEnabled = [this](bool on) {
        if (m_stepBtn) m_stepBtn->setEnabled(on && HardwareManager::instance().IsSystemHomed());
    };
    connect(m_worker, &SequenceWorker::stateChanged, this, [this, setStepEnabled](const QString& s) {
        SPDLOG_INFO("[Process] worker state: {}", s.toStdString());
        if (s == QStringLiteral("单步暂停")) {
            SetStatusText(QStringLiteral("● 单步暂停"), QStringLiteral("#e0a520"));
            setStepEnabled(true);   // 暂停期：允许释放下一步
        }
        else if (s == QStringLiteral("运行中")) {
            SetStatusText(QStringLiteral("● 运行中"), QStringLiteral("#7ed67e"));
            setStepEnabled(false);  // 动作执行中：禁用，杜绝提前放行
        }
        else if (s == QStringLiteral("执行选中动作")) {
            SetStatusText(QStringLiteral("● 执行选中动作"), QStringLiteral("#4fb0d8"));
            setStepEnabled(false);
        }
        else if (s == QStringLiteral("空闲")) {
            // 会话结束：优先保留终态（完成/已停止/出错），否则回到空闲
            SetStatusText(m_finalStatus.isEmpty() ? QStringLiteral("● 空闲") : m_finalStatus,
                          m_finalColor.isEmpty() ? QStringLiteral("#b8cce3") : m_finalColor);
            setStepEnabled(true);   // 会话结束，使能交回 homed 联动
        }
        else
            SetStatusText(QStringLiteral("● ") + s, QStringLiteral("#b8cce3"));
    });
    connect(m_worker, &SequenceWorker::schemeFinished, this, [setFinalStatus]() {
        setFinalStatus(QStringLiteral("✅ 完成"), QStringLiteral("#7ed67e"));
    });
    connect(m_worker, &SequenceWorker::interrupted, this, [this, setFinalStatus]() {
        // 急停与业务停止都发 interrupted，靠使能状态区分：急停断使能、业务停止保持使能
        // （HardwareManager::EmergencyStop 先 fill(false) 再 emit，此处读到的一定是终态）
        if (HardwareManager::instance().IsGlobalEnabled())
            setFinalStatus(QStringLiteral("■ 已停止（保持使能）"), QStringLiteral("#b8cce3"));
        else
            setFinalStatus(QStringLiteral("⏹ 已急停（需重新使能）"), QStringLiteral("#d14444"));
    });
    connect(m_worker, &SequenceWorker::errorOccurred, this,
            [this, setFinalStatus](const QString& msg) {
        SPDLOG_WARN("[Process] 执行出错: {}", msg.toStdString());
        setFinalStatus(QStringLiteral("✖ 出错"), QStringLiteral("#d14444"));
    });
    connect(m_worker, &SequenceWorker::actionStarted, this, [this, setStepEnabled](int i, const QString& name) {
        SPDLOG_INFO("[Process] 单步 actionStarted {}/{}", i, name.toStdString());
        // 自动选中当前正在执行的动作（单步/自动/单动作共用）：setCurrentRow 经
        // currentRowChanged 同步 m_currentActionIdx 并刷新右侧详情，执行进度在动作列表可见
        if (i >= 0 && i < m_actionList->count())
            m_actionList->setCurrentRow(i);
        setStepEnabled(false);  // 动作已开始执行，禁用单步按钮
    });
    connect(m_worker, &SequenceWorker::actionFinished, this, [this](int i, const QString& name) {
        SPDLOG_INFO("[Process] 单步 actionFinished {}/{}", i, name.toStdString());
    });
    connect(m_worker, &SequenceWorker::schemeFinished, this, &ProcessPage::ResetStepSession);
    connect(m_worker, &SequenceWorker::interrupted, this, &ProcessPage::ResetStepSession);
    connect(m_worker, &SequenceWorker::errorOccurred, this, &ProcessPage::ResetStepSession);
    // 顶栏全局急停也清单步会话（断使能 + worker 中断后需重置状态）
    connect(&HardwareManager::instance(), &HardwareManager::emergencyStopTriggered,
            this, &ProcessPage::ResetStepSession);
    // 回零互锁联动：未回零禁用单步/执行选中（开环步进断电丢坐标），全轴回零后激活。
    // 注：执行选中按钮执行中禁用由 singleActionFinished/interrupted/errorOccurred 恢复，
    // 此处仅在 homed 状态变化时设置，不与执行中禁用冲突（执行中 homed 恒 true）。
    const bool homed = HardwareManager::instance().IsSystemHomed();
    if (m_stepBtn) m_stepBtn->setEnabled(homed);
    if (m_runSelectedBtn) m_runSelectedBtn->setEnabled(homed);
    connect(&HardwareManager::instance(), &HardwareManager::homeStateChanged,
            this, [this](bool h) {
        if (m_stepBtn) m_stepBtn->setEnabled(h);
        if (m_runSelectedBtn) m_runSelectedBtn->setEnabled(h);
    });
    // 「执行选中动作」按钮复位：单动作结束/中断/出错三路径均恢复（含停止按钮 → interrupted 路径）
    connect(m_worker, &SequenceWorker::singleActionFinished, this, [this] {
        if (m_runSelectedBtn) m_runSelectedBtn->setEnabled(true);
    });
    connect(m_worker, &SequenceWorker::interrupted, this, [this] {
        if (m_runSelectedBtn) m_runSelectedBtn->setEnabled(true);
    });
    connect(m_worker, &SequenceWorker::errorOccurred, this, [this] {
        if (m_runSelectedBtn) m_runSelectedBtn->setEnabled(true);
    });
}

void ProcessPage::ResetStepSession()
{
    m_stepActive = false;
}

void ProcessPage::SetStatusText(const QString& text, const QString& color)
{
    if (!m_statusLabel) return;
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(
        QStringLiteral("color: %1; font-size: 13px; font-weight: 600; background: transparent; border: none;")
            .arg(color));
}

bool ProcessPage::IsExecutionActive() const
{
    return m_stepActive || (m_worker && m_worker->IsRunning());
}

void ProcessPage::ApplySpeedPercentToCurrentAction(int v)
{
    if (m_currentSchemeIdx < 0 || m_currentActionIdx < 0) return;
    auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx >= schemes.size()) return;
    auto& actions = schemes[m_currentSchemeIdx].actions;
    if (m_currentActionIdx >= actions.size()) return;
    actions[m_currentActionIdx].speedPercent = v;
}

void ProcessPage::ApplyGripperTargetToCurrentAction(double v)
{
    if (m_currentSchemeIdx < 0 || m_currentActionIdx < 0) return;
    auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx >= schemes.size()) return;
    auto& actions = schemes[m_currentSchemeIdx].actions;
    if (m_currentActionIdx >= actions.size()) return;
    if (actions[m_currentActionIdx].type != ActionType::Gripper) return;
    actions[m_currentActionIdx].gripperTarget = v;
}

void ProcessPage::RefreshGripperLimitRange()
{
    // 轴5（LogicalAxis::Gripper）软限位实时读 config，与手动控制页 Go 的校验同源。
    // GetLimitMin/Max 在 config 异常（lo>=hi）时返回无效区间 → 放宽 UI 限位，
    // 真正的拦截由执行层 IsWithinSoftLimits 兜底（拒绝而非夹紧）。
    auto& hw = HardwareManager::instance();
    const double lo = hw.GetLimitMin(LogicalAxis::Gripper);
    const double hi = hw.GetLimitMax(LogicalAxis::Gripper);
    if (lo < hi) {
        m_gripperTargetSpin->setRange(lo, hi);
        m_gripperHintLabel->setStyleSheet("color: #6a7a8a; font-size: 12px; background: transparent; border: none;");
        m_gripperHintLabel->setText(QStringLiteral("轴5 软限位 %1 ~ %2 mm（0 = 夹紧，负值 = 松开）")
                                        .arg(QString::number(lo, 'f', 2), QString::number(hi, 'f', 2)));
    } else {
        m_gripperTargetSpin->setRange(-999.0, 999.0);
        m_gripperHintLabel->setStyleSheet("color: #e0a520; font-size: 12px; background: transparent; border: none;");
        m_gripperHintLabel->setText(QStringLiteral("轴5 软限位未配置（%1 ~ %2），行程不受界面限制，执行时将被拒绝")
                                        .arg(QString::number(lo, 'f', 2), QString::number(hi, 'f', 2)));
    }
}

void ProcessPage::OnStepExecute()
{
    if (!m_worker) {
        SPDLOG_WARN("[Process] 单步执行：引擎未初始化");
        return;
    }
    if (!HardwareManager::instance().IsGlobalEnabled()) {
        SPDLOG_WARN("[Process] 单步执行：未使能，拒绝（请先手动使能所有轴）");
        return;
    }
    // 回零互锁（双保险，按钮联动之外兜底）：未回零禁止单步自动运动
    if (!HardwareManager::instance().IsSystemHomed()) {
        SPDLOG_WARN("[Process] 单步执行：系统未回零，拒绝（请先一键回零）");
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("系统未回零，请先执行一键回零"));
        return;
    }
    if (m_currentSchemeIdx < 0) {
        SPDLOG_WARN("[Process] 单步执行：未选择方案");
        return;
    }
    const auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx >= schemes.size()) return;
    const auto& scheme = schemes[m_currentSchemeIdx];

    if (!m_stepActive) {
        // 新会话开始：清空上一次的终态，避免「✅ 完成」残留到本次运行
        m_finalStatus.clear();
        m_finalColor.clear();
        m_worker->ReloadFromConfig();
        m_worker->SetStepMode(true);
        if (!m_worker->RunSequence(scheme)) {
            // 拒绝（运行中/引擎忙）：不进入单步会话，按钮维持当前状态，避免 m_stepActive 卡死
            // （D1：原实现不检查返回值，被拒后 m_stepActive 仍置 true → 此后所有点击都走 NextStep，
            //   会话永久卡死，只能靠停止/急停复位）
            SPDLOG_WARN("[Process] 单步执行启动被拒：引擎忙或不可用（会话未激活）");
            return;
        }
        m_stepActive = true;
        // 同步禁用（不等 worker 的 stateChanged 跨线程回传）：释放后的动作即将启动，
        // 若等到信号到达才禁用，快速连点的第 2 下会穿透窗口期 → 跳过暂停点连跑两个动作（D2 竞态）
        if (m_stepBtn) m_stepBtn->setEnabled(false);
        SPDLOG_INFO("[Process] 单步执行启动：方案={}", scheme.schemeName.toStdString());
    } else {
        if (!m_worker->NextStep()) {
            // NextStep 仅在 running/stepMode 都成立时有效；无效则按钮不应灰
            SPDLOG_WARN("[Process] 单步执行：释放失败（引擎不在单步运行态）");
            return;
        }
        // 同步禁用，同上：下一次「单步暂停」信号到达时再由 stateChanged 处理器重新点亮
        if (m_stepBtn) m_stepBtn->setEnabled(false);
        SPDLOG_INFO("[Process] 单步执行：释放下一步");
    }
}

void ProcessPage::OnRunSelectedAction()
{
    if (!m_worker) {
        SPDLOG_WARN("[Process] 执行选中动作：引擎未初始化");
        return;
    }
    // 回零互锁（双保险，按钮联动之外兜底）：未回零禁止单动作自动运动
    if (!HardwareManager::instance().IsSystemHomed()) {
        SPDLOG_WARN("[Process] 执行选中动作：系统未回零，拒绝（请先一键回零）");
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("系统未回零，请先执行一键回零"));
        return;
    }
    if (m_currentSchemeIdx < 0) {
        SPDLOG_WARN("[Process] 执行选中动作：未选择方案");
        return;
    }
    const auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx >= schemes.size()) return;
    const auto& scheme = schemes[m_currentSchemeIdx];

    int row = m_actionList->currentRow();
    if (row < 0) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先选择一个动作"));
        return;
    }
    if (row >= scheme.actions.size()) return;

    // 若正处于单步会话，先清单步状态，避免与单动作执行混用（RunSingleAction 内部另清 stepMode）
    if (m_stepActive) ResetStepSession();
    // 每次执行前刷新运动学/手眼（与 OnStepExecute 一致，保证与 ConfigPage 编辑同步）
    m_worker->ReloadFromConfig();
    m_finalStatus.clear();
    m_finalColor.clear();
    bool ok = m_worker->RunSingleAction(scheme, row);
    if (!ok) {
        // 拒绝（未使能/运行中/越界）不发完成信号，立即恢复按钮，避免永久禁用
        m_runSelectedBtn->setEnabled(true);
        return;
    }
    m_runSelectedBtn->setEnabled(false);
    SPDLOG_INFO("[Process] 执行选中动作启动：方案={} 动作[{}]={}",
                scheme.schemeName.toStdString(), row, scheme.actions[row].name.toStdString());
}

void ProcessPage::OnNewAction()
{
    SPDLOG_INFO("[Process] 新增动作 clicked");
    if (m_currentSchemeIdx < 0) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先新增或选择一个方案"));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("新建动作"));
    dlg.setStyleSheet("background: #1b222b; color: #dbe6f0;");
    auto* form = new QFormLayout(&dlg);
    form->setSpacing(12);
    form->setContentsMargins(20, 20, 20, 20);

    auto* nameEdit = new QLineEdit();
    nameEdit->setPlaceholderText(QStringLiteral("输入动作名称"));
    nameEdit->setStyleSheet("QLineEdit { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; padding: 6px 10px; font-size: 13px; }");
    form->addRow(QStringLiteral("名称:"), nameEdit);

    auto* typeCombo = new QComboBox();
    typeCombo->addItems({ QStringLiteral("移动"), QStringLiteral("识别"), QStringLiteral("挤压"), QStringLiteral("延时"), QStringLiteral("夹爪") });
    typeCombo->setStyleSheet("QComboBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; padding: 6px 10px; font-size: 13px; } QComboBox::drop-down { width: 20px; border: none; } QAbstractItemView { background: #1a222b; outline: none; selection-background-color: #2f6f9f; selection-color: white; color: #dbe6f0; }");
    form->addRow(QStringLiteral("类型:"), typeCombo);

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    btnBox->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    btnBox->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    btnBox->setStyleSheet("QPushButton { background: #2f6f9f; border: none; border-radius: 6px; padding: 6px 16px; color: white; font-weight: 600; } QPushButton:hover { background: #3a84b8; }");
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(btnBox);

    dlg.adjustSize();
    dlg.setMinimumSize(dlg.sizeHint());

    if (dlg.exec() != QDialog::Accepted) return;
    if (nameEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("动作名称不能为空"));
        return;
    }

    auto& actions = ProcessManager::instance().schemes()[m_currentSchemeIdx].actions;
    ActionData a;
    a.name = nameEdit->text().trimmed();
    a.type = static_cast<ActionType>(typeCombo->currentIndex());
    int insertPos = (m_currentActionIdx >= 0) ? m_currentActionIdx + 1 : actions.size();
    actions.insert(insertPos, a);
    m_currentActionIdx = insertPos;
    RefreshActionList();
    RefreshActionDetail(m_currentActionIdx);
    ProcessManager::instance().save();
}

void ProcessPage::OnEditAction()
{
    SPDLOG_INFO("[Process] 编辑动作 clicked");
    if (m_currentSchemeIdx < 0 || m_currentActionIdx < 0) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先选择一个动作"));
        return;
    }

    auto& action = ProcessManager::instance().schemes()[m_currentSchemeIdx].actions[m_currentActionIdx];

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("编辑动作"));
    dlg.setStyleSheet("background: #1b222b; color: #dbe6f0;");
    auto* form = new QFormLayout(&dlg);
    form->setSpacing(12);
    form->setContentsMargins(20, 20, 20, 20);

    auto* nameEdit = new QLineEdit(action.name);
    nameEdit->setStyleSheet("QLineEdit { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; padding: 6px 10px; font-size: 13px; }");
    form->addRow(QStringLiteral("名称:"), nameEdit);

    auto* typeCombo = new QComboBox();
    typeCombo->addItems({ QStringLiteral("移动"), QStringLiteral("识别"), QStringLiteral("挤压"), QStringLiteral("延时"), QStringLiteral("夹爪") });
    typeCombo->setCurrentIndex(static_cast<int>(action.type));
    typeCombo->setStyleSheet("QComboBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; padding: 6px 10px; font-size: 13px; } QComboBox::drop-down { width: 20px; border: none; } QAbstractItemView { background: #1a222b; outline: none; selection-background-color: #2f6f9f; selection-color: white; color: #dbe6f0; }");
    form->addRow(QStringLiteral("类型:"), typeCombo);

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    btnBox->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    btnBox->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    btnBox->setStyleSheet("QPushButton { background: #2f6f9f; border: none; border-radius: 6px; padding: 6px 16px; color: white; font-weight: 600; } QPushButton:hover { background: #3a84b8; }");
    connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(btnBox);

    dlg.adjustSize();
    dlg.setMinimumSize(dlg.sizeHint());

    if (dlg.exec() != QDialog::Accepted) return;
    if (nameEdit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("动作名称不能为空"));
        return;
    }

    action.name = nameEdit->text().trimmed();
    ActionType newType = static_cast<ActionType>(typeCombo->currentIndex());
    if (newType != action.type) {
        action.type = newType;
        action.points.clear();
        action.visionType.clear(); action.exposure = 0; action.templateName.clear(); action.threshold = 0;
        action.extrudeAmount = 0; action.extrudeSpeed = 0; action.suckBackAmount = 0; action.suckBackSpeed = 0;
        action.delayMs = 0;
        action.isGripperOpen = false;
        action.gripperTarget = 0.0;
    }
    RefreshActionList();
    RefreshActionDetail(m_currentActionIdx);
    ProcessManager::instance().save();
}

void ProcessPage::OnDeleteAction()
{
    SPDLOG_INFO("[Process] 删除动作 clicked");
    if (m_currentSchemeIdx < 0 || m_currentActionIdx < 0) return;
    auto& actions = ProcessManager::instance().schemes()[m_currentSchemeIdx].actions;
    if (m_currentActionIdx >= actions.size()) return;

    auto reply = QMessageBox::question(this,
        QStringLiteral("删除动作"),
        QStringLiteral("确定要删除动作 \"%1\" 吗？\n此操作不可撤销。").arg(actions[m_currentActionIdx].name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    actions.removeAt(m_currentActionIdx);
    if (actions.isEmpty())
        m_currentActionIdx = -1;
    else if (m_currentActionIdx >= actions.size())
        m_currentActionIdx = actions.size() - 1;
    RefreshActionList();
    if (m_currentActionIdx >= 0)
        RefreshActionDetail(m_currentActionIdx);
    else {
        m_detailStack->setCurrentIndex(0);
        m_currentActionLabel->setText(QStringLiteral("当前动作: (无)"));
    }
    ProcessManager::instance().save();
}

void ProcessPage::OnAddPoint()
{
    SPDLOG_INFO("[Process] 添加点位 clicked");
    if (m_currentSchemeIdx < 0 || m_currentActionIdx < 0) return;
    auto& action = ProcessManager::instance().schemes()[m_currentSchemeIdx].actions[m_currentActionIdx];
    if (action.type != ActionType::Move) return;
    PointData pt;
    pt.name = QStringLiteral("point_%1").arg(action.points.size() + 1);
    pt.posture = QStringLiteral("elbow_up");
    action.points.push_back(pt);
    RefreshActionDetail(m_currentActionIdx);
    ProcessManager::instance().save();
}

void ProcessPage::OnDeletePoint()
{
    SPDLOG_INFO("[Process] 删除点位 clicked");
    if (m_currentSchemeIdx < 0 || m_currentActionIdx < 0) return;
    auto& action = ProcessManager::instance().schemes()[m_currentSchemeIdx].actions[m_currentActionIdx];
    if (action.type != ActionType::Move) return;
    int row = m_pointTable->currentRow();
    if (row < 0 || row >= action.points.size()) return;

    auto reply = QMessageBox::question(this,
        QStringLiteral("删除点位"),
        QStringLiteral("确定要删除点位 \"%1\" 吗？\n此操作不可撤销。").arg(action.points[row].name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    action.points.removeAt(row);
    RefreshActionDetail(m_currentActionIdx);
    ProcessManager::instance().save();
}

void ProcessPage::OnMoveUp()
{
    SPDLOG_INFO("[Process] 上移点位 clicked");
    if (m_currentSchemeIdx < 0 || m_currentActionIdx < 0) return;
    auto& pts = ProcessManager::instance().schemes()[m_currentSchemeIdx].actions[m_currentActionIdx].points;
    int row = m_pointTable->currentRow();
    if (row <= 0) return;
    pts.swapItemsAt(row, row - 1);
    RefreshActionDetail(m_currentActionIdx);
    m_pointTable->selectRow(row - 1);
    ProcessManager::instance().save();
}

void ProcessPage::OnMoveDown()
{
    SPDLOG_INFO("[Process] 下移点位 clicked");
    if (m_currentSchemeIdx < 0 || m_currentActionIdx < 0) return;
    auto& pts = ProcessManager::instance().schemes()[m_currentSchemeIdx].actions[m_currentActionIdx].points;
    int row = m_pointTable->currentRow();
    if (row < 0 || row >= pts.size() - 1) return;
    pts.swapItemsAt(row, row + 1);
    RefreshActionDetail(m_currentActionIdx);
    m_pointTable->selectRow(row + 1);
    ProcessManager::instance().save();
}

void ProcessPage::OnMoveActionUp()
{
    SPDLOG_INFO("[Process] 上移动作 clicked");
    if (m_currentSchemeIdx < 0 || m_currentActionIdx < 0) return;
    auto& actions = ProcessManager::instance().schemes()[m_currentSchemeIdx].actions;
    if (m_currentActionIdx <= 0) return;
    actions.swapItemsAt(m_currentActionIdx, m_currentActionIdx - 1);
    --m_currentActionIdx;
    RefreshActionList();
    ProcessManager::instance().save();
}

void ProcessPage::OnMoveActionDown()
{
    SPDLOG_INFO("[Process] 下移动作 clicked");
    if (m_currentSchemeIdx < 0 || m_currentActionIdx < 0) return;
    auto& actions = ProcessManager::instance().schemes()[m_currentSchemeIdx].actions;
    if (m_currentActionIdx < 0 || m_currentActionIdx >= actions.size() - 1) return;
    actions.swapItemsAt(m_currentActionIdx, m_currentActionIdx + 1);
    ++m_currentActionIdx;
    RefreshActionList();
    ProcessManager::instance().save();
}

void ProcessPage::OnTeachRead()
{
    if (m_currentSchemeIdx < 0 || m_currentActionIdx < 0) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先选择「移动」动作"));
        return;
    }
    auto& action = ProcessManager::instance().schemes()[m_currentSchemeIdx].actions[m_currentActionIdx];
    if (action.type != ActionType::Move) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("示教读取仅适用于「移动」动作"));
        return;
    }

    auto& hw = HardwareManager::instance();
    if (!hw.IsGlobalEnabled()) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("请先在手动页使能并回零，再执行示教读取"));
        return;
    }

    Joints cur{ hw.GetPosition(LogicalAxis::J1),
                hw.GetPosition(LogicalAxis::J2),
                hw.GetPosition(LogicalAxis::Z),
                hw.GetPosition(LogicalAxis::R) };

    Kinematics kin = KinematicsHelper::FromConfig();
    Pose pose = kin.Forward(cur);

    PointData pt;
    pt.name = QStringLiteral("点_%1").arg(action.points.size() + 1, 3, 10, QLatin1Char('0'));
    pt.x = pose.x;
    pt.y = pose.y;
    pt.z = pose.z;
    pt.r = cur.r;
    pt.posture = QStringLiteral("elbow_up");
    action.points.push_back(pt);

    RefreshActionDetail(m_currentActionIdx);
    m_pointTable->selectRow(m_pointTable->rowCount() - 1);
    ProcessManager::instance().save();

    SPDLOG_INFO("[Process] Teach read: J({:.2f},{:.2f},{:.2f},{:.2f}) -> P({:.2f},{:.2f},{:.2f})",
        cur.j1, cur.j2, cur.z, cur.r, pose.x, pose.y, pose.z);
}

void ProcessPage::OnSaveAction()
{
    SPDLOG_INFO("[Process] 保存动作 clicked");
    if (m_currentSchemeIdx < 0 || m_currentActionIdx < 0) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先选择一个动作"));
        return;
    }

    auto& action = ProcessManager::instance().schemes()[m_currentSchemeIdx].actions[m_currentActionIdx];

    // 保存前按最新轴5 软限位刷新行程输入框范围（config 改限位后无需切换动作也能夹正）
    RefreshGripperLimitRange();

    if (m_speedPercentSpin)
        action.speedPercent = m_speedPercentSpin->value();

    switch (action.type) {
    case ActionType::Move: {
        QVector<PointData> pts;
        for (int r = 0; r < m_pointTable->rowCount(); ++r) {
            auto* item0 = m_pointTable->item(r, 0);
            auto* item1 = m_pointTable->item(r, 1);
            auto* item2 = m_pointTable->item(r, 2);
            auto* item3 = m_pointTable->item(r, 3);
            auto* item4 = m_pointTable->item(r, 4);
            if (!item0 || !item1 || !item2 || !item3 || !item4) continue;
            PointData pt;
            pt.name = item0->text();
            pt.x = item1->text().toDouble();
            pt.y = item2->text().toDouble();
            pt.z = item3->text().toDouble();
            pt.r = item4->text().toDouble();
            auto* combo = qobject_cast<QComboBox*>(m_pointTable->cellWidget(r, 5));
            pt.posture = combo ? combo->currentText() : QStringLiteral("elbow_up");
            pts.push_back(pt);
        }
        action.points = pts;
        break;
    }
    case ActionType::Vision:
        action.visionType = m_visionTypeCombo->currentText();
        action.exposure = m_exposureEdit->text().toDouble();
        action.templateName = m_templateEdit->text();
        action.threshold = m_thresholdEdit->text().toDouble();
        break;
    case ActionType::Extrude:
        action.extrudeAmount = m_extrudeAmountEdit->text().toDouble();
        action.extrudeSpeed = m_extrudeSpeedEdit->text().toDouble();
        action.suckBackAmount = m_suckBackAmountEdit->text().toDouble();
        action.suckBackSpeed = m_suckBackSpeedEdit->text().toDouble();
        break;
    case ActionType::Delay:
        action.delayMs = m_delaySpin->value();
        break;
    case ActionType::Gripper:
        action.isGripperOpen = (m_gripperCombo->currentIndex() == 1);
        action.gripperTarget = m_gripperTargetSpin->value();
        break;
    }

    RefreshActionList();
    ProcessManager::instance().save();
    QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("保存成功"));
    SPDLOG_INFO("[Process] Saved action: {} ({})", action.name.toStdString(),
        ProcessManager::actionTypeName(action.type).toStdString());
}

void ProcessPage::OnSchemeNameEdited()
{
    if (m_currentSchemeIdx < 0) return;
    auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx >= schemes.size()) return;

    QString newName = m_schemeNameEdit->text().trimmed();
    if (newName.isEmpty()) {
        m_schemeNameEdit->setText(schemes[m_currentSchemeIdx].schemeName);
        return;
    }
    if (newName == schemes[m_currentSchemeIdx].schemeName) return;

    schemes[m_currentSchemeIdx].schemeName = newName;
    RefreshSchemeCombo();
    ProcessManager::instance().save();
    SPDLOG_INFO("[Process] Scheme renamed: {}", newName.toStdString());
}
