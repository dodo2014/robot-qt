#include "VisionTestPage.h"

#include "HAL/core/HardwareManager.h"
#include "HAL/core/HALFactory.h"
#include "HAL/camera/FrameConverter.h"
#include "HAL/camera/FrameSaver.h"
#include "Config/ConfigManager.h"
#include "Config/ConfigPaths.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QTabWidget>
#include <QFileDialog>
#include <QDateTime>
#include <QElapsedTimer>
#include <QThread>
#include <QCoreApplication>
#include <QPixmap>
#include <QHeaderView>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QDesktopServices>
#include <QDir>
#include <QUrl>

#include <algorithm>
#include <cstring>

#include <spdlog/spdlog.h>

#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

namespace {

QString CameraTimeText(int64_t ms)
{
    if (ms <= 0) return QStringLiteral("--");
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

void SetButtonCheckedStyle(QPushButton* btn)
{
    btn->setStyleSheet(R"(
        QPushButton {
            background: #2f6f9f; color: white; border: none;
            border-radius: 8px; padding: 8px 14px; font-weight: 600; font-size: 13px;
        }
        QPushButton:hover { background: #3f80b5; }
        QPushButton:checked { background: #27b95c; }
    )");
}

void SetButtonActionStyle(QPushButton* btn, const char* baseColor)
{
    btn->setStyleSheet(QStringLiteral(R"(
        QPushButton {
            background: %1; color: white; border: none;
            border-radius: 8px; padding: 8px 14px; font-weight: 600; font-size: 13px;
        }
        QPushButton:hover { background: %2; }
    )").arg(QString::fromUtf8(baseColor), QString::fromUtf8(baseColor)));
}

QWidget* MakeSection(const QString& title, QLayout* content)
{
    auto* box = new QGroupBox(title);
    box->setStyleSheet(R"(
        QGroupBox {
            background: #1d252f; border: 1px solid #384652;
            border-radius: 10px; margin-top: 10px; padding: 8px;
        }
        QGroupBox::title {
            subcontrol-origin: margin; left: 10px; padding: 0 4px;
            color: #9bb3cf; font-weight: 600; font-size: 13px;
        }
    )");
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(8, 6, 8, 8);
    v->setSpacing(8);
    v->addLayout(content);
    return box;
}

QLabel* MakeLabel(const QString& text, int minWidth = 62)
{
    auto* l = new QLabel(text);
    l->setStyleSheet(QStringLiteral("color: #b8cce3; min-width: %1px; font-size: 13px; background: transparent; border: none;").arg(minWidth));
    return l;
}

QComboBox* MakeCombo()
{
    auto* c = new QComboBox();
    c->setStyleSheet(R"(
        QComboBox {
            background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0;
            padding: 4px 8px; border-radius: 6px; font-size: 13px;
        }
        QComboBox QAbstractItemView {
            background: #1a2129; color: #dbe6f0; border: none;
            selection-background-color: #2f6f9f;
        }
    )");
    return c;
}

QCheckBox* MakeCheck(const QString& text)
{
    auto* c = new QCheckBox(text);
    c->setStyleSheet("QCheckBox { color: #b8cce3; font-size: 13px; background: transparent; }");
    return c;
}

QPushButton* MakeButton(const QString& text, const char* objectName, const char* color)
{
    auto* b = new QPushButton(text);
    b->setObjectName(QString::fromUtf8(objectName));
    SetButtonActionStyle(b, color);
    return b;
}

// 分辨率串（"1280x720"）按像素总数降序 —— 大的在前，符合直觉
void SortResolutionsDesc(QStringList& list)
{
    std::sort(list.begin(), list.end(), [](const QString& a, const QString& b) {
        const auto pa = a.split(QLatin1Char('x'));
        const auto pb = b.split(QLatin1Char('x'));
        const qlonglong aa = pa.size() == 2 ? pa[0].toLongLong() * pa[1].toLongLong() : 0;
        const qlonglong bb = pb.size() == 2 ? pb[0].toLongLong() * pb[1].toLongLong() : 0;
        return aa > bb;
    });
}

void SortNumbersDesc(QStringList& list)
{
    std::sort(list.begin(), list.end(),
              [](const QString& a, const QString& b) { return a.toInt() > b.toInt(); });
}

// 一个画面格：外框 + 标题（+ 可选复选框）+ 画面 + 右下角叠加信息。
// 三格结构完全同构，抽出来避免三份重复代码（§5.3）。
struct PreviewTile {
    QWidget* frame = nullptr;
    QLabel*  view  = nullptr;
    QLabel*  info  = nullptr;
};

PreviewTile MakePreviewTile(const QString& title, const QString& objectName,
                            const QString& placeholder, bool withToggle,
                            QCheckBox** toggleOut)
{
    PreviewTile t;

    t.frame = new QWidget();
    t.frame->setStyleSheet("background: #1d252f; border: 1px solid #384652; border-radius: 12px;");
    auto* v = new QVBoxLayout(t.frame);
    v->setContentsMargins(8, 6, 8, 8);
    v->setSpacing(6);

    auto* head = new QHBoxLayout();
    head->setSpacing(8);
    auto* titleLabel = new QLabel(title);
    titleLabel->setStyleSheet("color: #9bb3cf; font-weight: 600; font-size: 13px; background: transparent; border: none;");
    head->addWidget(titleLabel);
    head->addStretch();
    if (withToggle) {
        auto* chk = MakeCheck(QStringLiteral("显示识别框"));
        chk->setObjectName(QStringLiteral("overlayToggleCheck"));
        chk->setChecked(true);            // 对应旧 overlayBtn_ 的默认勾选
        if (toggleOut) *toggleOut = chk;
        head->addWidget(chk);
    }
    v->addLayout(head);

    // 画面与角落信息**叠在同一格**：QGridLayout 同一 cell addWidget 两次即可重叠，
    // 后者在上层。省掉绝对定位，窗口缩放时自动跟随（§5.3）。
    auto* stack = new QGridLayout();
    stack->setContentsMargins(0, 0, 0, 0);

    t.view = new QLabel(placeholder);
    t.view->setObjectName(objectName);
    t.view->setAlignment(Qt::AlignCenter);
    t.view->setMinimumSize(240, 150);
    t.view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    t.view->setStyleSheet(R"(
        QLabel {
            background: #0b0d0f; border-radius: 12px;
            border: 1px solid #3a424e; color: #556677; font-size: 15px;
        }
    )");
    stack->addWidget(t.view, 0, 0);

    t.info = new QLabel(QString());
    t.info->setStyleSheet(
        "color: #cfe2f5; font-size: 11px; font-family: 'Consolas', monospace;"
        "background: rgba(11, 13, 15, 170); border: none; border-radius: 6px; padding: 2px 6px;");
    stack->addWidget(t.info, 0, 0, Qt::AlignRight | Qt::AlignBottom);

    v->addLayout(stack, 1);
    return t;
}

}  // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

VisionTestPage::VisionTestPage(QWidget* parent)
    : QWidget(parent)
{
    SetupUI();

    ConfigPaths::ensureConfigLoaded();   // 本页构造早于 config 加载（MainWindow 先 SetupUI 后 Initialize）

    auto camTypes = CameraFactory::Instance().AvailableTypes();
    if (camTypes.empty()) camTypes.push_back("SimCamera");
    for (const auto& t : camTypes)
        cameraTypeCombo_->addItem(QString::fromStdString(t));
    {
        const std::string cur = ConfigManager::instance().getValue<std::string>(
            "simulation.cameraType", std::string("SimCamera"));
        const int idx = cameraTypeCombo_->findText(QString::fromStdString(cur));
        if (idx >= 0) {
            const QSignalBlocker blocker(cameraTypeCombo_);
            cameraTypeCombo_->setCurrentIndex(idx);
        }
    }

    auto algoTypes = PuffAlgorithmFactory::Instance().AvailableTypes();
    if (algoTypes.empty()) algoTypes.push_back("SimAlgo");
    for (const auto& t : algoTypes)
        algoCombo_->addItem(QString::fromStdString(t));

    auto& hw = HardwareManager::instance();
    connect(&hw, &HardwareManager::frameReady, this, &VisionTestPage::OnFrameReady);
    // 相机链路状态边沿（P3）：写进页面日志区，让「拔插线」这类事件在页面上可见
    connect(&hw, &HardwareManager::cameraStateChanged, this, &VisionTestPage::OnCameraStateChanged);

    frameSaver_ = new FrameSaver();
    saverThread_ = new QThread(this);
    frameSaver_->moveToThread(saverThread_);
    connect(saverThread_, &QThread::finished, frameSaver_, &QObject::deleteLater);
    connect(frameSaver_, &FrameSaver::imageSaved, this, &VisionTestPage::OnImageSaved);
    connect(frameSaver_, &FrameSaver::saveError, this, &VisionTestPage::OnSaveError);
    frameSaver_->SetOutputDir(QCoreApplication::applicationDirPath() + QStringLiteral("/saves"));
    saverThread_->start();
    QMetaObject::invokeMethod(frameSaver_, "Start", Qt::QueuedConnection);

    fpsTimer_ = new QTimer(this);
    connect(fpsTimer_, &QTimer::timeout, this, &VisionTestPage::OnFpsTick);
    fpsTimer_->start(1000);

    // 渲染节流（§7.2 / D3）：三格同时刷新会把「每帧同步刷」的开销 ×3。
    // 故 `OnFrameReady` 只缓存 + 计数，三格由本定时器统一刷（33 ms ≈ 30 fps）。
    renderTimer_ = new QTimer(this);
    connect(renderTimer_, &QTimer::timeout, this, &VisionTestPage::OnRenderTick);
    renderTimer_->start(33);

    RefreshStreamParamsFromConfig();
    ReloadStreamProfiles();
    UpdateCameraState();
    UpdateButtons();
}

VisionTestPage::~VisionTestPage()
{
    if (saverThread_) {
        QMetaObject::invokeMethod(frameSaver_, "Stop", Qt::BlockingQueuedConnection);
        saverThread_->quit();
        saverThread_->wait(2000);
    }
}

QVector<PuffResult> VisionTestPage::ToQVector(const std::vector<PuffResult>& v)
{
    QVector<PuffResult> out;
    out.reserve(static_cast<int>(v.size()));
    for (const auto& r : v) out.push_back(r);
    return out;
}

// ---------------------------------------------------------------------------
// 模式互锁
// ---------------------------------------------------------------------------

void VisionTestPage::SetParamsLocked(bool locked)
{
    locked_ = locked;

    // 会改变采集 / 检测行为的控件（§6.2 表格）。采集启停按钮不在此列表——它们由
    // UpdateButtons() 按状态机统一算，locked_ 已在其中生效。
    const QList<QWidget*> params = {
        cameraTypeCombo_, deviceSnCombo_, refreshDevBtn_,
        colorResCombo_, colorFormatCombo_, colorFpsCombo_,
        depthResCombo_, depthFormatCombo_, depthFpsCombo_,
        d2cAlignCheck_,
        algoCombo_, templatePathEdit_, browseTemplateBtn_,
    };
    for (QWidget* w : params)
        if (w) w->setEnabled(!locked);

    UpdateButtons();
}

// ---------------------------------------------------------------------------
// 界面搭建
// ---------------------------------------------------------------------------

void VisionTestPage::SetupUI()
{
    setStyleSheet("background: #262c34;");

    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(12);

    mainLayout->addWidget(CreateControlPanel());
    mainLayout->addWidget(CreateQuadArea(), 1);
}

QWidget* VisionTestPage::CreateControlPanel()
{
    // 固定宽设在 QScrollArea 上（§5.1）：设在内部的 QGroupBox 上会与滚动条打架
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(320);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    auto* content = new QWidget();
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 6, 0);   // 右侧留一点给滚动条
    layout->setSpacing(10);

    // ---- 相机控制 ----
    {
        auto* grid = new QGridLayout();
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(8);
        int row = 0;

        grid->addWidget(MakeLabel(QStringLiteral("相机型号")), row, 0);
        cameraTypeCombo_ = MakeCombo();
        cameraTypeCombo_->setObjectName(QStringLiteral("cameraTypeCombo"));
        grid->addWidget(cameraTypeCombo_, row++, 1);

        // SN 下拉 + 刷新：设备标识的**唯一键**是 `communication.camera.sn`（C2），
        // 读点在 `CameraManager::Open`。刷新经 `EnumerateCameras()` 填真实 SN（S2）。
        grid->addWidget(MakeLabel(QStringLiteral("序列号")), row, 0);
        {
            auto* snRow = new QHBoxLayout();
            snRow->setSpacing(6);
            deviceSnCombo_ = MakeCombo();
            deviceSnCombo_->setObjectName(QStringLiteral("deviceSnCombo"));
            deviceSnCombo_->setEditable(false);
            connect(deviceSnCombo_, &QComboBox::currentTextChanged, this, [](const QString& sn) {
                if (sn.isEmpty()) return;
                ConfigManager::instance().set("communication.camera.sn", sn.toStdString());
            });
            snRow->addWidget(deviceSnCombo_, 1);

            refreshDevBtn_ = MakeButton(QStringLiteral("刷新"), "refreshDeviceBtn", "#3a4450");
            connect(refreshDevBtn_, &QPushButton::clicked, this, &VisionTestPage::OnRefreshDevices);
            snRow->addWidget(refreshDevBtn_);
            grid->addLayout(snRow, row++, 1);
        }

        // 彩色流三件套
        grid->addWidget(MakeLabel(QStringLiteral("彩色分辨率")), row, 0);
        colorResCombo_ = MakeCombo();
        colorResCombo_->setObjectName(QStringLiteral("colorResCombo"));
        connect(colorResCombo_, &QComboBox::currentTextChanged, this, [](const QString& t) {
            const auto pp = t.split(QLatin1Char('x'));
            if (pp.size() != 2) return;
            ConfigManager::instance().set("communication.camera.colorWidth",  pp[0].toInt());
            ConfigManager::instance().set("communication.camera.colorHeight", pp[1].toInt());
        });
        grid->addWidget(colorResCombo_, row++, 1);

        grid->addWidget(MakeLabel(QStringLiteral("彩色格式")), row, 0);
        colorFormatCombo_ = MakeCombo();
        colorFormatCombo_->setObjectName(QStringLiteral("colorFormatCombo"));
        connect(colorFormatCombo_, &QComboBox::currentTextChanged, this, [](const QString& t) {
            if (t.isEmpty()) return;
            ConfigManager::instance().set("communication.camera.colorFormat", t.toStdString());
        });
        grid->addWidget(colorFormatCombo_, row++, 1);

        grid->addWidget(MakeLabel(QStringLiteral("彩色帧率")), row, 0);
        colorFpsCombo_ = MakeCombo();
        colorFpsCombo_->setObjectName(QStringLiteral("colorFpsCombo"));
        connect(colorFpsCombo_, &QComboBox::currentTextChanged, this, [](const QString& t) {
            ConfigManager::instance().set("communication.camera.colorFps", static_cast<double>(t.toInt()));
        });
        grid->addWidget(colorFpsCombo_, row++, 1);

        // 深度流三件套
        grid->addWidget(MakeLabel(QStringLiteral("深度分辨率")), row, 0);
        depthResCombo_ = MakeCombo();
        depthResCombo_->setObjectName(QStringLiteral("depthResCombo"));
        connect(depthResCombo_, &QComboBox::currentTextChanged, this, [](const QString& t) {
            const auto pp = t.split(QLatin1Char('x'));
            if (pp.size() != 2) return;
            ConfigManager::instance().set("communication.camera.depthWidth",  pp[0].toInt());
            ConfigManager::instance().set("communication.camera.depthHeight", pp[1].toInt());
        });
        grid->addWidget(depthResCombo_, row++, 1);

        grid->addWidget(MakeLabel(QStringLiteral("深度格式")), row, 0);
        depthFormatCombo_ = MakeCombo();
        depthFormatCombo_->setObjectName(QStringLiteral("depthFormatCombo"));
        connect(depthFormatCombo_, &QComboBox::currentTextChanged, this, [](const QString& t) {
            if (t.isEmpty()) return;
            ConfigManager::instance().set("communication.camera.depthFormat", t.toStdString());
        });
        grid->addWidget(depthFormatCombo_, row++, 1);

        grid->addWidget(MakeLabel(QStringLiteral("深度帧率")), row, 0);
        depthFpsCombo_ = MakeCombo();
        depthFpsCombo_->setObjectName(QStringLiteral("depthFpsCombo"));
        connect(depthFpsCombo_, &QComboBox::currentTextChanged, this, [](const QString& t) {
            ConfigManager::instance().set("communication.camera.depthFps", static_cast<double>(t.toInt()));
        });
        grid->addWidget(depthFpsCombo_, row++, 1);

        // D2C 对齐（V10）：状态写回 config，开流时由 `CameraManager` 读 config 下发。
        // 不写回就会形成双事实源（复选框 vs `communication.camera.d2cAlign`）。
        d2cAlignCheck_ = MakeCheck(QStringLiteral("D2C 对齐（软件）"));
        d2cAlignCheck_->setObjectName(QStringLiteral("d2cAlignCheck"));
        connect(d2cAlignCheck_, &QCheckBox::toggled, this, [](bool on) {
            ConfigManager::instance().set("communication.camera.d2cAlign",
                                          std::string(on ? "SW" : "OFF"));
        });
        grid->addWidget(d2cAlignCheck_, row++, 0, 1, 2);

        auto* stateRow = new QHBoxLayout();
        cameraStateDot_ = new QLabel();
        cameraStateDot_->setFixedSize(12, 12);
        cameraStateDot_->setStyleSheet("background: #556677; border-radius: 6px;");
        cameraStateText_ = new QLabel(QStringLiteral("相机未打开"));
        cameraStateText_->setStyleSheet("color: #8da3bb; font-size: 13px; background: transparent; border: none;");
        stateRow->addWidget(cameraStateDot_);
        stateRow->addWidget(cameraStateText_);
        stateRow->addStretch();
        grid->addLayout(stateRow, row++, 0, 1, 2);

        // V4 裁定：只留「开始采集 / 停止采集」一对。停止采集时若**没有其他持有者**
        // 则自动关相机（不留占设备的状态）。
        auto* btnRow = new QHBoxLayout();
        btnRow->setSpacing(8);
        startStreamBtn_ = MakeButton(QStringLiteral("开始采集"), "startStreamBtn", "#27b95c");
        connect(startStreamBtn_, &QPushButton::clicked, this, &VisionTestPage::OnStartStream);
        btnRow->addWidget(startStreamBtn_);

        stopStreamBtn_ = MakeButton(QStringLiteral("停止采集"), "stopStreamBtn", "#c0524a");
        connect(stopStreamBtn_, &QPushButton::clicked, this, &VisionTestPage::OnStopStream);
        btnRow->addWidget(stopStreamBtn_);
        grid->addLayout(btnRow, row++, 0, 1, 2);

        layout->addWidget(MakeSection(QStringLiteral("相机控制"), grid));
    }

    // ---- 算法测试 ----
    {
        auto* v = new QVBoxLayout();
        v->setSpacing(8);

        auto* row = new QHBoxLayout();
        row->addWidget(MakeLabel(QStringLiteral("算法引擎")));
        algoCombo_ = MakeCombo();
        algoCombo_->setObjectName(QStringLiteral("algoCombo"));
        row->addWidget(algoCombo_, 1);
        v->addLayout(row);

        // 模板配置（可选填）。⚠ **当前算法接口没有模板参数**——`IPuffAlgorithm` 只有
        // `Detect/LocateBest/LoadConfig`，故本控件目前只做「入口 + 持久化」，由将来的
        // 算法实现消费（需求原文即写「可选填；留空则用算法内置默认值」）。浏览时会在
        // 页面日志里如实说明，不做「看起来在工作」的假象。
        auto* tplRow = new QHBoxLayout();
        tplRow->setSpacing(6);
        tplRow->addWidget(MakeLabel(QStringLiteral("模板配置")));
        templatePathEdit_ = new QLineEdit();
        templatePathEdit_->setObjectName(QStringLiteral("templatePathEdit"));
        templatePathEdit_->setReadOnly(true);
        templatePathEdit_->setPlaceholderText(QStringLiteral("（可选）留空用算法默认值"));
        templatePathEdit_->setStyleSheet(
            "background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0;"
            "padding: 4px 8px; border-radius: 6px; font-size: 12px;");
        tplRow->addWidget(templatePathEdit_, 1);
        browseTemplateBtn_ = MakeButton(QStringLiteral("浏览"), "browseTemplateBtn", "#3a4450");
        connect(browseTemplateBtn_, &QPushButton::clicked, this, &VisionTestPage::OnBrowseTemplate);
        tplRow->addWidget(browseTemplateBtn_);
        v->addLayout(tplRow);

        // 存图三开关（V8 裁定：只在**单次检测**时生效，连续检测期间忽略）
        auto* saveRow = new QHBoxLayout();
        saveRow->setSpacing(10);
        saveRgbCheck_     = MakeCheck(QStringLiteral("存 RGB"));
        saveRgbCheck_->setObjectName(QStringLiteral("saveRgbCheck"));
        saveDepthCheck_   = MakeCheck(QStringLiteral("存 Depth"));
        saveDepthCheck_->setObjectName(QStringLiteral("saveDepthCheck"));
        saveOverlayCheck_ = MakeCheck(QStringLiteral("存 Overlay"));
        saveOverlayCheck_->setObjectName(QStringLiteral("saveOverlayCheck"));
        saveRow->addWidget(saveRgbCheck_);
        saveRow->addWidget(saveDepthCheck_);
        saveRow->addWidget(saveOverlayCheck_);
        saveRow->addStretch();
        v->addLayout(saveRow);

        auto* algoBtnRow = new QHBoxLayout();
        algoBtnRow->setSpacing(8);
        singleDetectBtn_ = MakeButton(QStringLiteral("单次检测"), "singleDetectBtn", "#2f6f9f");
        connect(singleDetectBtn_, &QPushButton::clicked, this, &VisionTestPage::OnSingleDetect);
        algoBtnRow->addWidget(singleDetectBtn_);

        continuousBtn_ = MakeButton(QStringLiteral("连续检测"), "continuousBtn", "#2f6f9f");
        continuousBtn_->setCheckable(true);
        SetButtonCheckedStyle(continuousBtn_);
        connect(continuousBtn_, &QPushButton::clicked, this, &VisionTestPage::OnToggleContinuous);
        algoBtnRow->addWidget(continuousBtn_);

        loadImgBtn_ = MakeButton(QStringLiteral("加载本地图片"), "loadImgBtn", "#3a4450");
        connect(loadImgBtn_, &QPushButton::clicked, this, &VisionTestPage::OnLoadImage);
        algoBtnRow->addWidget(loadImgBtn_);
        v->addLayout(algoBtnRow);

        saveFrameBtn_ = MakeButton(QStringLiteral("保存当前帧"), "saveFrameBtn", "#27b95c");
        connect(saveFrameBtn_, &QPushButton::clicked, this, &VisionTestPage::OnSnapshot);
        v->addWidget(saveFrameBtn_);

        layout->addWidget(MakeSection(QStringLiteral("算法测试"), v));
    }

    // ---- 面板底部状态 + 日志目录入口 ----
    statusLabel_ = new QLabel(QStringLiteral("就绪"));
    statusLabel_->setStyleSheet("color: #7f9fc0; font-size: 12px; background: transparent; border: none;");
    statusLabel_->setWordWrap(true);
    layout->addWidget(statusLabel_);

    auto* openLogBtn = MakeButton(QStringLiteral("打开日志目录"), "openLogDirBtn", "#3a4450");
    connect(openLogBtn, &QPushButton::clicked, this, &VisionTestPage::OnOpenLogDir);
    layout->addWidget(openLogBtn);

    layout->addStretch();
    scroll->setWidget(content);
    return scroll;
}

QWidget* VisionTestPage::CreateQuadArea()
{
    auto* area = new QWidget();
    auto* grid = new QGridLayout(area);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(8);

    PreviewTile rgb = MakePreviewTile(QStringLiteral("RGB 彩色"), QStringLiteral("rgbView"),
                                      QStringLiteral("RGB 画面"), false, nullptr);
    rgbViewLabel_   = rgb.view;
    rgbInfoLabel_   = rgb.info;

    PreviewTile depth = MakePreviewTile(QStringLiteral("深度 伪彩"), QStringLiteral("depthView"),
                                        QStringLiteral("深度画面"), false, nullptr);
    depthViewLabel_ = depth.view;
    depthInfoLabel_ = depth.info;

    PreviewTile overlay = MakePreviewTile(QStringLiteral("Overlay 叠加"), QStringLiteral("overlayView"),
                                          QStringLiteral("识别结果叠加"), true, &overlayToggleCheck_);
    overlayViewLabel_ = overlay.view;
    overlayInfoLabel_ = overlay.info;
    connect(overlayToggleCheck_, &QCheckBox::toggled, this, &VisionTestPage::OnOverlayToggle);

    // 右下格：QTabWidget（V7 裁定）。11 列结果表在 2×2 等分后的宽度下必然横向滚动，
    // 与日志同屏会互相挤；用页签表达「同一格两件事」，且不改变 2×2 的等分结构。
    auto* dataTile = new QWidget();
    dataTile->setStyleSheet("background: #1d252f; border: 1px solid #384652; border-radius: 12px;");
    auto* dv = new QVBoxLayout(dataTile);
    dv->setContentsMargins(8, 6, 8, 8);

    auto* tabs = new QTabWidget();
    tabs->setStyleSheet(R"(
        QTabWidget::pane { border: none; background: transparent; }
        QTabBar::tab {
            background: #111a22; color: #9bb3cf; padding: 5px 12px;
            border-top-left-radius: 6px; border-top-right-radius: 6px; font-size: 12px;
        }
        QTabBar::tab:selected { background: #2f6f9f; color: white; }
    )");

    resultTable_ = new QTableWidget();
    resultTable_->setObjectName(QStringLiteral("resultTable"));
    // V3：保留原有 10 列（进算法阶段仍需 置信度 / U / V / 宽 / 高 做调参定位）
    //     + 追加「耗时 ms」= 11 列
    resultTable_->setColumnCount(11);
    resultTable_->setHorizontalHeaderLabels({
        QStringLiteral("序号"), QStringLiteral("置信度"),
        QStringLiteral("X/mm"), QStringLiteral("Y/mm"), QStringLiteral("Z/mm"),
        QStringLiteral("偏航/°"),
        QStringLiteral("U"), QStringLiteral("V"), QStringLiteral("宽"), QStringLiteral("高"),
        QStringLiteral("耗时ms")
    });
    resultTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    resultTable_->horizontalHeader()->setStretchLastSection(true);
    resultTable_->horizontalHeader()->setDefaultSectionSize(56);
    resultTable_->verticalHeader()->setVisible(false);
    resultTable_->setStyleSheet(
        "QTableWidget { background: #0f151b; color: #dbe6f0; gridline-color: #2b3542; border: none; }"
        "QHeaderView::section { background: #1a2129; color: #9bb3cf; border: none; padding: 3px; }");
    tabs->addTab(resultTable_, QStringLiteral("检测结果"));

    logView_ = new QPlainTextEdit();
    logView_->setObjectName(QStringLiteral("logView"));
    logView_->setReadOnly(true);
    // V6 裁定：页面级环形缓冲。`setMaximumBlockCount` 自带环形上限，
    // **不接 spdlog sink**（那要改 main.cpp 的 logger 配置 + 跨线程 marshal，
    // 与「HAL / 页面不越界」的既有边界不符）。全量日志仍在 log/creampuff_*.log。
    logView_->setMaximumBlockCount(500);
    logView_->setStyleSheet(
        "QPlainTextEdit { background: #0f151b; color: #b8cce3; border: none;"
        "font-family: 'Consolas', monospace; font-size: 11px; }");
    tabs->addTab(logView_, QStringLiteral("运行日志"));

    dv->addWidget(tabs);

    grid->addWidget(rgb.frame,     0, 0);
    grid->addWidget(depth.frame,   0, 1);
    grid->addWidget(overlay.frame, 1, 0);
    grid->addWidget(dataTile,      1, 1);
    grid->setRowStretch(0, 1);
    grid->setRowStretch(1, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);

    return area;
}

// ---------------------------------------------------------------------------
// 状态与按钮
// ---------------------------------------------------------------------------

void VisionTestPage::UpdateCameraState()
{
    auto& hw = HardwareManager::instance();
    // 「本页是否在采集」的真相源是**持有者集合**，不是本地标志——自动运行可能持有流，
    // 而本页只是观察者。
    const bool iHold   = hw.CameraHolders().count("VisionPage") > 0;
    const bool opened  = hw.camera() && hw.camera()->IsOpened();
    const bool anyHold = !hw.CameraHolders().empty();
    streaming_ = iHold;   // 缓存供 OnFpsTick 判断「是否该报帧率异常」

    if (iHold) {
        cameraStateDot_->setStyleSheet("background: #27b95c; border-radius: 6px;");
        cameraStateText_->setText(QStringLiteral("采集运行中"));
    } else if (anyHold) {
        cameraStateDot_->setStyleSheet("background: #f0b429; border-radius: 6px;");
        cameraStateText_->setText(QStringLiteral("其他流程正在使用相机"));
    } else if (opened) {
        cameraStateDot_->setStyleSheet("background: #f0b429; border-radius: 6px;");
        cameraStateText_->setText(QStringLiteral("相机已打开"));
    } else {
        cameraStateDot_->setStyleSheet("background: #556677; border-radius: 6px;");
        cameraStateText_->setText(QStringLiteral("相机未打开"));
    }
}

// 按钮可用性状态机（§6.3）。`locked_`（自动运行中）优先级最高。
void VisionTestPage::UpdateButtons()
{
    auto& hw = HardwareManager::instance();
    const bool hasCam  = hw.camera() != nullptr;
    const bool opened  = hasCam && hw.camera()->IsOpened();
    const bool iHold   = hw.CameraHolders().count("VisionPage") > 0;
    const bool enuming = hasCam && hw.camera()->SupportsDeviceEnumeration();

    // 「断线恢复中」→ 检测类置灰（结果不可信）。SimCamera 恒为 Unknown → 不触发。
    const CameraLinkState st = hw.GetCameraLinkState();
    const bool recovering = (st == CameraLinkState::Disconnected || st == CameraLinkState::Connecting);

    if (startStreamBtn_) startStreamBtn_->setEnabled(hasCam && !locked_ && !iHold);
    if (stopStreamBtn_)  stopStreamBtn_->setEnabled(hasCam && !locked_ && iHold);
    if (singleDetectBtn_) singleDetectBtn_->setEnabled(hasFrame_ && !recovering);
    if (continuousBtn_)  continuousBtn_->setEnabled(opened && !recovering);
    if (refreshDevBtn_)  refreshDevBtn_->setEnabled(enuming && !locked_ && !iHold);
    if (saveFrameBtn_)   saveFrameBtn_->setEnabled(hasFrame_);
    if (loadImgBtn_)     loadImgBtn_->setEnabled(true);   // 离线图检测与相机无关，始终可用
}

// ---------------------------------------------------------------------------
// 流参数：按枚举结果填充 + 从 config 回填
// ---------------------------------------------------------------------------

void VisionTestPage::ReloadStreamProfiles()
{
    auto& hw  = HardwareManager::instance();
    auto& cfg = ConfigManager::instance();
    ConfigPaths::ensureConfigLoaded();

    // 填充：清空 → 按枚举结果加入 → 选中 config 现值。若现值不在列表里则补一项
    // （保证「config 里的有效值」永远可选，不会被悄悄覆盖成别的）。
    auto fill = [](QComboBox* combo, QStringList items, const QString& want) {
        if (!combo) return;
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItems(items);
        int idx = combo->findText(want);
        if (idx < 0 && !want.isEmpty()) {
            combo->addItem(want);
            idx = combo->count() - 1;
        }
        if (idx >= 0) combo->setCurrentIndex(idx);
    };

    const std::vector<CameraStreamProfile> color = hw.GetCameraStreamProfiles(false);
    const std::vector<CameraStreamProfile> depth = hw.GetCameraStreamProfiles(true);

    QStringList colorRes, colorFmt, colorFps, depthRes, depthFmt, depthFps;
    auto collect = [](const std::vector<CameraStreamProfile>& src,
                      QStringList& res, QStringList& fmt, QStringList& fps) {
        for (const auto& p : src) {
            const QString r = QStringLiteral("%1x%2").arg(p.width).arg(p.height);
            if (!res.contains(r)) res << r;
            const QString f = QString::fromStdString(p.format);
            // "?" 是 formatName() 的「未知格式」哨兵，不进下拉框
            if (!f.isEmpty() && f != QLatin1String("?") && !fmt.contains(f)) fmt << f;
            const QString fr = QString::number(static_cast<int>(p.fps + 0.5));
            if (!fps.contains(fr)) fps << fr;
        }
        SortResolutionsDesc(res);
        SortNumbersDesc(fps);
    };
    collect(color, colorRes, colorFmt, colorFps);
    collect(depth, depthRes, depthFmt, depthFps);

    // 枚举不可用（SimCamera / 未接真机）→ 列表为空，此时**只保留 config 现值**
    // （由 fill() 的「补一项」逻辑兜住）。绝不硬编码预置项：需求里的
    // `Y16` 彩色格式、`1920x1080` 分辨率都是**假设**，选到不支持的组合会直接开流失败。
    const int cw = cfg.getValue<int>("communication.camera.colorWidth", 1280);
    const int ch = cfg.getValue<int>("communication.camera.colorHeight", 720);
    const int dw = cfg.getValue<int>("communication.camera.depthWidth", 848);
    const int dh = cfg.getValue<int>("communication.camera.depthHeight", 480);

    fill(colorResCombo_, colorRes, QStringLiteral("%1x%2").arg(cw).arg(ch));
    fill(colorFormatCombo_, colorFmt,
         QString::fromStdString(cfg.getValue<std::string>("communication.camera.colorFormat", std::string())));
    fill(colorFpsCombo_, colorFps,
         QString::number(static_cast<int>(cfg.getValue<double>("communication.camera.colorFps", 30.0) + 0.5)));

    fill(depthResCombo_, depthRes, QStringLiteral("%1x%2").arg(dw).arg(dh));
    fill(depthFormatCombo_, depthFmt,
         QString::fromStdString(cfg.getValue<std::string>("communication.camera.depthFormat", std::string("Y16"))));
    fill(depthFpsCombo_, depthFps,
         QString::number(static_cast<int>(cfg.getValue<double>("communication.camera.depthFps", 30.0) + 0.5)));

    if (!color.empty() || !depth.empty()) {
        AppendLog(QStringLiteral("[相机] profile 枚举：彩色 %1 项 / 深度 %2 项")
                      .arg(color.size()).arg(depth.size()));
    }
}

void VisionTestPage::RefreshStreamParamsFromConfig()
{
    ConfigPaths::ensureConfigLoaded();
    auto& cfg = ConfigManager::instance();

    // SN：config 值不在下拉框里就补进去（刷新设备之前也要能显示当前配置值）
    if (deviceSnCombo_) {
        const std::string sn = cfg.getValue<std::string>("communication.camera.sn", std::string());
        if (!sn.empty()) {
            const QString want = QString::fromStdString(sn);
            const QSignalBlocker blocker(deviceSnCombo_);
            int idx = deviceSnCombo_->findText(want);
            if (idx < 0) {
                deviceSnCombo_->addItem(want);
                idx = deviceSnCombo_->count() - 1;
            }
            deviceSnCombo_->setCurrentIndex(idx);
        }
    }

    // D2C 对齐（V10）
    if (d2cAlignCheck_) {
        const std::string mode = cfg.getValue<std::string>("communication.camera.d2cAlign", std::string("SW"));
        const QSignalBlocker blocker(d2cAlignCheck_);
        d2cAlignCheck_->setChecked(mode == "SW");
    }

    // 模板路径（`vision.templatePath`，本页新增的持久化键）
    if (templatePathEdit_) {
        const std::string tpl = cfg.getValue<std::string>("vision.templatePath", std::string());
        templatePathEdit_->setText(QString::fromStdString(tpl));
    }

    // 6 个流参数 combo 的选中项由 ReloadStreamProfiles() 统一处理（它需要枚举结果）
}

void VisionTestPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // `communication.camera.*` 在「设备配置」页的「通信与连接」标签页里也有写入口 →
    // 每次显示时重新回填，否则切回本页看到的仍是旧值、且下拉框会把配置悄悄覆盖掉。
    RefreshStreamParamsFromConfig();
    ReloadStreamProfiles();
}

// ---------------------------------------------------------------------------
// 相机控制
// ---------------------------------------------------------------------------

void VisionTestPage::OnRefreshDevices()
{
    SPDLOG_INFO("[VisionTest] 刷新设备 clicked");
    auto& hw = HardwareManager::instance();

    if (!hw.camera() || !hw.camera()->SupportsDeviceEnumeration()) {
        // V9 裁定：不支持的实现下按钮本就置灰，这里是双保险
        SetStatus(QStringLiteral("当前相机实现不支持设备枚举"));
        return;
    }

    const std::vector<CameraDeviceInfo> devs = hw.EnumerateCameras();
    if (devs.empty()) {
        SetStatus(QStringLiteral("未枚举到相机设备"));
        return;
    }

    const QString keep = deviceSnCombo_ ? deviceSnCombo_->currentData().toString() : QString();
    if (deviceSnCombo_) {
        const QSignalBlocker blocker(deviceSnCombo_);   // 批量填充期间不写 config
        deviceSnCombo_->clear();
        for (const auto& d : devs) {
            const QString label = d.name.empty()
                ? QString::fromStdString(d.id)
                : QStringLiteral("%1 (%2)").arg(QString::fromStdString(d.id),
                                                QString::fromStdString(d.name));
            // userData 存裸 SN：显示可以带型号，写 config 必须是裸 SN
            deviceSnCombo_->addItem(label, QString::fromStdString(d.id));
        }
        int idx = 0;
        for (int i = 0; i < deviceSnCombo_->count(); ++i) {
            if (deviceSnCombo_->itemData(i).toString() == keep) { idx = i; break; }
        }
        deviceSnCombo_->setCurrentIndex(idx);
    }

    const QString sn = deviceSnCombo_ ? deviceSnCombo_->currentData().toString() : QString();
    if (!sn.isEmpty())
        ConfigManager::instance().set("communication.camera.sn", sn.toStdString());

    // 枚举成功后顺带刷新 profile 列表（下拉框要反映这台设备的真实支持集）
    ReloadStreamProfiles();

    SetStatus(QStringLiteral("枚举到 %1 台设备，当前选择 %2").arg(devs.size()).arg(sn));
    SPDLOG_INFO("[VisionTest] 刷新设备: {} 台，选中 SN='{}'", devs.size(), sn.toStdString());
}

void VisionTestPage::OnStartStream()
{
    SPDLOG_INFO("[VisionTest] 开始采集 clicked");
    auto& hw = HardwareManager::instance();
    if (!hw.camera()) {
        SetStatus(QStringLiteral("相机未注册，无法采集"));
        return;
    }

    // 流参数不再由本页显式传入 —— 它们是 `communication.camera.*` 的权威值
    // （下拉框改动即写回 config），`AcquireCamera` 内部的 `Open(0,0,0)` 会按配置打开。
    //
    // **启流走持有者集合**（§6.1）：流是全局共享资源，本页只是持有者之一；
    // 自动运行若已持流，本页 acquire 只登记、不重复启流。
    if (hw.AcquireCamera("VisionPage")) {
        SetStatus(QStringLiteral("开始采集（holder=VisionPage）"));
    } else {
        SetStatus(QStringLiteral("开始采集失败"));
    }
    UpdateCameraState();
    UpdateButtons();
}

void VisionTestPage::OnStopStream()
{
    SPDLOG_INFO("[VisionTest] 停止采集 clicked");
    auto& hw = HardwareManager::instance();

    hw.ReleaseCamera("VisionPage");

    // 还有别的持有者（典型：自动运行）→ 流仍在跑，**不真停也不关设备**，
    // 且必须如实告知，否则用户会以为「停了」而画面还在动（§2.1 的状态提示要求）。
    if (!hw.CameraHolders().empty()) {
        SetStatus(QStringLiteral("已停止本页采集；其他流程仍在使用相机，流未中断"));
        UpdateCameraState();
        UpdateButtons();
        return;
    }

    // V4 裁定：没有其他持有者 → **自动关相机**（不留「打开着但不采集」占设备的状态）。
    if (hw.camera() && hw.camera()->IsOpened())
        hw.CameraClose();

    SetStatus(QStringLiteral("采集已停止，相机已关闭"));
    UpdateCameraState();
    UpdateButtons();
}

void VisionTestPage::OnCameraStateChanged(CameraLinkState state)
{
    const char* text = "未知";
    switch (state) {
    case CameraLinkState::Disconnected: text = "断开"; break;
    case CameraLinkState::Connecting:   text = "正在连接/恢复"; break;
    case CameraLinkState::Streaming:    text = "已恢复出图"; break;
    case CameraLinkState::Degraded:     text = "帧率异常"; break;
    case CameraLinkState::Unknown:      return;   // SimCamera：不记日志（避免噪声）
    }
    AppendLog(QStringLiteral("[相机] %1").arg(QString::fromUtf8(text)));
    UpdateCameraState();
    UpdateButtons();
}

// ---------------------------------------------------------------------------
// 帧流与渲染
// ---------------------------------------------------------------------------

void VisionTestPage::OnFrameReady(const CameraFrame& frame)
{
    // §7.2 / D3：只缓存 + 计数，**不在这里渲染**。三格同时刷新会把「每帧同步刷」
    // 的开销 ×3，渲染统一由 renderTimer_（33 ms）驱动。
    latestFrame_ = frame;
    hasFrame_    = true;
    frameCount_++;

    if (continuous_)
        RunDetection(frame, /*allowAutoSave=*/false);
}

void VisionTestPage::OnRenderTick()
{
    RenderTiles();
    UpdateButtons();   // 33 ms 一次，保证「无画面→有画面」时检测按钮及时解禁
}

void VisionTestPage::RenderTiles()
{
    if (!hasFrame_) return;   // 无帧时保留占位文本

    const QString info = QStringLiteral("FPS %1 | %2 | %3x%4")
                             .arg(QString::number(fps_, 'f', 1))
                             .arg(CameraTimeText(latestFrame_.timestamp))
                             .arg(latestFrame_.width)
                             .arg(latestFrame_.height);

    auto put = [&info](QLabel* view, QLabel* infoLabel, const QImage& img) {
        if (view && !img.isNull()) {
            int w = view->width();
            int h = view->height();
            if (w <= 10) w = 320;
            if (h <= 10) h = 200;
            const QSize target(w, h);

            // 缩放策略（§7.2）：`SmoothTransformation` 在 1280×720 → 460×280 下开销明显。
            // 折中——**目标尺寸变化时**才用 Smooth 重算（窗口缩放 / 首次布局），
            // 尺寸没变时只是新帧进来、像素数一致，用 Fast 观感差异可忽略。
            const bool sizeChanged = view->property("lastScaledSize").toSize() != target;
            view->setProperty("lastScaledSize", target);

            view->setPixmap(QPixmap::fromImage(img).scaled(
                target, Qt::KeepAspectRatio,
                sizeChanged ? Qt::SmoothTransformation : Qt::FastTransformation));
        }
        if (infoLabel) infoLabel->setText(info);
    };

    put(rgbViewLabel_,     rgbInfoLabel_,     BuildRgbImage());
    put(depthViewLabel_,   depthInfoLabel_,   BuildDepthImage());
    put(overlayViewLabel_, overlayInfoLabel_, BuildOverlayImage());
}

QImage VisionTestPage::BuildRgbImage()
{
    if (!hasFrame_) return {};
    return FrameConverter::ColorToQImage(latestFrame_);
}

QImage VisionTestPage::BuildDepthImage()
{
    if (!hasFrame_) return {};
    return FrameConverter::DepthToQImage(latestFrame_, 0.0f, 500.0f);
}

QImage VisionTestPage::BuildOverlayImage()
{
    if (!hasFrame_) return {};
    QImage img = FrameConverter::ColorToQImage(latestFrame_);
    if (img.isNull()) return img;
    if (showOverlay_) {
        // 阈值在这里只决定**画哪些框**；检测阶段的过滤已在 `SimAlgo::Detect` 内按
        // `EffectiveThreshold()` 完成（P1 / C1）。
        const double thr = ConfigManager::instance().getValue<double>("vision.confidenceThreshold", 0.0);
        FrameConverter::DrawOverlays(img, lastResults_, thr);
    }
    return img;
}

// ---------------------------------------------------------------------------
// 检测
// ---------------------------------------------------------------------------

void VisionTestPage::OnSingleDetect()
{
    SPDLOG_INFO("[VisionTest] 单次检测 clicked");
    if (!hasFrame_) {
        SetStatus(QStringLiteral("无画面可检测，请先开始采集或加载图片"));
        return;
    }
    RunDetection(latestFrame_, /*allowAutoSave=*/true);
}

void VisionTestPage::RunDetection(const CameraFrame& frame, bool allowAutoSave)
{
    auto* algo = HardwareManager::instance().algorithm();
    if (!algo) {
        SetStatus(QStringLiteral("算法未注册，无法检测"));
        return;
    }

    const int prevCount = lastResults_.size();

    QElapsedTimer timer;
    timer.start();
    auto results = ToQVector(algo->Detect(frame));
    lastDetectMs_ = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;

    ShowResults(results, lastDetectMs_);
    lastResults_ = results;
    RenderTiles();

    // 耗时进日志（§7.3）。连续检测下**只在目标数变化时记**——否则 30 fps × 每秒 30 行
    // 会把 500 行的环形缓冲在十几秒内冲干净，反而看不到关键事件。
    const bool countChanged = (results.size() != prevCount);
    if (!continuous_ || countChanged) {
        AppendLog(QStringLiteral("[检测] 目标 %1 个，耗时 %2 ms")
                      .arg(results.size())
                      .arg(QString::number(lastDetectMs_, 'f', 1)));
    }

    // V8 裁定：三开关只在**单次检测**（含离线图检测）时生效；连续检测期间忽略自动存图。
    // 否则 30 fps × 3 路 = 90 张/秒 → 秒级爆盘，且必然命中 FrameSaver 的 D4/D5。
    if (allowAutoSave) {
        if (saveRgbCheck_ && saveRgbCheck_->isChecked())         QueueSaveRgb();
        if (saveDepthCheck_ && saveDepthCheck_->isChecked())     QueueSaveDepth();
        if (saveOverlayCheck_ && saveOverlayCheck_->isChecked()) QueueSaveOverlay();
    }

    if (results.empty())
        SetStatus(QStringLiteral("检测完成：未找到目标（%1 ms）").arg(QString::number(lastDetectMs_, 'f', 1)));
    else
        SetStatus(QStringLiteral("检测完成：发现 %1 个目标（%2 ms）")
                      .arg(results.size())
                      .arg(QString::number(lastDetectMs_, 'f', 1)));
}

void VisionTestPage::ShowResults(const QVector<PuffResult>& results, double elapsedMs)
{
    resultTable_->setRowCount(results.size());
    const QString msText = QString::number(elapsedMs, 'f', 1);
    for (int i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        auto set = [&](int col, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setTextAlignment(Qt::AlignCenter);
            resultTable_->setItem(i, col, item);
        };
        set(0, QString::number(i + 1));
        set(1, QString::number(r.confidence * 100.0, 'f', 1) + QStringLiteral("%"));
        set(2, QString::number(r.x, 'f', 1));
        set(3, QString::number(r.y, 'f', 1));
        set(4, QString::number(r.z, 'f', 1));
        set(5, QString::number(r.yaw, 'f', 1));
        set(6, QString::number(r.pixelU));
        set(7, QString::number(r.pixelV));
        set(8, QString::number(r.width));
        set(9, QString::number(r.height));
        set(10, msText);   // 第 11 列：本次检测的整体耗时（V3）
    }
}

void VisionTestPage::OnToggleContinuous()
{
    continuous_ = continuousBtn_->isChecked();
    SPDLOG_INFO("[VisionTest] 连续检测 {} clicked", continuous_ ? "开" : "关");
    SetStatus(continuous_ ? QStringLiteral("连续检测已开启（期间不自动存图）")
                          : QStringLiteral("连续检测已关闭"));
}

void VisionTestPage::OnOverlayToggle()
{
    showOverlay_ = overlayToggleCheck_->isChecked();
    SPDLOG_INFO("[VisionTest] 叠加框 {} clicked", showOverlay_ ? "开" : "关");
    RenderTiles();
}

void VisionTestPage::OnBrowseTemplate()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择模板配置文件"), QString(), QStringLiteral("配置 (*.json);;所有文件 (*)"));
    if (path.isEmpty()) return;

    templatePathEdit_->setText(path);
    ConfigManager::instance().set("vision.templatePath", path.toStdString());
    SPDLOG_INFO("[VisionTest] 模板配置 path={}", path.toStdString());
    // 如实说明：算法接口（`IPuffAlgorithm`）目前没有模板参数，本控件只做入口 + 持久化
    SetStatus(QStringLiteral("模板路径已保存到 vision.templatePath（当前算法实现尚未消费）"));
}

// ---------------------------------------------------------------------------
// 存图
// ---------------------------------------------------------------------------

void VisionTestPage::QueueSaveRgb()
{
    if (!hasFrame_) return;
    QMetaObject::invokeMethod(frameSaver_, "SaveImage", Qt::QueuedConnection,
                              Q_ARG(QImage, BuildRgbImage()),
                              Q_ARG(QString, QStringLiteral("rgb")));
}

void VisionTestPage::QueueSaveDepth()
{
    if (!hasFrame_) return;
    QMetaObject::invokeMethod(frameSaver_, "SaveImage", Qt::QueuedConnection,
                              Q_ARG(QImage, BuildDepthImage()),
                              Q_ARG(QString, QStringLiteral("depth")));
}

void VisionTestPage::QueueSaveOverlay()
{
    if (!hasFrame_) return;
    QMetaObject::invokeMethod(frameSaver_, "SaveImage", Qt::QueuedConnection,
                              Q_ARG(QImage, BuildOverlayImage()),
                              Q_ARG(QString, QStringLiteral("overlay")));
}

void VisionTestPage::OnSnapshot()
{
    SPDLOG_INFO("[VisionTest] 保存当前帧 clicked");
    if (!hasFrame_) {
        SetStatus(QStringLiteral("当前无画面，无法保存"));
        return;
    }

    // 按三开关落盘；三开关全未勾选时兜底存 RGB（按钮不能点了没反应）
    const bool any = (saveRgbCheck_ && saveRgbCheck_->isChecked())
                  || (saveDepthCheck_ && saveDepthCheck_->isChecked())
                  || (saveOverlayCheck_ && saveOverlayCheck_->isChecked());
    if (!any) {
        QueueSaveRgb();
        SetStatus(QStringLiteral("三路存图开关均未勾选，已按 RGB 保存当前帧"));
        return;
    }
    if (saveRgbCheck_ && saveRgbCheck_->isChecked())         QueueSaveRgb();
    if (saveDepthCheck_ && saveDepthCheck_->isChecked())     QueueSaveDepth();
    if (saveOverlayCheck_ && saveOverlayCheck_->isChecked()) QueueSaveOverlay();
    SetStatus(QStringLiteral("当前帧已加入保存队列"));
}

void VisionTestPage::OnLoadImage()
{
    SPDLOG_INFO("[VisionTest] 加载离线图片 clicked");
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择离线图片"),
        QString(), QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp)"));
    if (path.isEmpty()) return;
    SPDLOG_INFO("[VisionTest] 加载离线图片 path={}", path.toStdString());

    QImage raw(path);
    if (raw.isNull()) {
        SetStatus(QStringLiteral("无法加载图片: %1").arg(path));
        return;
    }
    const QImage img = raw.convertToFormat(QImage::Format_RGB888);

    CameraFrame f;
    f.width  = img.width();
    f.height = img.height();
    f.timestamp = QDateTime::currentMSecsSinceEpoch();
    f.colorData.resize(static_cast<size_t>(f.width) * f.height * 3);
    for (int y = 0; y < f.height; ++y)
        std::memcpy(f.colorData.data() + static_cast<size_t>(y) * f.width * 3,
                    img.scanLine(y),
                    static_cast<size_t>(f.width) * 3);

    latestFrame_ = f;
    hasFrame_    = true;
    lastResults_.clear();
    SetStatus(QStringLiteral("已加载图片 %1x%2，正在检测…").arg(f.width).arg(f.height));
    RunDetection(latestFrame_, /*allowAutoSave=*/true);
}

// ---------------------------------------------------------------------------
// 定时器 / 回调 / 日志
// ---------------------------------------------------------------------------

void VisionTestPage::OnFpsTick()
{
    fps_ = frameCount_;
    frameCount_ = 0;

    // 帧率异常（§7.3）：低于 2 fps 视为流异常（与 camera_integration_plan 的判据一致）
    if (hasFrame_ && streaming_ && fps_ > 0.0 && fps_ < 2.0)
        AppendLog(QStringLiteral("[帧率] %1 fps（低于 2 fps 阈值）").arg(QString::number(fps_, 'f', 1)));
}

void VisionTestPage::OnImageSaved(const QString& path)
{
    SetStatus(QStringLiteral("已保存: %1").arg(path));
}

void VisionTestPage::OnSaveError(const QString& message)
{
    SetStatus(message);
}

void VisionTestPage::OnOpenLogDir()
{
    // 与 spdlog 落盘目录**同一实现**（`ConfigPaths::logDir()`，P4 从 main.cpp 迁入），
    // 避免在页面里重写一遍判定规则造出第二套（C5 的教训）。
    QString reason;
    const QString dir = ConfigPaths::logDir(&reason);
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    SetStatus(QStringLiteral("已打开日志目录: %1（%2）").arg(dir, reason));
}

void VisionTestPage::AppendLog(const QString& text)
{
    if (!logView_) return;
    logView_->appendPlainText(QStringLiteral("[%1] %2")
                                  .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                                       text));
}

void VisionTestPage::SetStatus(const QString& text)
{
    if (statusLabel_) statusLabel_->setText(QStringLiteral("● %1").arg(text));
    AppendLog(text);
    SPDLOG_INFO("[VisionTestPage] {}", text.toStdString());
}
