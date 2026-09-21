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
#include <QTreeWidget>
#include <QStandardItemModel>
#include <QInputDialog>
#include <QHeaderView>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QDebug>
#include <QPainter>
#include <cmath>
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
    case 6: // group box（新建分组）
        p.drawRect(QRectF(3.5, 4.5, 13.0, 11.0));
        p.drawLine(QPointF(3.5, 8.5), QPointF(16.5, 8.5));
        break;
    }
    p.end();
    return QIcon(pm);
}

// 节点树显示文本：用**持久化编号**（#id）而非行号——行号会随插入/删除/移动变化，
// 编号不重排不回收，是 sourceActionId 引用（P5）与 UI 之间唯一稳定的锚点。
QString NodeLabel(const ActionData& n)
{
    if (n.isGroup())
        return QStringLiteral("#%1 〔分组〕 %2（%3 个子动作）")
            .arg(n.id).arg(n.name).arg(n.children.size());
    return QStringLiteral("#%1 (%2) %3")
        .arg(n.id).arg(ProcessManager::actionTypeName(n.type)).arg(n.name);
}

// 详情页索引：0=占位，1..6 = ActionType + 1，7 = 分组页。
// ⚠ 分组页**必须存在**：RefreshNodeDetail 用 `static_cast<int>(type) + 1` 当 stack 索引，
// 缺页时 setCurrentIndex 越界会被 Qt 静默忽略 → 界面停留在上一个节点的面板上（错显）。
constexpr int kGroupPageIndex = 7;

// 节点在给定列表里的下标（-1 = 不在其中）
int IndexInList(const QVector<ActionData>& list, int id)
{
    for (int i = 0; i < list.size(); ++i)
        if (list[i].id == id) return i;
    return -1;
}

// 一组节点已占用的**资源标识**（去重，跳过分组与 excludeId）——
// 防呆置灰与组内校验共用同一套资源定义（ProcessManager::resourceKeysOf，§六 按资源不按类型）
QStringList OccupiedResourceKeys(const QVector<ActionData>& nodes, int excludeId = 0)
{
    QStringList used;
    for (const auto& n : nodes) {
        if (n.id == excludeId || n.isGroup()) continue;
        for (const auto& k : ProcessManager::resourceKeysOf(n.type))
            if (!used.contains(k)) used.push_back(k);
    }
    return used;
}

