#include "ProcessPage.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QDebug>
#include <QStyleFactory>
#include <QListView>

ProcessPage::ProcessPage(QWidget* parent)
    : QWidget(parent)
{
    SetupUI();
}

void ProcessPage::SetupUI()
{
    setStyleSheet("background: #262c34;");

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(14, 14, 14, 14);
    mainLayout->setSpacing(12);

    // ==== Top bar ====
    auto* processTop = new QWidget();
    processTop->setStyleSheet("background: #1b222b; padding: 8px 16px; border-radius: 12px;");
    auto* topLayout = new QHBoxLayout(processTop);
    topLayout->setContentsMargins(16, 8, 16, 8);
    topLayout->setSpacing(14);

    auto makeBtnStyle = [](const QString& bg, const QString& hover) {
        return QStringLiteral("QPushButton { background: %1; border: none; border-radius: 8px; padding: 8px 16px; font-weight: 600; font-size: 14px; color: white; } QPushButton:hover { background: %2; }").arg(bg, hover);
    };

    auto* newSchemeBtn = new QPushButton(QStringLiteral("新增方案"));
    newSchemeBtn->setStyleSheet(makeBtnStyle("#2f6f9f", "#3a84b8"));
    newSchemeBtn->setCursor(Qt::PointingHandCursor);
    connect(newSchemeBtn, &QPushButton::clicked, this, &ProcessPage::OnNewScheme);

    auto* currentLabel = new QLabel(QStringLiteral("当前方案:"));
    currentLabel->setStyleSheet("color: #b8cce3; font-size: 13px; font-weight: 500; background: transparent; border: none;");

    auto* schemeInput = new QLineEdit(QStringLiteral("方案_A_2026"));
    schemeInput->setFixedWidth(150);
    schemeInput->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px 8px; border-radius: 6px; font-size: 13px;");

    auto* schemeCombo = new QComboBox();
    schemeCombo->addItems({ QStringLiteral("方案_A"), QStringLiteral("方案_B") });
    schemeCombo->setFixedWidth(130);
    // No per-widget stylesheet — use global QComboBox style from MainWindow

    auto* confirmBtn = new QPushButton(QStringLiteral("确认切换"));
    confirmBtn->setStyleSheet(makeBtnStyle("#2f6f9f", "#3a84b8"));
    confirmBtn->setCursor(Qt::PointingHandCursor);
    connect(confirmBtn, &QPushButton::clicked, this, &ProcessPage::OnConfirmSwitch);

    auto* stepBtn = new QPushButton(QStringLiteral("单步执行"));
    stepBtn->setStyleSheet(makeBtnStyle("#8f7f3f", "#9f8f4f"));
    stepBtn->setCursor(Qt::PointingHandCursor);
    connect(stepBtn, &QPushButton::clicked, this, &ProcessPage::OnStepExecute);

    topLayout->addWidget(newSchemeBtn);
    topLayout->addWidget(currentLabel);
    topLayout->addWidget(schemeInput);
    topLayout->addWidget(schemeCombo);
    topLayout->addWidget(confirmBtn);
    topLayout->addWidget(stepBtn);
    topLayout->addStretch();

    mainLayout->addWidget(processTop);

    // ==== Split: left (action list) + right (point table) ====
    auto* splitWidget = new QWidget();
    splitWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* splitLayout = new QHBoxLayout(splitWidget);
    splitLayout->setContentsMargins(0, 0, 0, 0);
    splitLayout->setSpacing(16);

    // Left: action list
    auto* leftPanel = new QWidget();
    leftPanel->setStyleSheet("background: #1b222b; border-radius: 12px; border: 1px solid #384550;");
    leftPanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(10, 10, 10, 10);
    leftLayout->setSpacing(8);

    auto* actionHeader = new QLabel(QStringLiteral("动作列表"));
    actionHeader->setStyleSheet("color: #b8cce3; font-size: 13px; font-weight: 500; background: transparent; border: none;");

    auto* actionList = new QWidget();
    actionList->setStyleSheet("background: #121a22; border-radius: 8px; padding: 8px;");
    auto* actionLayout = new QVBoxLayout(actionList);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(4);

    QStringList actions = {
        QStringLiteral("[1] 移动到：安全上方点"),
        QStringLiteral("[2] 打开夹爪"),
        QStringLiteral("[3] 等待视觉结果"),
        QStringLiteral("[4] 直线插补：插入灌装口"),
        QStringLiteral("[5] 挤出奶油"),
        QStringLiteral("[6] 关闭夹爪"),
        QStringLiteral("[7] 移动到放置点"),
    };

    for (const auto& action : actions)
    {
        auto* lbl = new QLabel(action);
        lbl->setStyleSheet("color: #c0d6ec; font-family: 'Consolas', monospace; font-size: 14px; background: transparent; border: none; padding: 2px 0;");
        actionLayout->addWidget(lbl);
    }
    actionLayout->addStretch();

    auto* actionBtnRow = new QHBoxLayout();
    actionBtnRow->setSpacing(8);

    auto* newActionBtn = new QPushButton(QStringLiteral("新建动作"));
    newActionBtn->setStyleSheet(makeBtnStyle("#3a6f8f", "#4a7f9f"));
    newActionBtn->setCursor(Qt::PointingHandCursor);
    connect(newActionBtn, &QPushButton::clicked, this, &ProcessPage::OnNewAction);

    auto* deleteActionBtn = new QPushButton(QStringLiteral("删除动作"));
    deleteActionBtn->setStyleSheet(makeBtnStyle("#8f4f4f", "#aa5f5f"));
    deleteActionBtn->setCursor(Qt::PointingHandCursor);
    connect(deleteActionBtn, &QPushButton::clicked, this, &ProcessPage::OnDeleteAction);

    actionBtnRow->addWidget(newActionBtn);
    actionBtnRow->addWidget(deleteActionBtn);
    actionBtnRow->addStretch();

    leftLayout->addWidget(actionHeader);
    leftLayout->addWidget(actionList, 1);
    leftLayout->addLayout(actionBtnRow);

    // Right: point table
    auto* rightPanel = new QWidget();
    rightPanel->setStyleSheet("background: #1b222b; border-radius: 12px; border: 1px solid #384550;");
    rightPanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(10, 10, 10, 10);
    rightLayout->setSpacing(8);

    auto* currentActionLabel = new QLabel(QStringLiteral("当前动作: 直线插补：插入灌装口"));
    currentActionLabel->setStyleSheet("color: #b8cce3; font-size: 13px; font-weight: 500; background: transparent; border: none;");

    auto* pointTable = new QTableWidget(2, 6);
    pointTable->setHorizontalHeaderLabels({
        QStringLiteral("点名称"), QStringLiteral("X"), QStringLiteral("Y"),
        QStringLiteral("Z"), QStringLiteral("R"), QStringLiteral("姿态")
    });
    pointTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    pointTable->setSelectionMode(QAbstractItemView::SingleSelection);
    pointTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    pointTable->verticalHeader()->setVisible(false);
    for (int c = 0; c < 6; ++c)
        pointTable->horizontalHeader()->setSectionResizeMode(c, QHeaderView::Stretch);
    pointTable->setStyleSheet(R"(
        QTableWidget { background: #192029; border: 1px solid #2f3d4d; border-radius: 8px; }
        QTableWidget::item { padding: 4px 6px; color: #cddef0; border-top: 1px solid #2b3542; }
        QHeaderView::section { background: #242e3a; color: #b0c7df; padding: 6px; border: none; }
    )");

    QStringList poseItems = { QStringLiteral("elbow_up"), QStringLiteral("elbow_down") };

    auto makeTableCombo = [](const QStringList& items, int idx) -> QComboBox* {
        auto* c = new QComboBox();
        c->addItems(items);
        c->setCurrentIndex(idx);
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

    struct RowData { QString name; QString x, y, z, r; int pose; };
    QVector<RowData> rows = {
        { QStringLiteral("fill_start"), QStringLiteral("85.0"), QStringLiteral("92.0"), QStringLiteral("22.0"), QStringLiteral("2.0"), 0 },
        { QStringLiteral("fill_end"),   QStringLiteral("85.0"), QStringLiteral("92.0"), QStringLiteral("10.0"), QStringLiteral("2.0"), 1 },
    };

    for (int r = 0; r < rows.size(); ++r) {
        const auto& d = rows[r];
        pointTable->setItem(r, 0, new QTableWidgetItem(d.name));
        pointTable->setItem(r, 1, new QTableWidgetItem(d.x));
        pointTable->setItem(r, 2, new QTableWidgetItem(d.y));
        pointTable->setItem(r, 3, new QTableWidgetItem(d.z));
        pointTable->setItem(r, 4, new QTableWidgetItem(d.r));
        pointTable->setCellWidget(r, 5, makeTableCombo(poseItems, d.pose));
        pointTable->setRowHeight(r, 40);
    }

    auto* pointBtnRow = new QHBoxLayout();
    pointBtnRow->setSpacing(8);

    struct PointBtn { QString text; QString bg; QString hover; void (ProcessPage::*slot)(); };
    QVector<PointBtn> pointBtns = {
        { QStringLiteral("添加点位"),     "#2f6f8f", "#3a84a8", &ProcessPage::OnAddPoint },
        { QStringLiteral("删除选中点"),   "#8f4f4f", "#aa5f5f", &ProcessPage::OnDeletePoint },
        { QStringLiteral("上移"),         "#4f4f6f", "#5f5f8f", &ProcessPage::OnMoveUp },
        { QStringLiteral("下移"),         "#4f4f6f", "#5f5f8f", &ProcessPage::OnMoveDown },
        { QStringLiteral("读取当前坐标(示教)"), "#7f7f3f", "#9f9f4f", &ProcessPage::OnTeachRead },
        { QStringLiteral("保存动作"),     "#2f7f5f", "#3f9f7f", &ProcessPage::OnSaveAction },
    };

    for (const auto& pb : pointBtns)
    {
        auto* btn = new QPushButton(pb.text);
        btn->setStyleSheet(makeBtnStyle(pb.bg, pb.hover));
        btn->setCursor(Qt::PointingHandCursor);
        connect(btn, &QPushButton::clicked, this, pb.slot);
        pointBtnRow->addWidget(btn);
    }
    pointBtnRow->addStretch();

    rightLayout->addWidget(currentActionLabel);
    rightLayout->addWidget(pointTable, 1);
    rightLayout->addLayout(pointBtnRow);

    splitLayout->addWidget(leftPanel, 3);
    splitLayout->addWidget(rightPanel, 7);

    mainLayout->addWidget(splitWidget, 1);
}

void ProcessPage::OnNewScheme()    { qDebug() << "按钮被点击: 新增方案"; }
void ProcessPage::OnConfirmSwitch(){ qDebug() << "按钮被点击: 确认切换"; }
void ProcessPage::OnStepExecute()  { qDebug() << "按钮被点击: 单步执行"; }
void ProcessPage::OnNewAction()    { qDebug() << "按钮被点击: 新建动作"; }
void ProcessPage::OnDeleteAction() { qDebug() << "按钮被点击: 删除动作"; }
void ProcessPage::OnAddPoint()     { qDebug() << "按钮被点击: 添加点位"; }
void ProcessPage::OnDeletePoint()  { qDebug() << "按钮被点击: 删除选中点"; }
void ProcessPage::OnMoveUp()       { qDebug() << "按钮被点击: 上移"; }
void ProcessPage::OnMoveDown()     { qDebug() << "按钮被点击: 下移"; }
void ProcessPage::OnTeachRead()    { qDebug() << "按钮被点击: 读取当前坐标(示教)"; }
void ProcessPage::OnSaveAction()   { qDebug() << "按钮被点击: 保存动作"; }
