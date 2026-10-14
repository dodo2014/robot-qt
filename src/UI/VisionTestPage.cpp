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

// 可勾选按钮样式（当前仅 `continuousBtn_` 使用）。
// 伪状态特异性：`:hover:enabled`（2 个伪状态）强于 `:checked`（1 个），
// 若不显式补一条勾选态的悬停规则，勾选后悬停会被蓝色顶掉。故补
// `:checked:enabled:hover`（3 个）→ 勾选态悬停保持绿色系。
void SetButtonCheckedStyle(QPushButton* btn)
{
    btn->setStyleSheet(R"(
        QPushButton {
            background: #2f6f9f; color: white; border: none;
            border-radius: 8px; padding: 8px 14px; font-weight: 600; font-size: 13px;
        }
        QPushButton:hover:enabled { background: #3f80b5; }
        QPushButton:checked { background: #27b95c; }
        QPushButton:checked:enabled:hover { background: #2fd06a; }
    )");
}

// 动作按钮样式：`baseColor` 为常态底色，`hoverColor` 为悬停高亮色。
// 二者必须不同：此前 `%1` 与 `%2` 都填了 `baseColor`，悬停色等于常态色，
// 鼠标移上去没有任何视觉反馈（2026-09-21 修复）。
// `:enabled` 限定不可省——`SetParamsLocked` 与 `UpdateButtons` 会用
// `setEnabled(false)` 置灰按钮，缺了限定则置灰按钮悬停反而变亮，
// 产生「看着可点」的误导。
void SetButtonActionStyle(QPushButton* btn, const char* baseColor, const char* hoverColor)
{
    btn->setStyleSheet(QStringLiteral(R"(
        QPushButton {
            background: %1; color: white; border: none;
            border-radius: 8px; padding: 8px 14px; font-weight: 600; font-size: 13px;
        }
        QPushButton:hover:enabled { background: %2; }
    )").arg(QString::fromUtf8(baseColor), QString::fromUtf8(hoverColor)));
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

QPushButton* MakeButton(const QString& text, const char* objectName,
                        const char* color, const char* hoverColor)
{
    auto* b = new QPushButton(text);
    b->setObjectName(QString::fromUtf8(objectName));
    SetButtonActionStyle(b, color, hoverColor);
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

    // 检测工作线程（P1，2026-09-22）：`SimAlgo::Detect` 要对整幅 1920×1080 逐像素扫描
    // （实测 9.4~13.1 ms/次），原先由 `OnFrameReady` 在 UI 线程**同步**跑——每帧一个
    // 10 ms 级不可中断块，拖窗 / 切页 / 切左侧菜单全被顶住（阶段 11B 用例 11.14 偏差的
    // 直接原因）。挪到独立线程后 UI 线程只剩「投递帧 + 收结果」。
    // `detectCtx_` 仅作线程归属载体：走 `QMetaObject::invokeMethod` 的 functor 重载，
    // 不需要 `Q_OBJECT`，因此无需新增源文件（新增文件会触发 CMake reconfigure）。
    detectThread_ = new QThread(this);
    detectCtx_    = new QObject();
    detectCtx_->moveToThread(detectThread_);
    connect(detectThread_, &QThread::finished, detectCtx_, &QObject::deleteLater);
    detectThread_->start();

    RefreshStreamParamsFromConfig();
    ReloadStreamProfiles();
    UpdateCameraState();
    UpdateButtons();
}

VisionTestPage::~VisionTestPage()
{
    // 检测线程先停：`quit()` 会等当前这一拍检测跑完（10 ms 级），`wait` 之后
    // 不可能再有 worker 侧回调访问本对象。回 UI 线程的结果事件以 `this` 为 context，
    // 页面析构时会被 Qt 自动撤销，不会悬垂。
    if (detectThread_) {
        detectThread_->quit();
        detectThread_->wait(2000);
    }
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
        // ⚠ 本控件**没有连接任何槽**（2026-09-22 用例 11.28 实测确认）——与同组的
        // `deviceSnCombo_`（`:394`）、`colorResCombo_`（`:410`）等不同，改它既不写回 config、
        // 也不重建相机实例。`HardwareManager::Initialize` 只在启动时按 `simulation.cameraType`
        // 造一次实例（`HardwareManager.cpp:168`），运行期无「切型号重建」接口。
        // **故切型号必须改 `config/config.json` 后重启软件才生效**（11.28 的前置条件）。
        // 设计文档 `doc/vision_test_page_redesign_plan.md` M5 行（`:76` / `:155`）写明
        // 「切型号需重建实例」，该步尚未实现——补齐需给 HardwareManager 加接口。
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

            refreshDevBtn_ = MakeButton(QStringLiteral("刷新"), "refreshDeviceBtn", "#3a4450", "#4c586a");
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
        startStreamBtn_ = MakeButton(QStringLiteral("开始采集"), "startStreamBtn", "#27b95c", "#2fd06a");
        connect(startStreamBtn_, &QPushButton::clicked, this, &VisionTestPage::OnStartStream);
        btnRow->addWidget(startStreamBtn_);

        stopStreamBtn_ = MakeButton(QStringLiteral("停止采集"), "stopStreamBtn", "#c0524a", "#d4635a");
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
        browseTemplateBtn_ = MakeButton(QStringLiteral("浏览"), "browseTemplateBtn", "#3a4450", "#4c586a");
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
        singleDetectBtn_ = MakeButton(QStringLiteral("单次检测"), "singleDetectBtn", "#2f6f9f", "#3f80b5");
        connect(singleDetectBtn_, &QPushButton::clicked, this, &VisionTestPage::OnSingleDetect);
        algoBtnRow->addWidget(singleDetectBtn_);

        continuousBtn_ = MakeButton(QStringLiteral("连续检测"), "continuousBtn", "#2f6f9f", "#3f80b5");
        continuousBtn_->setCheckable(true);
        SetButtonCheckedStyle(continuousBtn_);
        connect(continuousBtn_, &QPushButton::clicked, this, &VisionTestPage::OnToggleContinuous);
        algoBtnRow->addWidget(continuousBtn_);

        loadImgBtn_ = MakeButton(QStringLiteral("加载本地图片"), "loadImgBtn", "#3a4450", "#4c586a");
        connect(loadImgBtn_, &QPushButton::clicked, this, &VisionTestPage::OnLoadImage);
        algoBtnRow->addWidget(loadImgBtn_);
        v->addLayout(algoBtnRow);

        saveFrameBtn_ = MakeButton(QStringLiteral("保存当前帧"), "saveFrameBtn", "#27b95c", "#2fd06a");
        connect(saveFrameBtn_, &QPushButton::clicked, this, &VisionTestPage::OnSnapshot);
        v->addWidget(saveFrameBtn_);

        layout->addWidget(MakeSection(QStringLiteral("算法测试"), v));
    }

    // ---- 面板底部状态 + 日志目录入口 ----
    statusLabel_ = new QLabel(QStringLiteral("就绪"));
    statusLabel_->setStyleSheet("color: #7f9fc0; font-size: 12px; background: transparent; border: none;");
    statusLabel_->setWordWrap(true);
    layout->addWidget(statusLabel_);

    auto* openLogBtn = MakeButton(QStringLiteral("打开日志目录"), "openLogDirBtn", "#3a4450", "#4c586a");
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
    // ⚠「停止采集」**刻意不受 `locked_` 门禁**（2026-09-23 裁定，用例 11.26，TR-110）：
    // 它只解除本页那一份持有登记，**不碰** `AutoRun` 的持有——`AutoRunPage.cpp:502` 自动运行页
    // 自己独立 `AcquireCamera("AutoRun")`、只在 Idle/Fault 释放（`:791`），且 `SequenceWorker`
    // 取帧是直接 `hw.camera()->CaptureFrame()`（`SequenceWorker.cpp:981`）、不经过本页。
    // 故放行**不会**打断正在执行的识别动作——原设计文档 §6.2 的立论已被代码证伪。
    // 「开始采集」仍受 `locked_` 门禁：自动运行已持流，本页无需（也不应）重复 acquire。
    if (stopStreamBtn_)  stopStreamBtn_->setEnabled(hasCam && iHold);
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
    // P3-4（2026-09-22）：隐藏期间 `OnRenderTick` 整拍早退，三格会停在切走前那一帧 →
    // 切回时补一次强制重绘。按钮态一并刷新（隐藏期间 `UpdateButtons` 也没跑）。
    if (hasFrame_) RenderTiles(/*force=*/true);
    UpdateButtons();
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
    // 链路状态**边沿**补一条文件日志（2026-09-24）：`AppendLog` 只写 UI 页面日志区、
    // **不调 spdlog**（见 `:1432`），而用例 11.36 的判据 `[相机] 断开 / 正在连接/恢复 /
    // 已恢复出图` 需要能在 `log/creampuff_*.log` 中**离线取证**（此前只能目视页面）。
    // 不引入噪声：`Unknown`（SimCamera）已在上方 `return`，且 `HardwareManager::PollCamera`
    // 只在该状态**边沿**广播、首次仅建基线不广播（`HardwareManager.cpp:395-417`）。
    SPDLOG_INFO("[VisionTestPage] [相机] {}", text);
    UpdateCameraState();
    UpdateButtons();
}

// ---------------------------------------------------------------------------
// 帧流与渲染
// ---------------------------------------------------------------------------

void VisionTestPage::OnFrameReady(const CameraFramePtr& frame)
{
    if (!frame) return;
    // §7.2 / D3：只缓存 + 计数，**不在这里渲染**。三格同时刷新会把「每帧同步刷」
    // 的开销 ×3，渲染统一由 renderTimer_（33 ms）驱动。
    // P2：`frame` 是共享只读指针 → 这里只是持有同一份，不再深拷贝整帧（≈14.5 MB）。
    latestFrame_ = frame;
    hasFrame_    = true;
    frameCount_++;

    if (continuous_)
        ScheduleDetection(frame, /*allowAutoSave=*/false);
}

void VisionTestPage::OnRenderTick()
{
    // P3-4（2026-09-22）：本页不可见时整拍早退。
    // `renderTimer_` 与帧信号都是启动即建立的，切到别的页之后本页仍会以 15 fps 做
    // 「6.2 MB 拷贝 + 深度逐像素 + 3 次转位图」——与用户正在看的页面抢 UI 线程。
    // 隐藏期间 `lastRenderedTs_` 不推进，故 `showEvent` 的补绘不会命中去重。
    if (!isVisible()) return;
    UpdateTileInfos();   // 廉价：3 个 setText（FPS 每拍都在变，不该被位图去重挡住）
    RenderTiles();       // 昂贵路径：按帧时间戳 + 目标尺寸去重
    UpdateButtons();     // 33 ms 一次，保证「无画面→有画面」时检测按钮及时解禁
}

void VisionTestPage::UpdateTileInfos()
{
    if (!hasFrame_) return;
    // 深度格显示**深度图自身**尺寸（TR-116，2026-09-23）：D2C 关闭时深度是原生 848×480
    // 而彩色是 1280×720，沿用彩色尺寸会**误导**——用户正是靠这一行判断 D2C 是否生效。
    auto infoText = [this](int w, int h) {
        return QStringLiteral("FPS %1 | %2 | %3x%4")
                   .arg(QString::number(fps_, 'f', 1))
                   .arg(CameraTimeText(latestFrame_->timestamp))
                   .arg(w)
                   .arg(h);
    };
    const int dw = (latestFrame_->depthWidth  > 0) ? latestFrame_->depthWidth  : latestFrame_->width;
    const int dh = (latestFrame_->depthHeight > 0) ? latestFrame_->depthHeight : latestFrame_->height;

    if (rgbInfoLabel_)     rgbInfoLabel_->setText(infoText(latestFrame_->width, latestFrame_->height));
    if (depthInfoLabel_)   depthInfoLabel_->setText(infoText(dw, dh));
    if (overlayInfoLabel_) overlayInfoLabel_->setText(infoText(latestFrame_->width, latestFrame_->height));
}

void VisionTestPage::RenderTiles(bool force)
{
    if (!hasFrame_) return;   // 无帧时保留占位文本

    // 目标尺寸口径与下方 put() 一致（尚未布局时回落 320×200）。
    auto tileSize = [](QLabel* view) {
        if (!view) return QSize(0, 0);
        int w = view->width();
        int h = view->height();
        if (w <= 10) w = 320;
        if (h <= 10) h = 200;
        return QSize(w, h);
    };

    // 去重（P0，2026-09-22）：帧时间戳与三格目标尺寸都没变 → 上一拍画的还是同一张图。
    // 收益两条：① 相机 15 fps 对 30 Hz 渲染定时器，重复帧不再重算（省掉约一半位图开销）；
    // ② 停流后 `hasFrame_` 仍为真（用于保留末帧），但时间戳不再前进 → 负载归零，
    // 不再像修复前那样以 30 Hz 空转重绘静止帧直到程序退出。
    const bool sameSizes = rgbViewLabel_ && depthViewLabel_ && overlayViewLabel_ &&
                           rgbViewLabel_->property("lastScaledSize").toSize()     == tileSize(rgbViewLabel_) &&
                           depthViewLabel_->property("lastScaledSize").toSize()   == tileSize(depthViewLabel_) &&
                           overlayViewLabel_->property("lastScaledSize").toSize() == tileSize(overlayViewLabel_);
    if (!force && sameSizes && latestFrame_->timestamp == lastRenderedTs_)
        return;
    lastRenderedTs_ = latestFrame_->timestamp;

    // P3-3（2026-09-22）：下面用的是**非拥有** QImage 视图，必须保证这一帧活到渲染结束。
    // `OnFrameReady` 与本函数同在 UI 线程、不会交错，仍以局部变量显式固定住——
    // 语义上不依赖「同线程」这个隐含前提。
    const CameraFramePtr frame = latestFrame_;
    const QSize srcSize(frame->width, frame->height);
    // 深度图**可能自带尺寸**（TR-116，2026-09-23）：D2C 关闭时是 848×480 而彩色是 1280×720
    // ⇒ 适配必须按**深度自身**宽高比算，否则会被拉伸到彩色比例（同族：别用彩色尺寸代替深度尺寸）。
    const QSize depthSrcSize((frame->depthWidth  > 0) ? frame->depthWidth  : frame->width,
                             (frame->depthHeight > 0) ? frame->depthHeight : frame->height);

    // 「源尺寸按 KeepAspectRatio 适配到标签」后的实际绘制尺寸。
    // `QSize::scaled` 是纯尺寸计算，不碰像素。
    auto fitSize = [](const QSize& src, const QSize& box) {
        if (src.isEmpty() || box.isEmpty()) return box;
        return src.scaled(box, Qt::KeepAspectRatio);
    };

    // P3-1（2026-09-22）：**先缩放到目标尺寸、再转 QPixmap**。
    // 改造前是 `QPixmap::fromImage(全分辨率图).scaled(...)`——先按 1920×1080 建位图
    // （2.07 M 像素转换 + 8.3 MB 分配）再缩小，三格各来一遍；而三格显示面积合计只有
    // 约 0.39 M 像素，利用率约 1/16。次序换过来后 `fromImage` 只处理缩放后的像素。
    // 视觉无变化：`QImage::scaled` 与 `QPixmap::scaled` 的 `Fast` 路径同源。
    // `Smooth` 仍只在目标尺寸变化时启用（窗口缩放 / 首次布局）。
    auto put = [&tileSize](QLabel* view, const QImage& img, const QSize& fit) {
        if (!view || img.isNull() || fit.isEmpty()) return;
        const QSize target = tileSize(view);
        const bool sizeChanged = view->property("lastScaledSize").toSize() != target;
        view->setProperty("lastScaledSize", target);

        const QImage out = (img.size() == fit)
                               ? img   // 深度图已按目标尺寸产出（P3-2），无需再缩
                               : img.scaled(fit, Qt::IgnoreAspectRatio,
                                            sizeChanged ? Qt::SmoothTransformation
                                                        : Qt::FastTransformation);
        view->setPixmap(QPixmap::fromImage(out));
    };

    const QSize rgbFit = fitSize(srcSize,      tileSize(rgbViewLabel_));
    const QSize depFit = fitSize(depthSrcSize, tileSize(depthViewLabel_));
    const QSize ovlFit = fitSize(srcSize,      tileSize(overlayViewLabel_));

    // P3-3：彩图只转一次、且**零拷贝**——RGB 格与 Overlay 格共用同一份只读视图。
    const QImage rgb = FrameConverter::ColorToQImageView(*frame);
    put(rgbViewLabel_,     rgb,                    rgbFit);
    put(depthViewLabel_,   BuildDepthImage(depFit), depFit);
    put(overlayViewLabel_, BuildOverlayImage(rgb),  ovlFit);
}

QImage VisionTestPage::BuildRgbImage()
{
    if (!hasFrame_) return {};
    return FrameConverter::ColorToQImage(*latestFrame_);
}

QImage VisionTestPage::BuildDepthImage(const QSize& target)
{
    if (!hasFrame_) return {};
    // P3-2：`target` 非空 → 直接产出该尺寸（最近邻采样），不生成 1920×1080 中间图。
    // 本函数**只服务预览格**（伪彩可视化）。存图路径走 `FrameConverter::DepthToQImage16`
    // （原始 16-bit 灰度），**不得**再回落到这里——见 `QueueSaveDepth` 与 TR-125。
    return FrameConverter::DepthToQImage(*latestFrame_, 0.0f, 500.0f, target);
}

QImage VisionTestPage::BuildOverlayImage(const QImage& base)
{
    if (!hasFrame_) return {};
    // `base` 为空表示调用方没现成彩图（如存图路径），自己转一次。
    const QImage src = base.isNull() ? FrameConverter::ColorToQImage(*latestFrame_) : base;
    if (src.isNull()) return src;
    // 阈值在这里只决定**画哪些框**；检测阶段的过滤已在 `SimAlgo::Detect` 内按
    // `EffectiveThreshold()` 完成（P1 / C1）。
    const double thr = ConfigManager::instance().getValue<double>("vision.confidenceThreshold", 0.0);
    // P3（2026-09-22）：无结果可画时直接返回浅拷贝——`DrawOverlays` 里的 `QPainter`
    // 会对图像 `detach()`，那是一次 6.2 MB 深拷贝，白付。
    if (showOverlay_ && !lastResults_.isEmpty()) {
        QImage img = src;   // QImage 隐式共享：此处只是浅拷贝
        FrameConverter::DrawOverlays(img, lastResults_, thr);
        return img;
    }
    return src;
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
    SetStatus(QStringLiteral("检测中…"));
    ScheduleDetection(latestFrame_, /*allowAutoSave=*/true);
}

void VisionTestPage::ScheduleDetection(const CameraFramePtr& frame, bool allowAutoSave)
{
    if (!frame || !detectCtx_) return;

    // 背压（P1）：检测比帧率慢时**只保留最新一帧**，不排队堆积——否则队列越积越长、
    // 结果越拖越旧、内存也跟着涨。单次检测若撞上在途请求，同样按「最新优先」覆盖。
    if (detectBusy_) {
        pendingFrame_    = frame;
        pendingAutoSave_ = allowAutoSave;
        return;
    }

    auto* algo = HardwareManager::instance().algorithm();
    if (!algo) {
        SetStatus(QStringLiteral("算法未注册，无法检测"));
        return;
    }

    // ⚠ 参数快照必须在**主线程**取：`SnapshotParams()` 读 `ConfigManager`（`json` 读写非
    // 线程安全），工作线程绝不读 ConfigManager 是本项目的既有硬约束
    // （见 `SequenceWorker.cpp:106` 的同名约定）。
    const AlgorithmParams params = algo->SnapshotParams();

    detectBusy_ = true;
    QMetaObject::invokeMethod(detectCtx_, [this, frame, params, algo, allowAutoSave]() {
        QElapsedTimer timer;
        timer.start();
        const auto raw = algo->Detect(*frame, params);
        const double ms = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
        const QVector<PuffResult> results = ToQVector(raw);

        // 结果回 UI 线程。以 `this` 为 context：页面析构时该事件被 Qt 自动撤销，不会悬垂。
        QMetaObject::invokeMethod(this, [this, results, ms, allowAutoSave]() {
            ApplyDetectionResult(results, ms, allowAutoSave);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void VisionTestPage::ApplyDetectionResult(const QVector<PuffResult>& results, double elapsedMs, bool allowAutoSave)
{
    detectBusy_ = false;

    const int prevCount = lastResults_.size();
    lastDetectMs_ = elapsedMs;

    ShowResults(results, lastDetectMs_);
    lastResults_ = results;
    // 叠加框需要重绘，但**只在单次检测时强制刷一次**（P0，2026-09-22）：
    // 单次检测的帧已经画过，`RenderTiles()` 的去重会跳过 → 必须 force；
    // 连续检测下每帧时间戳都是新的，33 ms 定时器那一拍自然会带上刚更新的
    // `lastResults_` 重绘，这里再调一次纯属重复（修复前每帧渲染 2~3 次）。
    if (!continuous_)
        RenderTiles(/*force=*/true);

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

    // 忙时暂存的最新一帧（连续检测的背压落点；或单次检测撞上在途请求）→ 立刻补跑。
    // 只补一帧、且只补最新的那一帧，故队列深度恒为 0 或 1。
    if (pendingFrame_) {
        const CameraFramePtr next = pendingFrame_;
        const bool nextAutoSave   = pendingAutoSave_;
        pendingFrame_.reset();
        pendingAutoSave_ = false;
        ScheduleDetection(next, nextAutoSave);
    }
}

void VisionTestPage::ShowResults(const QVector<PuffResult>& results, double elapsedMs)
{
    const int n = results.size();
    if (resultTable_->rowCount() != n)
        resultTable_->setRowCount(n);

    // 增量更新（P1，2026-09-22）：本函数在**连续检测下每帧都跑**，原先每帧
    // `new` 11 × N 个 `QTableWidgetItem`（2 目标 = 22 个堆分配/帧 @ 15 fps）纯属浪费，
    // 且每 `setItem` 都触发一次表格重绘。改为：复用已有 item，仅在文本真的变了才
    // `setText`；批量更新期间关掉表格刷新，最后一次性恢复。
    const QString msText = QString::number(elapsedMs, 'f', 1);
    const bool wasUpdatesEnabled = resultTable_->updatesEnabled();
    resultTable_->setUpdatesEnabled(false);

    for (int i = 0; i < n; ++i) {
        const auto& r = results[i];
        auto set = [&](int col, const QString& text) {
            QTableWidgetItem* item = resultTable_->item(i, col);
            if (!item) {
                item = new QTableWidgetItem;
                item->setTextAlignment(Qt::AlignCenter);
                resultTable_->setItem(i, col, item);
            }
            if (item->text() != text)
                item->setText(text);
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

    resultTable_->setUpdatesEnabled(wasUpdatesEnabled);
}

void VisionTestPage::OnToggleContinuous()
{
    continuous_ = continuousBtn_->isChecked();
    if (!continuous_) {
        // 关掉连续检测 → 丢弃暂存帧，避免「已经关了又冒出一帧结果」。
        // 在途那一拍不打断（10 ms 级，让它跑完即可）。
        pendingFrame_.reset();
        pendingAutoSave_ = false;
    }
    SPDLOG_INFO("[VisionTest] 连续检测 {} clicked", continuous_ ? "开" : "关");
    SetStatus(continuous_ ? QStringLiteral("连续检测已开启（期间不自动存图）")
                          : QStringLiteral("连续检测已关闭"));
}

void VisionTestPage::OnOverlayToggle()
{
    showOverlay_ = overlayToggleCheck_->isChecked();
    SPDLOG_INFO("[VisionTest] 叠加框 {} clicked", showOverlay_ ? "开" : "关");
    // 画面内容没变（帧时间戳不变），但叠加框的有无变了 → 必须强制重绘，
    // 否则会被 `RenderTiles` 的去重挡掉，表现为「勾选框点了没反应」。
    RenderTiles(/*force=*/true);
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
    // ⚠ 存的是**原始 16-bit 深度值（mm）**，不是伪彩图（TR-125，2026-10-14）。
    // 改造前这里调 `BuildDepthImage()`（= `DepthToQImage` 伪彩），落盘的是 8-bit RGB：
    // 深度查看器（如 `DepthViewer.exe`）按 16-bit 灰度读，得到全 0 ⇒ 「保存的深度图没有深度值」。
    // 伪彩渐变不可逆（8-bit 三通道 + 受 depthMin/depthMax 影响），无法从中还原 mm。
    // 预览格仍走 `BuildDepthImage()`——那是给人看的，语义与落盘**刻意不同**。
    QMetaObject::invokeMethod(frameSaver_, "SaveImage", Qt::QueuedConnection,
                              Q_ARG(QImage, FrameConverter::DepthToQImage16(*latestFrame_)),
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

    // P2：离线图同样以共享只读指针缓存，与在线帧走同一条渲染/检测路径。
    latestFrame_ = std::make_shared<CameraFrame>(std::move(f));
    hasFrame_    = true;
    lastResults_.clear();
    SetStatus(QStringLiteral("已加载图片 %1x%2，正在检测…").arg(latestFrame_->width).arg(latestFrame_->height));
    ScheduleDetection(latestFrame_, /*allowAutoSave=*/true);
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