// 本方案内是否存在「编号 = id 且类型为识别」的节点（**全树扫描**：一级节点 + 分组子节点）。
// P6：动作可以位于分组内，视觉引导的引用校验必须覆盖全树，否则组内的识别动作无法被引用。
const ActionData* FindVisionNode(const QVector<ActionData>& nodes, int id)
{
    if (id <= 0) return nullptr;
    for (const auto& n : nodes) {
        if (!n.isGroup() && n.id == id && n.type == ActionType::Vision) return &n;
        if (n.isGroup())
            for (const auto& c : n.children)
                if (c.id == id && c.type == ActionType::Vision) return &c;
    }
    return nullptr;
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
        RefreshNodeTree();
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

    // P6：QListWidget → QTreeWidget（两级：一级节点 = 动作 / 分组，分组的子节点为动作）
    m_nodeTree = new QTreeWidget();
    m_nodeTree->setHeaderHidden(true);
    m_nodeTree->setRootIsDecorated(true);
    m_nodeTree->setIndentation(16);
    m_nodeTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_nodeTree->setStyleSheet(R"(
        QTreeWidget { background: #121a22; border: none; border-radius: 8px; padding: 4px; outline: none; }
        QTreeWidget::item { min-height: 32px; padding: 4px 6px; color: #b8cce3; border-radius: 4px; font-size: 13px; }
        QTreeWidget::item:selected { background: #2f6f9f; color: white; }
        QTreeWidget::item:hover:!selected { background: #1e2a36; }
        QTreeWidget::branch { background: transparent; }
        QScrollBar:vertical { width: 28px; background: #1a2430; border: none; margin: 0; }
        QScrollBar::handle:vertical { background: #ffffff; min-height: 50px; border-radius: 8px; margin: 2px 4px; }
        QScrollBar::handle:vertical:hover { background: #e0e8f0; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; border: none; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: #1a2430; }
    )");
    // 选中变化：由 currentRowChanged（扁平行号）改为 currentItemChanged（树条目），
    // 节点标识取自条目的 UserRole（= 持久化编号），不再依赖行号。
    connect(m_nodeTree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
        if (!cur) return;
        const int id = cur->data(0, Qt::UserRole).toInt();
        if (id <= 0) return;
        m_currentNodeId = id;
        RefreshNodeDetail(id);
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

    // 新建分组（P6）：组内子动作并发执行，压缩节拍
    auto* newGroupBtn = new QPushButton();
    newGroupBtn->setIcon(makeActionIcon(6));
    newGroupBtn->setIconSize(QSize(18, 18));
    newGroupBtn->setToolTip(QStringLiteral("新建分组（组内子动作并发执行）"));
    newGroupBtn->setStyleSheet(makeBtnStyle("#6f5f8f", "#7f6f9f"));
    newGroupBtn->setCursor(Qt::PointingHandCursor);
    newGroupBtn->setFixedSize(40, 32);
    connect(newGroupBtn, &QPushButton::clicked, this, &ProcessPage::OnNewGroup);

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
    actionBtnRow->addWidget(newGroupBtn);
    actionBtnRow->addWidget(editActionBtn);
    actionBtnRow->addWidget(deleteActionBtn);
    actionBtnRow->addWidget(upActionBtn);
    actionBtnRow->addWidget(downActionBtn);
    actionBtnRow->addWidget(m_runSelectedBtn);
    actionBtnRow->addStretch();

    leftLayout->addWidget(actionHeader);
    leftLayout->addWidget(m_nodeTree, 1);
    leftLayout->addLayout(actionBtnRow);

    splitLayout->addWidget(leftPanel, 3);

    // Right stack
    auto* rightPanel = new QWidget();
    rightPanel->setStyleSheet("background: #1b222b; border-radius: 12px; border: 1px solid #384550;");
    rightPanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(10, 10, 10, 10);
    rightLayout->setSpacing(8);

    m_currentActionLabel = new QLabel(QStringLiteral("当前节点: (无)"));
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

    // Page 6: VisionGuide（P5 落地，doc/vision_guide_action_plan.md §5.1 / §5.5）
    // ⚠ 这一页**必须存在**：RefreshNodeDetail 用 `static_cast<int>(type) + 1` 当 stack 索引，
    // 缺页时 setCurrentIndex(6) 越界会被 Qt 静默忽略 → 界面**停留在上一个动作的面板**上（错显）。
    auto* visionGuidePage = new QWidget();
    auto* vgf = makeFormPage(visionGuidePage);

    const QString comboStyle = QStringLiteral(
        "QComboBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; "
        "border-radius: 6px; padding: 4px 8px; font-size: 13px; } "
        "QComboBox::drop-down { width: 18px; border: none; }");
    const QString spinStyle = QStringLiteral(
        "QDoubleSpinBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; "
        "border-radius: 6px; padding: 4px 8px; font-size: 13px; }");

    m_guideSourceCombo = new QComboBox();
    m_guideSourceCombo->setFixedHeight(30);
    m_guideSourceCombo->setStyleSheet(comboStyle);
    vgf->addRow(makeLabel(QStringLiteral("引用识别动作")), m_guideSourceCombo);

    // offsetZ 按**末端高度 Pose.z** 算、正值向上（决策 3）；范围给足，越界由软限位在运行期拦。
    m_guideOffsetZSpin = new QDoubleSpinBox();
    m_guideOffsetZSpin->setRange(-500.0, 500.0);
    m_guideOffsetZSpin->setDecimals(2);
    m_guideOffsetZSpin->setSingleStep(1.0);
    m_guideOffsetZSpin->setSuffix(QStringLiteral(" mm"));
    m_guideOffsetZSpin->setFixedHeight(30);
    m_guideOffsetZSpin->setStyleSheet(spinStyle);
    vgf->addRow(makeLabel(QStringLiteral("目标上方偏移")), m_guideOffsetZSpin);

    // guideR：轴 4(R) 的固定目标角。默认 179.9（离 ±180 回绕奇点稍远，读数更稳，§七）。
    // 上限取 R 轴软限位上限附近；实际下发前 ResolveRunnerJoints 会做 NormalizeRotationAngle 归一化。
    m_guideRSpin = new QDoubleSpinBox();
    m_guideRSpin->setRange(-180.0, 185.0);
    m_guideRSpin->setDecimals(1);
    m_guideRSpin->setSingleStep(0.1);
    m_guideRSpin->setSuffix(QStringLiteral(" °"));
    m_guideRSpin->setFixedHeight(30);
    m_guideRSpin->setStyleSheet(spinStyle);
    vgf->addRow(makeLabel(QStringLiteral("引导 R 角")), m_guideRSpin);

    // 两组坐标**只读**预览（决策 1）：坐标由识别结果决定，手改会与识别结果不一致，故不可编辑。
    m_guidePreviewLabel = new QLabel();
    m_guidePreviewLabel->setWordWrap(true);
    m_guidePreviewLabel->setMinimumHeight(76);
    m_guidePreviewLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_guidePreviewLabel->setStyleSheet(
        "color: #8da3bb; background: #111a22; border: 1px solid #3f4e5e; "
        "border-radius: 6px; padding: 8px; font-size: 13px;");
    vgf->addRow(m_guidePreviewLabel);

    auto* vgHint = makeLabel(QStringLiteral(
        "顺序严格「先目标上方、后目标」（与安全位同源约束）。坐标来自识别结果，"
        "**不写入 process.json**，仅本次会话内有效；点「保存动作」只保存引用编号与参数。"));
    vgHint->setWordWrap(true);
    vgf->addRow(vgHint);

    // 参数改动即时刷新预览（不必等保存）
    connect(m_guideOffsetZSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { UpdateGuidePreview(); });
    connect(m_guideRSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { UpdateGuidePreview(); });
    connect(m_guideSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { UpdateGuidePreview(); });

    auto* guideSaveLayout = new QHBoxLayout();
    guideSaveLayout->addStretch();
    auto* guideSaveBtn = new QPushButton(QStringLiteral("保存动作"));
    guideSaveBtn->setStyleSheet(saveBtnStyle);
    guideSaveBtn->setCursor(Qt::PointingHandCursor);
    connect(guideSaveBtn, &QPushButton::clicked, this, &ProcessPage::OnSaveAction);
    guideSaveLayout->addWidget(guideSaveBtn);
    vgf->addRow(guideSaveLayout);
    m_detailStack->addWidget(visionGuidePage);

    // Page 7: Group（P6 §八 第 8 项）——分组没有动作参数，只有编号 / 名称 / 子动作清单
    // 与组内并发校验结果。索引由 kGroupPageIndex 固定（= 7）。
    auto* groupPage = new QWidget();
    auto* gpf = makeFormPage(groupPage);
    m_groupInfoLabel = new QLabel();
    m_groupInfoLabel->setWordWrap(true);
    m_groupInfoLabel->setMinimumHeight(140);
    m_groupInfoLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_groupInfoLabel->setStyleSheet(
        "color: #b8cce3; background: #111a22; border: 1px solid #3f4e5e; "
        "border-radius: 6px; padding: 10px; font-size: 13px;");
    gpf->addRow(m_groupInfoLabel);

    auto* groupHint = makeLabel(QStringLiteral(
        "分组内的子动作同时下发、统一汇合：等待时间重叠，故节拍约等于最长子动作，"
        "而不是各子动作之和。\n"
        "同一分组内不允许两个子动作争用同一硬件资源（含相机与各轴），也不允许「识别」与"
        "引用它的「视觉引导」同组；分组不能嵌套。\n"
        "把动作加入分组：选中分组（或组内任一动作）后点「新建动作」。"));
    groupHint->setWordWrap(true);
    gpf->addRow(groupHint);
    m_detailStack->addWidget(groupPage);

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

// 按编号在树里找条目（一级节点或分组子节点）。找不到返回 nullptr。
QTreeWidgetItem* ProcessPage::FindTreeItem(int nodeId) const
{
    if (!m_nodeTree || nodeId <= 0) return nullptr;
    for (int i = 0; i < m_nodeTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = m_nodeTree->topLevelItem(i);
        if (!top) continue;
        if (top->data(0, Qt::UserRole).toInt() == nodeId) return top;
        for (int j = 0; j < top->childCount(); ++j) {
            QTreeWidgetItem* ch = top->child(j);
            if (ch && ch->data(0, Qt::UserRole).toInt() == nodeId) return ch;
        }
    }
    return nullptr;
}

ActionData* ProcessPage::CurrentNode()
{
    auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx < 0 || m_currentSchemeIdx >= schemes.size()) return nullptr;
    return ProcessManager::findNode(schemes[m_currentSchemeIdx].nodes, m_currentNodeId);
}

int ProcessPage::TopLevelIndex(int nodeId) const
{
    const auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx < 0 || m_currentSchemeIdx >= schemes.size()) return -1;
    return IndexInList(schemes[m_currentSchemeIdx].nodes, nodeId);
}

void ProcessPage::RefreshNodeTree()
{
    m_nodeTree->blockSignals(true);
    m_nodeTree->clear();

    if (m_currentSchemeIdx >= 0 && m_currentSchemeIdx < ProcessManager::instance().schemes().size()) {
        const auto& nodes = ProcessManager::instance().schemes()[m_currentSchemeIdx].nodes;
        for (const auto& n : nodes) {
            auto* item = new QTreeWidgetItem(m_nodeTree);
            item->setText(0, NodeLabel(n));
            // 编号挂在 UserRole：**一切选中 / 定位都按编号**，不按行号（行号会漂移）
            item->setData(0, Qt::UserRole, n.id);
            if (n.isGroup()) {
                for (const auto& c : n.children) {
                    auto* ci = new QTreeWidgetItem(item);
                    ci->setText(0, NodeLabel(c));
                    ci->setData(0, Qt::UserRole, c.id);
                }
                item->setExpanded(true);
            }
        }
    }

    QTreeWidgetItem* target = FindTreeItem(m_currentNodeId);
    if (!target && m_nodeTree->topLevelItemCount() > 0)
        target = m_nodeTree->topLevelItem(0);
    if (target) m_nodeTree->setCurrentItem(target);
    m_nodeTree->blockSignals(false);

    if (target) {
        m_currentNodeId = target->data(0, Qt::UserRole).toInt();
        RefreshNodeDetail(m_currentNodeId);
    } else {
        m_currentNodeId = 0;
        m_detailStack->setCurrentIndex(0);
        m_currentActionLabel->setText(QStringLiteral("当前节点: (无)"));
    }
}

void ProcessPage::RefreshNodeDetail(int nodeId)
{
    auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx < 0 || m_currentSchemeIdx >= schemes.size()) {
        m_currentActionLabel->setText(QStringLiteral("当前节点: (无)"));
        m_detailStack->setCurrentIndex(0);
        return;
    }
    ActionData* node = ProcessManager::findNode(schemes[m_currentSchemeIdx].nodes, nodeId);
    if (!node) {
        m_currentActionLabel->setText(QStringLiteral("当前节点: (无)"));
        m_detailStack->setCurrentIndex(0);
        return;
    }

    // 显示持久化编号（与节点树 #N 一致）；不再用行号——行号不是编号
    m_currentActionLabel->setText(
        node->isGroup() ? QStringLiteral("当前节点: #%1 〔分组〕 %2").arg(node->id).arg(node->name)
                        : QStringLiteral("当前节点: #%1 %2").arg(node->id).arg(node->name));
    m_currentNodeId = nodeId;

    // ---- 分组：编号 / 名称 / 子动作清单 / 组内并发校验结果（§八 第 8 项）----
    if (node->isGroup()) {
        QString info = QStringLiteral("编号：#%1\n名称：%2\n子动作：%3 个（并发执行）\n\n")
                           .arg(node->id).arg(node->name).arg(node->children.size());
        if (node->children.isEmpty()) {
            info += QStringLiteral("⚠ 空分组：执行时直接跳过。请选中本分组后点「新建动作」把动作加进来。");
        } else {
            const QString bad = ProcessManager::validateGroupChildren(node->children);
            if (bad.isEmpty()) {
                info += QStringLiteral("✓ 组内无资源冲突，可并发执行。\n"
                                       "节拍 ≈ 最长子动作，而不是各子动作之和。");
                for (const auto& c : node->children)
                    info += QStringLiteral("\n  · #%1 %2（%3）")
                                .arg(c.id).arg(c.name)
                                .arg(ProcessManager::actionTypeName(c.type));
            } else {
                info += QStringLiteral("✖ 组内冲突：%1").arg(bad);
            }
        }
        m_groupInfoLabel->setText(info);
        m_speedPercentRow->setVisible(false);
        m_detailStack->setCurrentIndex(kGroupPageIndex);
        return;
    }

    int stackIdx = static_cast<int>(node->type) + 1;
    if (stackIdx >= 0 && stackIdx < m_detailStack->count())
        m_detailStack->setCurrentIndex(stackIdx);

    // 视觉引导也走两段移动动作，速度百分比同样生效 → 一并显示（P5）
    bool hasSpeedPercent = (node->type == ActionType::Move ||
                            node->type == ActionType::Extrude ||
                            node->type == ActionType::Gripper ||
                            node->type == ActionType::VisionGuide);
    m_speedPercentRow->setVisible(hasSpeedPercent);
    if (hasSpeedPercent) {
        m_speedPercentSpin->setValue(node->speedPercent);
        m_speedPercentSlider->setValue(node->speedPercent);
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

    switch (node->type) {
    case ActionType::Move:
        while (m_pointTable->rowCount() > 0)
            m_pointTable->removeRow(0);
        for (int r = 0; r < node->points.size(); ++r) {
            const auto& pt = node->points[r];
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
        m_visionTypeCombo->setCurrentText(node->visionType);
        m_exposureEdit->setText(QString::number(node->exposure));
        m_templateEdit->setText(node->templateName);
        m_thresholdEdit->setText(QString::number(node->threshold));
        break;
    case ActionType::Extrude:
        m_extrudeAmountEdit->setText(QString::number(node->extrudeAmount));
        m_extrudeSpeedEdit->setText(QString::number(node->extrudeSpeed));
        m_suckBackAmountEdit->setText(QString::number(node->suckBackAmount));
        m_suckBackSpeedEdit->setText(QString::number(node->suckBackSpeed));
        break;
    case ActionType::Delay:
        m_delaySpin->setValue(node->delayMs);
        break;
    case ActionType::Gripper:
        RefreshGripperLimitRange();
        m_gripperCombo->blockSignals(true);
        m_gripperCombo->setCurrentIndex(node->isGripperOpen ? 1 : 0);
        m_gripperCombo->blockSignals(false);
        m_gripperTargetSpin->setValue(node->gripperTarget);
        if (node->isGripperOpen) m_lastOpenTarget = node->gripperTarget;
        else                     m_lastCloseTarget = node->gripperTarget;
        break;
    case ActionType::VisionGuide:
        // P5：回填引用与参数，并刷新只读坐标预览（引用悬垂 / 无结果都会在预览里显式提示）
        RefreshGuideSourceCombo(node->sourceActionId);
        m_guideOffsetZSpin->setValue(node->offsetZ);
        m_guideRSpin->setValue(node->guideR);
        UpdateGuidePreview();
        break;
    }
}

// ---- 视觉引导（P5）辅助 ----

void ProcessPage::RefreshGuideSourceCombo(int selectedId)
{
    if (!m_guideSourceCombo) return;
    m_guideSourceCombo->blockSignals(true);
    m_guideSourceCombo->clear();

    const auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx >= 0 && m_currentSchemeIdx < schemes.size()) {
        // 只列本方案的「识别」动作（决策 2：不允许跨方案引用）。
        // P6：动作可能位于分组内 —— 一级节点与分组子节点都要遍历，否则组内的识别动作选不到。
        const auto& nodes = schemes[m_currentSchemeIdx].nodes;
        for (const auto& n : nodes) {
            if (!n.isGroup() && n.type == ActionType::Vision)
                m_guideSourceCombo->addItem(QStringLiteral("#%1 %2").arg(n.id).arg(n.name),
                                            QVariant(n.id));
            if (n.isGroup())
                for (const auto& c : n.children)
                    if (c.type == ActionType::Vision)
                        m_guideSourceCombo->addItem(
                            QStringLiteral("#%1 %2（组内）").arg(c.id).arg(c.name), QVariant(c.id));
        }
    }
    if (selectedId > 0) {
        const int i = m_guideSourceCombo->findData(QVariant(selectedId));
        if (i >= 0) {
            m_guideSourceCombo->setCurrentIndex(i);
        } else {
            // 悬垂引用：引用的识别动作已被删除 / 改了类型。**显式列出来**而不是静默落到第一个，
            // 否则用户看到的是一个合法下拉，保存下去却把引用悄悄指向了别的动作。
            m_guideSourceCombo->addItem(
                QStringLiteral("#%1（已不存在）").arg(selectedId), QVariant(selectedId));
            m_guideSourceCombo->setCurrentIndex(m_guideSourceCombo->count() - 1);
        }
    }
    m_guideSourceCombo->blockSignals(false);
}

void ProcessPage::UpdateGuidePreview()
{
    if (!m_guidePreviewLabel || !m_guideSourceCombo || !m_guideOffsetZSpin || !m_guideRSpin) return;
    ActionData* node = CurrentNode();
    if (!node || node->isGroup() || node->type != ActionType::VisionGuide) return;

    const int src = m_guideSourceCombo->currentData().isValid()
                        ? m_guideSourceCombo->currentData().toInt()
                        : 0;
    const double dz = m_guideOffsetZSpin->value();
    const double r  = m_guideRSpin->value();

    // 引用有效性：必须指向**本方案内**类型为「识别」的动作（决策 2 / §4.3）。
    // P6：全树扫描（动作可在分组内）。
    bool refOk = false;
    {
        const auto& schemes = ProcessManager::instance().schemes();
        if (m_currentSchemeIdx >= 0 && m_currentSchemeIdx < schemes.size())
            refOk = (FindVisionNode(schemes[m_currentSchemeIdx].nodes, src) != nullptr);
    }
    const auto it = m_visionSnapshots.constFind(src);
    const bool hasCoord = (it != m_visionSnapshots.constEnd() && it.value().valid);

    QString text;
    QString color = QStringLiteral("#8da3bb");
    if (src <= 0) {
        text = QStringLiteral("⚠ 未选择引用的识别动作 —— 保存会被拒绝。");
        color = QStringLiteral("#f7c948");
    } else if (!refOk) {
        text = QStringLiteral("⚠ 引用的识别动作 #%1 不存在或不是「识别」类型 —— 保存会被拒绝。").arg(src);
        color = QStringLiteral("#ff5e6b");
    } else if (!hasCoord) {
        text = QStringLiteral("引用 #%1：本次会话尚未产出识别坐标。\n"
                              "运行该识别动作后，此处显示两组路径点（只读）。").arg(src);
    } else {
        const double x = it.value().x, y = it.value().y, z = it.value().z;
        text = QStringLiteral("引用 #%1 的识别结果（基座坐标，只读）：\n"
                              "目标上方  (%2, %3, %4)\n"
                              "目标      (%5, %6, %7)\n"
                              "r = %8°（轴 4 固定值，与 yaw 无关）")
                   .arg(src)
                   .arg(x, 0, 'f', 1).arg(y, 0, 'f', 1).arg(z + dz, 0, 'f', 1)
                   .arg(x, 0, 'f', 1).arg(y, 0, 'f', 1).arg(z, 0, 'f', 1)
                   .arg(r, 0, 'f', 1);
    }
    m_guidePreviewLabel->setStyleSheet(
        QStringLiteral("color: %1; background: #111a22; border: 1px solid #3f4e5e; "
                       "border-radius: 6px; padding: 8px; font-size: 13px;").arg(color));
    m_guidePreviewLabel->setText(text);
}

void ProcessPage::OnVisionResultReady(int actionId, double x, double y, double z)
{
    VisionSnapshot snap;
    snap.x = x; snap.y = y; snap.z = z; snap.valid = true;
    m_visionSnapshots.insert(actionId, snap);
    SPDLOG_INFO("[Process] vision result #{}: ({:.1f}, {:.1f}, {:.1f})", actionId, x, y, z);
    UpdateGuidePreview();          // 当前若正停在某条视觉引导上，立即刷新预览
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
    RefreshNodeTree();
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
        m_currentNodeId = 0;
        m_schemeNameEdit->clear();
    } else {
        m_currentSchemeIdx = 0;
        m_currentNodeId = 0;
    }
    RefreshSchemeCombo();
    RefreshNodeTree();
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
    m_currentNodeId = 0;
    m_schemeNameEdit->setText(schemes[idx].schemeName);
    RefreshNodeTree();
}

void ProcessPage::SetSequenceWorker(SequenceWorker* worker)
{
    m_worker = worker;
    if (!m_worker) return;
    connect(m_worker, &SequenceWorker::logMessage, this, [this](const QString& msg) {
        SPDLOG_INFO("[Process] worker: {}", msg.toStdString());
    });

    // 识别结果（P5）：worker 线程 → UI 线程（Queued）。只用于视觉引导编辑区的**只读**坐标预览，
    // 不做任何落盘（决策 5）。未连接时该信号无接收者，功能不受影响。
    connect(m_worker, &SequenceWorker::visionResultReady,
            this, &ProcessPage::OnVisionResultReady);

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
    // 状态消费者改为 enum（原为 4 个中文字符串比较，改文案即静默错配）。
    // 单步按钮可点的权威条件 = Paused && reason==Step：比字符串判定更强，
    // 守住 D2 修复（"每次点击必然落在暂停期"，杜绝执行期提前放行连跑两个动作）。
    connect(m_worker, &SequenceWorker::stateChanged, this,
            [this, setStepEnabled](SequenceWorker::WorkerState st,
                                   SequenceWorker::PauseReason reason) {
        using WS = SequenceWorker::WorkerState;
        using PR = SequenceWorker::PauseReason;
        SPDLOG_INFO("[Process] worker state: {} reason: {}",
                    static_cast<int>(st), static_cast<int>(reason));
        // 会话期间禁用「执行选中动作」（UI 防呆）：单动作与任何执行中会话互斥，
        // 与其点了才被拒，不如直接灰掉（与单步按钮同源联动）
        if (m_runSelectedBtn)
            m_runSelectedBtn->setEnabled(st == WS::Idle || st == WS::Fault);
        switch (st) {
        case WS::Running:
            // 单动作会话沿用原「执行选中动作」文案（原字符串语义不丢）
            if (m_worker->IsSingleActionSession())
                SetStatusText(QStringLiteral("● 执行选中动作"), QStringLiteral("#4fb0d8"));
            else
                SetStatusText(QStringLiteral("● 运行中"), QStringLiteral("#7ed67e"));
            setStepEnabled(false);      // 动作执行中：禁用，杜绝提前放行
            break;
        case WS::Paused:
            if (reason == PR::Step) {
                SetStatusText(QStringLiteral("● 单步暂停"), QStringLiteral("#e0a520"));
                setStepEnabled(true);   // 仅"单步挂起"才允许释放下一步
            } else {
                // 用户从自动运行页暂停：NextStep 在此态无效，灰掉防误点
                SetStatusText(QStringLiteral("● 已暂停（自动运行页）"), QStringLiteral("#e0a520"));
                setStepEnabled(false);
            }
            break;
        case WS::Fault:
            SetStatusText(QStringLiteral("✖ 故障（请清除报警）"), QStringLiteral("#d14444"));
            setStepEnabled(false);
            break;
        case WS::Idle:
            // 会话结束：优先保留终态（完成/已停止/出错），否则回到空闲
            SetStatusText(m_finalStatus.isEmpty() ? QStringLiteral("● 空闲") : m_finalStatus,
                          m_finalColor.isEmpty() ? QStringLiteral("#b8cce3") : m_finalColor);
            setStepEnabled(true);       // 会话结束，使能交回 homed 联动
            break;
        }
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
    // 单步联动：形参由「扁平行号」改为**节点编号**（P6 §八 第 10 项）
    connect(m_worker, &SequenceWorker::actionStarted, this, [this, setStepEnabled](int id, const QString& name) {
        // 安全位临时方案（TR-074）不归属工艺流程页的方案列表：跳过选中联动，
        // 否则回安全位会把本页节点树误选到第 0/1 个节点
        if (m_worker->IsSafePosSession()) return;
        SPDLOG_INFO("[Process] 单步 actionStarted #{} {}", id, name.toStdString());
        // 自动选中当前正在执行的节点（单步/自动/单动作共用）：按编号在树里定位，
        // 经 currentItemChanged 同步 m_currentNodeId 并刷新右侧详情，执行进度在节点树可见
        if (auto* item = FindTreeItem(id)) m_nodeTree->setCurrentItem(item);
        setStepEnabled(false);  // 节点已开始执行，禁用单步按钮
    });
    connect(m_worker, &SequenceWorker::actionFinished, this, [this](int id, const QString& name) {
        SPDLOG_INFO("[Process] 单步 actionFinished #{} {}", id, name.toStdString());
    });
    // 分组并发调度（P6）：与 actionStarted 的区别是粒度——这里只在「组内并发调度」这一层发
    connect(m_worker, &SequenceWorker::groupStarted, this, [this](int id, const QString& name) {
        SPDLOG_INFO("[Process] 分组并发开始 #{} {}", id, name.toStdString());
        SetStatusText(QStringLiteral("● 分组并发执行中"), QStringLiteral("#4fb0d8"));
    });
    connect(m_worker, &SequenceWorker::groupFinished, this, [this](int id, const QString& name) {
        SPDLOG_INFO("[Process] 分组并发结束 #{} {}", id, name.toStdString());
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

// 模式切换强制清理（TR-075，Q12）：MainWindow 切【自动】时若单步会话仍有残留则调用。
// Stop() 只置 cancel：Pausing(Step) 挂起会退出并走 interrupted 终态；Fault/Idle 态为 no-op
//（cancel 残留由 RunSequence 启动时复位，无害）。仅确有残留时才动状态文本。
void ProcessPage::AbortStepSession()
{
    const bool had = IsExecutionActive();
    if (m_worker) m_worker->Stop();
    ResetStepSession();
    if (had) {
        SetStatusText(QStringLiteral("■ 已停止（模式切换清理）"), QStringLiteral("#b8cce3"));
        SPDLOG_WARN("[ProcessPage] 模式切换强制清理单步会话");
    }
}

void ProcessPage::ApplySpeedPercentToCurrentAction(int v)
{
    ActionData* node = CurrentNode();
    if (!node || node->isGroup()) return;
    node->speedPercent = v;
}

void ProcessPage::ApplyGripperTargetToCurrentAction(double v)
{
    ActionData* node = CurrentNode();
    if (!node || node->isGroup() || node->type != ActionType::Gripper) return;
    node->gripperTarget = v;
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

    // 防御（异常路径兜底）：若 m_stepActive 与引擎态不同步（如历史 bug 中会话标志被误清），
    // 而引擎确实处于"单步挂起"，则按"释放下一步"处理——否则会误走新会话启动分支被 running
    // 门禁拒绝，单步会话永久卡死（2026-09-09 真机实测）。
    if (!m_stepActive
        && m_worker->GetState() == SequenceWorker::WorkerState::Paused
        && m_worker->GetPauseReason() == SequenceWorker::PauseReason::Step
        && m_worker->IsStepMode()) {
        SPDLOG_WARN("[Process] 单步执行：会话标志与引擎态不同步，按释放下一步处理");
        m_stepActive = true;
    }

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

    // P6：按**节点编号**执行（不再是扁平行号）——编号持久化，插入/删除/移动后仍指向同一节点。
    // 选中的是分组 → 并发执行整组；选中的是组内子节点 → 只执行该子节点。
    const int nodeId = m_currentNodeId;
    const ActionData* node = ProcessManager::findNode(scheme.nodes, nodeId);
    if (!node) {
        QMessageBox::warning(this, QStringLiteral("提示"),
                             QStringLiteral("请先选择一个动作或分组"));
        return;
    }

    // 执行期门禁：单动作与单步/自动会话互斥。
    // ⚠ 原实现在此处 `if (m_stepActive) ResetStepSession();` —— 在 RunSingleAction 之前就清了
    // m_stepActive，而 RunSingleAction 随后被引擎拒绝（already running）→ 标志已丢、worker 仍在
    // 单步会话 → 之后点「单步执行」走启动分支被拒，单步会话永久卡死（2026-09-09 真机实测）。
    // 正确做法：**先判门禁、拒绝时不动任何状态**（被拒后单步可继续）。
    if (IsExecutionActive()) {
        SPDLOG_WARN("[Process] 执行选中动作被拒：已有会话在执行（单步调试或自动运行）");
        QMessageBox::warning(this, QStringLiteral("提示"),
            QStringLiteral("已有流程在执行（单步调试或自动运行）。\n请先「停止」结束当前会话，再执行选中动作。"));
        return;
    }
    // 每次执行前刷新运动学/手眼（与 OnStepExecute 一致，保证与 ConfigPage 编辑同步）
    m_worker->ReloadFromConfig();
    m_finalStatus.clear();
    m_finalColor.clear();
    bool ok = m_worker->RunSingleAction(scheme, nodeId);
    if (!ok) {
        // 拒绝（未使能/运行中/越界）不发完成信号，立即恢复按钮，避免永久禁用
        m_runSelectedBtn->setEnabled(true);
        return;
    }
    m_runSelectedBtn->setEnabled(false);
    SPDLOG_INFO("[Process] 执行选中节点启动：方案={} 节点 #{}={}",
                scheme.schemeName.toStdString(), nodeId, node->name.toStdString());
}

void ProcessPage::OnNewAction()
{
    SPDLOG_INFO("[Process] 新增动作 clicked");
    if (m_currentSchemeIdx < 0) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先新增或选择一个方案"));
        return;
    }
    auto& scheme = ProcessManager::instance().schemes()[m_currentSchemeIdx];

    // ---- 插入目标（P6 §八 第 6 项）：按选中节点在树中的位置决定 ----
    //   选中分组       → 加入该分组（组内并发执行）
    //   选中组内子节点 → 加入其所属分组
    //   选中一级动作   → 插入一级列表其后
    //   未选中         → 追加到一级列表末尾
    ActionData* sel = CurrentNode();
    ActionData* parentGroup = nullptr;
    int insertPos = scheme.nodes.size();
    if (sel) {
        parentGroup = sel->isGroup() ? sel
                                     : ProcessManager::findParentGroup(scheme.nodes, sel->id);
        if (!parentGroup) {
            const int idx = IndexInList(scheme.nodes, sel->id);
            if (idx >= 0) insertPos = idx + 1;
        }
    }

    QDialog dlg(this);
    dlg.setWindowTitle(parentGroup
                           ? QStringLiteral("新建动作（加入分组「%1」）").arg(parentGroup->name)
                           : QStringLiteral("新建动作"));
    dlg.setStyleSheet("background: #1b222b; color: #dbe6f0;");
    auto* form = new QFormLayout(&dlg);
    form->setSpacing(12);
    form->setContentsMargins(20, 20, 20, 20);

    auto* nameEdit = new QLineEdit();
    nameEdit->setPlaceholderText(QStringLiteral("输入动作名称"));
    nameEdit->setStyleSheet("QLineEdit { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; padding: 6px 10px; font-size: 13px; }");
    form->addRow(QStringLiteral("名称:"), nameEdit);

    auto* typeCombo = new QComboBox();
    // ⚠ 本列表的**索引直接就是 ActionType 枚举值**（见下方 static_cast<ActionType>(currentIndex())），
    // 故顺序必须与 ProcessManager.h 的枚举严格一致，新增类型只能**追加在末尾**。
    typeCombo->addItems({ QStringLiteral("移动"), QStringLiteral("识别"), QStringLiteral("挤压"), QStringLiteral("延时"), QStringLiteral("夹爪"), QStringLiteral("视觉引导") });
    typeCombo->setStyleSheet("QComboBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; padding: 6px 10px; font-size: 13px; } QComboBox::drop-down { width: 20px; border: none; } QAbstractItemView { background: #1a222b; outline: none; selection-background-color: #2f6f9f; selection-color: white; color: #dbe6f0; }");
    // ---- 防呆（§八 第 5 项，落点就在这一处 typeCombo）----
    // 按**父分组已占用的资源集合**对冲突类型置灰（§六：按资源互斥，不按类型）。
    // Delay 不占硬件资源，故永不置灰；Move 与 VisionGuide 占同一组轴资源，故互相置灰。
    if (parentGroup) {
        const QStringList used = OccupiedResourceKeys(parentGroup->children);
        auto* model = qobject_cast<QStandardItemModel*>(typeCombo->model());
        int firstEnabled = -1;
        for (int t = 0; t < typeCombo->count(); ++t) {
            const QStringList need = ProcessManager::resourceKeysOf(static_cast<ActionType>(t));
            bool clash = false;
            for (const auto& k : need)
                if (used.contains(k)) { clash = true; break; }
            if (!clash) {
                if (firstEnabled < 0) firstEnabled = t;
                continue;
            }
            if (model) {
                if (auto* it = model->item(t)) {
                    it->setFlags(it->flags() & ~Qt::ItemIsEnabled);
                    it->setToolTip(QStringLiteral("与分组「%1」内已有子动作争用同一资源，不能并发")
                                       .arg(parentGroup->name));
                }
            }
        }
        if (firstEnabled >= 0) typeCombo->setCurrentIndex(firstEnabled);
    }
    form->addRow(QStringLiteral("类型:"), typeCombo);

    if (parentGroup) {
        auto* hint = new QLabel(QStringLiteral(
            "该动作将加入分组「%1」并与组内其它子动作**并发执行**。\n"
            "争用同一硬件资源的类型已置灰（分组不能嵌套）。").arg(parentGroup->name));
        hint->setWordWrap(true);
        hint->setStyleSheet("color: #6a7a8a; font-size: 12px; background: transparent; border: none;");
        form->addRow(hint);
    }

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

    ActionData a;
    a.name = nameEdit->text().trimmed();
    a.type = static_cast<ActionType>(typeCombo->currentIndex());
    // 编号由方案的单调计数器分配（删除不回退，见 ProcessManager::allocateActionId）
    a.id = ProcessManager::allocateActionId(scheme);

    // ---- 组内校验（第二道）：置灰只覆盖资源冲突；数据依赖（识别 × 引用它的视觉引导）
    // 与顺序无关，用「试插一份再整体校验」兜住。校验失败即拒绝，不落盘。
    if (parentGroup) {
        QVector<ActionData> trial = parentGroup->children;
        trial.push_back(a);
        const QString bad = ProcessManager::validateGroupChildren(trial);
        if (!bad.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("提示"),
                QStringLiteral("无法加入分组「%1」：%2").arg(parentGroup->name, bad));
            return;
        }
        parentGroup->children.push_back(a);
    } else {
        scheme.nodes.insert(insertPos, a);
    }
    m_currentNodeId = a.id;
    RefreshNodeTree();
    ProcessManager::instance().save();
    SPDLOG_INFO("[Process] 新增节点 #{}: {} ({}){}", a.id, a.name.toStdString(),
                ProcessManager::actionTypeName(a.type).toStdString(),
                parentGroup ? QStringLiteral(" → 分组 #%1").arg(parentGroup->id).toStdString()
                            : std::string());
}

// 新建分组（P6）：分组只在一级（不能嵌套），组内子动作并发执行。
void ProcessPage::OnNewGroup()
{
    SPDLOG_INFO("[Process] 新建分组 clicked");
    if (m_currentSchemeIdx < 0) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先新增或选择一个方案"));
        return;
    }
    auto& scheme = ProcessManager::instance().schemes()[m_currentSchemeIdx];

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("新建分组"));
    dlg.setStyleSheet("background: #1b222b; color: #dbe6f0;");
    auto* form = new QFormLayout(&dlg);
    form->setSpacing(12);
    form->setContentsMargins(20, 20, 20, 20);

    auto* nameEdit = new QLineEdit(QStringLiteral("并发组_%1").arg(scheme.nodes.size() + 1));
    nameEdit->setStyleSheet("QLineEdit { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; padding: 6px 10px; font-size: 13px; }");
    form->addRow(QStringLiteral("名称:"), nameEdit);

    auto* hint = new QLabel(QStringLiteral(
        "分组内的子动作同时下发、统一汇合（节拍 ≈ 最长子动作）。\n"
        "分组不能嵌套；同一分组内不允许两个子动作争用同一硬件资源。"));
    hint->setWordWrap(true);
    hint->setStyleSheet("color: #6a7a8a; font-size: 12px; background: transparent; border: none;");
    form->addRow(hint);

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
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("分组名称不能为空"));
        return;
    }

    // 插入位置：选中的是一级节点 → 其后；选中的是组内子节点 → 其所属分组之后；未选中 → 末尾。
    // 分组**只在一级**（不能嵌套），故组内子节点只能把新分组放到它所属分组之后。
    ActionData* sel = CurrentNode();
    int insertPos = scheme.nodes.size();
    if (sel) {
        int anchorId = sel->id;
        if (!sel->isGroup()) {
            const ActionData* pg = ProcessManager::findParentGroup(scheme.nodes, sel->id);
            if (pg) anchorId = pg->id;
        }
        const int idx = IndexInList(scheme.nodes, anchorId);
        if (idx >= 0) insertPos = idx + 1;
    }

    ActionData g;
    g.nodeType = NodeType::Group;
    g.name = nameEdit->text().trimmed();
    g.id = ProcessManager::allocateActionId(scheme);   // 分组同样占编号（§7.2）
    scheme.nodes.insert(insertPos, g);

    m_currentNodeId = g.id;
    RefreshNodeTree();
    ProcessManager::instance().save();
    SPDLOG_INFO("[Process] 新建分组 #{}: {}", g.id, g.name.toStdString());
}

void ProcessPage::OnEditAction()
{
    SPDLOG_INFO("[Process] 编辑节点 clicked");
    auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx < 0 || m_currentNodeId <= 0) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先选择一个动作或分组"));
        return;
    }
    ActionData* node = ProcessManager::findNode(schemes[m_currentSchemeIdx].nodes, m_currentNodeId);
    if (!node) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先选择一个动作或分组"));
        return;
    }

    // ---- 分组：只能改名称（编号是引用锚点不可改；分组没有动作类型与参数）----
    if (node->isGroup()) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("编辑分组"),
            QStringLiteral("分组名称:"), QLineEdit::Normal, node->name, &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        node->name = name.trimmed();
        RefreshNodeTree();
        ProcessManager::instance().save();
        SPDLOG_INFO("[Process] 分组 #{} 重命名: {}", node->id, node->name.toStdString());
        return;
    }

    const ActionData backup = *node;   // 快照：校验不通过时回滚（不落盘）
    ActionData* parent = ProcessManager::findParentGroup(schemes[m_currentSchemeIdx].nodes, node->id);

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("编辑动作"));
    dlg.setStyleSheet("background: #1b222b; color: #dbe6f0;");
    auto* form = new QFormLayout(&dlg);
    form->setSpacing(12);
    form->setContentsMargins(20, 20, 20, 20);

    auto* nameEdit = new QLineEdit(node->name);
    nameEdit->setStyleSheet("QLineEdit { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; padding: 6px 10px; font-size: 13px; }");
    form->addRow(QStringLiteral("名称:"), nameEdit);

    auto* typeCombo = new QComboBox();
    // ⚠ 本列表的**索引直接就是 ActionType 枚举值**（见下方 static_cast<ActionType>(currentIndex())），
    // 故顺序必须与 ProcessManager.h 的枚举严格一致，新增类型只能**追加在末尾**。
    typeCombo->addItems({ QStringLiteral("移动"), QStringLiteral("识别"), QStringLiteral("挤压"), QStringLiteral("延时"), QStringLiteral("夹爪"), QStringLiteral("视觉引导") });
    typeCombo->setCurrentIndex(static_cast<int>(node->type));
    typeCombo->setStyleSheet("QComboBox { background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; border-radius: 6px; padding: 6px 10px; font-size: 13px; } QComboBox::drop-down { width: 20px; border: none; } QAbstractItemView { background: #1a222b; outline: none; selection-background-color: #2f6f9f; selection-color: white; color: #dbe6f0; }");
    // 防呆（§八 第 5 项）：在分组内编辑时，按**排除自身后**的已占用资源对冲突类型置灰
    if (parent) {
        const QStringList used = OccupiedResourceKeys(parent->children, node->id);
        auto* model = qobject_cast<QStandardItemModel*>(typeCombo->model());
        for (int t = 0; t < typeCombo->count(); ++t) {
            if (t == static_cast<int>(node->type)) continue;   // 当前类型必须可选（否则无法保持）
            const QStringList need = ProcessManager::resourceKeysOf(static_cast<ActionType>(t));
            bool clash = false;
            for (const auto& k : need)
                if (used.contains(k)) { clash = true; break; }
            if (clash && model) {
                if (auto* it = model->item(t)) {
                    it->setFlags(it->flags() & ~Qt::ItemIsEnabled);
                    it->setToolTip(QStringLiteral("与分组「%1」内其它子动作争用同一资源，不能并发")
                                       .arg(parent->name));
                }
            }
        }
    }
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

    node->name = nameEdit->text().trimmed();
    ActionType newType = static_cast<ActionType>(typeCombo->currentIndex());
    if (newType != node->type) {
        node->type = newType;
        node->points.clear();
        node->visionType.clear(); node->exposure = 0; node->templateName.clear(); node->threshold = 0;
        node->extrudeAmount = 0; node->extrudeSpeed = 0; node->suckBackAmount = 0; node->suckBackSpeed = 0;
        node->delayMs = 0;
        node->isGripperOpen = false;
        node->gripperTarget = 0.0;
        // P5：改了类型，视觉引导的引用与参数一并作废（否则残留引用会指向不存在的语义）
        node->sourceActionId = 0;
        node->offsetZ = 0.0;
        node->guideR = 179.9;
    }
    // 组内校验（第二道）：改类型可能引入资源 / 数据依赖冲突 → 拒绝并回滚
    if (parent) {
        const QString bad = ProcessManager::validateGroupChildren(parent->children);
        if (!bad.isEmpty()) {
            *node = backup;
            QMessageBox::warning(this, QStringLiteral("提示"),
                QStringLiteral("修改被拒绝：%1").arg(bad));
            return;
        }
    }
    RefreshNodeTree();
    ProcessManager::instance().save();
}

void ProcessPage::OnDeleteAction()
{
    SPDLOG_INFO("[Process] 删除节点 clicked");
    auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx < 0 || m_currentNodeId <= 0) return;
    auto& scheme = schemes[m_currentSchemeIdx];
    ActionData* sel = ProcessManager::findNode(scheme.nodes, m_currentNodeId);
    if (!sel) return;

    const QString text = sel->isGroup()
        ? QStringLiteral("确定要删除分组 \"%1\" 吗？\n组内 %2 个子动作会一并删除。\n此操作不可撤销。")
              .arg(sel->name).arg(sel->children.size())
        : QStringLiteral("确定要删除动作 \"%1\" 吗？\n此操作不可撤销。").arg(sel->name);
    if (QMessageBox::question(this, QStringLiteral("删除节点"), text,
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    ActionData* parent = ProcessManager::findParentGroup(scheme.nodes, sel->id);
    QVector<ActionData>& list = parent ? parent->children : scheme.nodes;
    const int idx = IndexInList(list, sel->id);
    if (idx < 0) return;
    list.removeAt(idx);

    // 选中回退：同容器内的相邻节点；容器空了就落到所属分组（组内删空），一级删空则清选中
    if (list.isEmpty())
        m_currentNodeId = parent ? parent->id : 0;
    else
        m_currentNodeId = list[qBound(0, idx, list.size() - 1)].id;

    RefreshNodeTree();
    ProcessManager::instance().save();
}

void ProcessPage::OnAddPoint()
{
    SPDLOG_INFO("[Process] 添加点位 clicked");
    ActionData* node = CurrentNode();
    if (!node || node->isGroup() || node->type != ActionType::Move) return;
    PointData pt;
    pt.name = QStringLiteral("point_%1").arg(node->points.size() + 1);
    pt.posture = QStringLiteral("elbow_up");
    node->points.push_back(pt);
    RefreshNodeDetail(m_currentNodeId);
    ProcessManager::instance().save();
}

void ProcessPage::OnDeletePoint()
{
    SPDLOG_INFO("[Process] 删除点位 clicked");
    ActionData* node = CurrentNode();
    if (!node || node->isGroup() || node->type != ActionType::Move) return;
    const int row = m_pointTable->currentRow();
    if (row < 0 || row >= node->points.size()) return;

    auto reply = QMessageBox::question(this,
        QStringLiteral("删除点位"),
        QStringLiteral("确定要删除点位 \"%1\" 吗？\n此操作不可撤销。").arg(node->points[row].name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    node->points.removeAt(row);
    RefreshNodeDetail(m_currentNodeId);
    ProcessManager::instance().save();
}

void ProcessPage::OnMoveUp()
{
    SPDLOG_INFO("[Process] 上移点位 clicked");
    ActionData* node = CurrentNode();
    if (!node || node->isGroup() || node->type != ActionType::Move) return;
    const int row = m_pointTable->currentRow();
    if (row <= 0) return;
    node->points.swapItemsAt(row, row - 1);
    RefreshNodeDetail(m_currentNodeId);
    m_pointTable->selectRow(row - 1);
    ProcessManager::instance().save();
}

void ProcessPage::OnMoveDown()
{
    SPDLOG_INFO("[Process] 下移点位 clicked");
    ActionData* node = CurrentNode();
    if (!node || node->isGroup() || node->type != ActionType::Move) return;
    const int row = m_pointTable->currentRow();
    if (row < 0 || row >= node->points.size() - 1) return;
    node->points.swapItemsAt(row, row + 1);
    RefreshNodeDetail(m_currentNodeId);
    m_pointTable->selectRow(row + 1);
    ProcessManager::instance().save();
}

void ProcessPage::OnMoveActionUp()
{
    SPDLOG_INFO("[Process] 上移节点 clicked");
    auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx < 0 || m_currentNodeId <= 0) return;
    auto& scheme = schemes[m_currentSchemeIdx];
    ActionData* sel = ProcessManager::findNode(scheme.nodes, m_currentNodeId);
    if (!sel) return;

    // **同级移动**（§八 第 7 项）：组内子节点只在组内换位、一级节点只在一级列表换位。
    // 跨组移动会改变资源独占关系（一个动作从组 A 挪到组 B 可能引入冲突），
    // 必须走「删除 + 重新加入」，由用户显式确认——这里不提供。
    ActionData* parent = ProcessManager::findParentGroup(scheme.nodes, sel->id);
    QVector<ActionData>& list = parent ? parent->children : scheme.nodes;
    const int idx = IndexInList(list, sel->id);
    if (idx <= 0) return;
    list.swapItemsAt(idx, idx - 1);
    RefreshNodeTree();
    ProcessManager::instance().save();
}

void ProcessPage::OnMoveActionDown()
{
    SPDLOG_INFO("[Process] 下移节点 clicked");
    auto& schemes = ProcessManager::instance().schemes();
    if (m_currentSchemeIdx < 0 || m_currentNodeId <= 0) return;
    auto& scheme = schemes[m_currentSchemeIdx];
    ActionData* sel = ProcessManager::findNode(scheme.nodes, m_currentNodeId);
    if (!sel) return;

    ActionData* parent = ProcessManager::findParentGroup(scheme.nodes, sel->id);
    QVector<ActionData>& list = parent ? parent->children : scheme.nodes;
    const int idx = IndexInList(list, sel->id);
    if (idx < 0 || idx >= list.size() - 1) return;
    list.swapItemsAt(idx, idx + 1);
    RefreshNodeTree();
    ProcessManager::instance().save();
}

void ProcessPage::OnTeachRead()
{
    ActionData* node = CurrentNode();
    if (!node || node->isGroup()) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先选择「移动」动作"));
        return;
    }
    if (node->type != ActionType::Move) {
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
    pt.name = QStringLiteral("点_%1").arg(node->points.size() + 1, 3, 10, QLatin1Char('0'));
    pt.x = pose.x;
    pt.y = pose.y;
    pt.z = pose.z;
    pt.r = cur.r;
    pt.posture = QStringLiteral("elbow_up");
    // 示教同时记录关节角（j3=Z 电机坐标，非 coord.z 末端高度），执行时优先按关节角
    // 确定性复现示教姿态（无 IK 双解歧义）；j4=cur.r 与 coord.r 同值冗余记录
    pt.hasJoints = true;
    pt.j1 = cur.j1;
    pt.j2 = cur.j2;
    pt.j3 = cur.z;
    pt.j4 = cur.r;
    node->points.push_back(pt);

    RefreshNodeDetail(m_currentNodeId);
    m_pointTable->selectRow(m_pointTable->rowCount() - 1);
    ProcessManager::instance().save();

    SPDLOG_INFO("[Process] Teach read: J({:.2f},{:.2f},{:.2f},{:.2f}) -> P({:.2f},{:.2f},{:.2f})",
        cur.j1, cur.j2, cur.z, cur.r, pose.x, pose.y, pose.z);
}

void ProcessPage::OnSaveAction()
{
    SPDLOG_INFO("[Process] 保存动作 clicked");
    auto& schemes = ProcessManager::instance().schemes();
    ActionData* node = CurrentNode();
    if (m_currentSchemeIdx < 0 || !node || node->isGroup()) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先选择一个动作"));
        return;
    }
    const ActionData backup = *node;   // 快照：任何一道校验不过都回滚（不落盘，界面保持原样）
    ActionData* parent = ProcessManager::findParentGroup(schemes[m_currentSchemeIdx].nodes, node->id);

    // 保存前按最新轴5 软限位刷新行程输入框范围（config 改限位后无需切换动作也能夹正）
    RefreshGripperLimitRange();

    if (m_speedPercentSpin)
        node->speedPercent = m_speedPercentSpin->value();

    switch (node->type) {
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
            // 关节角继承：坐标与示教存储值一致（容差 0.006 覆盖 2 位小数显示舍入 ≤0.005）
            // → 沿用示教 joints；任一被手改 → 作废（执行回退 IK，避免按旧关节角走到错位）
            if (r < node->points.size() && node->points[r].hasJoints) {
                const auto& old = node->points[r];
                constexpr double kEps = 0.006;
                if (std::fabs(pt.x - old.x) <= kEps && std::fabs(pt.y - old.y) <= kEps &&
                    std::fabs(pt.z - old.z) <= kEps && std::fabs(pt.r - old.r) <= kEps) {
                    pt.hasJoints = true;
                    pt.j1 = old.j1; pt.j2 = old.j2; pt.j3 = old.j3; pt.j4 = old.j4;
                }
            }
            pts.push_back(pt);
        }
        node->points = pts;
        break;
    }
    case ActionType::Vision:
        node->visionType = m_visionTypeCombo->currentText();
        node->exposure = m_exposureEdit->text().toDouble();
        node->templateName = m_templateEdit->text();
        node->threshold = m_thresholdEdit->text().toDouble();
        break;
    case ActionType::Extrude:
        node->extrudeAmount = m_extrudeAmountEdit->text().toDouble();
        node->extrudeSpeed = m_extrudeSpeedEdit->text().toDouble();
        node->suckBackAmount = m_suckBackAmountEdit->text().toDouble();
        node->suckBackSpeed = m_suckBackSpeedEdit->text().toDouble();
        break;
    case ActionType::Delay:
        node->delayMs = m_delaySpin->value();
        break;
    case ActionType::Gripper:
        node->isGripperOpen = (m_gripperCombo->currentIndex() == 1);
        node->gripperTarget = m_gripperTargetSpin->value();
        break;
    case ActionType::VisionGuide: {
        // §4.3 编辑期校验：sourceActionId 必须指向**本方案内存在且类型为 Vision** 的动作。
        // 跨方案引用不合法（决策 2）——下拉只列本方案动作，此处再校验一次防悬垂引用落盘。
        // P6：动作可能在分组内，故**全树扫描**（FindVisionNode）。
        const int src = m_guideSourceCombo->currentData().isValid()
                            ? m_guideSourceCombo->currentData().toInt()
                            : 0;
        const bool refOk = (FindVisionNode(schemes[m_currentSchemeIdx].nodes, src) != nullptr);
        if (!refOk) {
            *node = backup;
            QMessageBox::warning(this, QStringLiteral("提示"),
                src <= 0
                    ? QStringLiteral("请先选择引用的识别动作。\n\n"
                                     "要求：必须是**本方案内**类型为「识别」的动作（不支持跨方案引用）。")
                    : QStringLiteral("引用的识别动作 #%1 不存在或不是「识别」类型，无法保存。\n\n"
                                     "请重新选择（若该识别动作已被删除，说明引用已失效）。").arg(src));
            return;                       // 拒绝保存：不落盘，界面保持原样
        }
        node->sourceActionId = src;
        node->offsetZ = m_guideOffsetZSpin->value();
        node->guideR   = m_guideRSpin->value();
        break;
    }
    }

    // P6 组内校验（第二道）：改类型 / 改引用后可能引入资源或数据依赖冲突 → 拒绝并回滚。
    // 置灰只覆盖「编辑时已存在的资源占用」，而引用改动、跨节点交互只能在此整体校验。
    if (parent) {
        const QString bad = ProcessManager::validateGroupChildren(parent->children);
        if (!bad.isEmpty()) {
            *node = backup;
            QMessageBox::warning(this, QStringLiteral("提示"),
                QStringLiteral("保存被拒绝：%1").arg(bad));
            return;
        }
    }

    RefreshNodeTree();
    ProcessManager::instance().save();
    QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("保存成功"));
    SPDLOG_INFO("[Process] Saved node #{}: {} ({})", node->id, node->name.toStdString(),
        ProcessManager::actionTypeName(node->type).toStdString());
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
