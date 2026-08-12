#include "VisionTestPage.h"

#include "HAL/core/HardwareManager.h"
#include "HAL/core/HALFactory.h"
#include "HAL/camera/FrameConverter.h"
#include "HAL/camera/FrameSaver.h"
#include "Config/ConfigManager.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QDateTime>
#include <QThread>
#include <QCoreApplication>
#include <QPixmap>
#include <QDebug>
#include <QHeaderView>

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

}

VisionTestPage::VisionTestPage(QWidget* parent)
    : QWidget(parent)
{
    SetupUI();

    auto camTypes = CameraFactory::Instance().AvailableTypes();
    if (camTypes.empty()) camTypes.push_back("SimCamera");
    for (const auto& t : camTypes)
        cameraTypeCombo_->addItem(QString::fromStdString(t));

    auto algoTypes = PuffAlgorithmFactory::Instance().AvailableTypes();
    if (algoTypes.empty()) algoTypes.push_back("SimAlgo");
    for (const auto& t : algoTypes)
        algoCombo_->addItem(QString::fromStdString(t));

    auto& hw = HardwareManager::instance();

    connect(&hw, &HardwareManager::frameReady, this, &VisionTestPage::OnFrameReady);

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

void VisionTestPage::SetupUI()
{
    setStyleSheet("background: #262c34;");

    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(12);

    mainLayout->addWidget(CreateControlPanel());
    mainLayout->addWidget(CreatePreviewArea(), 1);
}

QWidget* VisionTestPage::CreateControlPanel()
{
    auto* panel = new QWidget();
    panel->setFixedWidth(330);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    // ---- 相机控制 ----
    {
        auto* grid = new QGridLayout();
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(8);

        grid->addWidget(MakeLabel(QStringLiteral("相机型号")), 0, 0);
        cameraTypeCombo_ = MakeCombo();
        grid->addWidget(cameraTypeCombo_, 0, 1);

        grid->addWidget(MakeLabel(QStringLiteral("序列号")), 1, 0);
        deviceIdEdit_ = new QLineEdit();
        deviceIdEdit_->setText(QStringLiteral("CAM-SIM-001"));
        deviceIdEdit_->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px 8px; border-radius: 6px; font-size: 13px;");
        grid->addWidget(deviceIdEdit_, 1, 1);

        grid->addWidget(MakeLabel(QStringLiteral("分辨率")), 2, 0);
        resolutionCombo_ = MakeCombo();
        resolutionCombo_->addItems({ QStringLiteral("640x480"), QStringLiteral("1280x720") });
        grid->addWidget(resolutionCombo_, 2, 1);

        grid->addWidget(MakeLabel(QStringLiteral("帧率")), 3, 0);
        fpsCombo_ = MakeCombo();
        fpsCombo_->addItems({ QStringLiteral("15"), QStringLiteral("30"), QStringLiteral("60") });
        fpsCombo_->setCurrentIndex(1);
        grid->addWidget(fpsCombo_, 3, 1);

        auto* stateRow = new QHBoxLayout();
        cameraStateDot_ = new QLabel();
        cameraStateDot_->setFixedSize(12, 12);
        cameraStateDot_->setStyleSheet("background: #556677; border-radius: 6px;");
        cameraStateText_ = new QLabel(QStringLiteral("相机未打开"));
        cameraStateText_->setStyleSheet("color: #8da3bb; font-size: 13px; background: transparent; border: none;");
        stateRow->addWidget(cameraStateDot_);
        stateRow->addWidget(cameraStateText_);
        stateRow->addStretch();
        grid->addLayout(stateRow, 4, 0, 1, 2);

        auto* btnRow = new QHBoxLayout();
        btnRow->setSpacing(8);
        auto* openBtn = new QPushButton(QStringLiteral("打开"));
        openBtn->setObjectName("openCamBtn");
        SetButtonActionStyle(openBtn, "#2f6f9f");
        connect(openBtn, &QPushButton::clicked, this, &VisionTestPage::OnOpenCamera);
        btnRow->addWidget(openBtn);

        auto* closeBtn = new QPushButton(QStringLiteral("关闭"));
        closeBtn->setObjectName("closeCamBtn");
        SetButtonActionStyle(closeBtn, "#3a4450");
        connect(closeBtn, &QPushButton::clicked, this, &VisionTestPage::OnCloseCamera);
        btnRow->addWidget(closeBtn);

        auto* startBtn = new QPushButton(QStringLiteral("开始采集"));
        startBtn->setObjectName("startStreamBtn");
        SetButtonActionStyle(startBtn, "#27b95c");
        connect(startBtn, &QPushButton::clicked, this, &VisionTestPage::OnStartStream);
        btnRow->addWidget(startBtn);

        auto* stopBtn = new QPushButton(QStringLiteral("停止"));
        stopBtn->setObjectName("stopStreamBtn");
        SetButtonActionStyle(stopBtn, "#c0524a");
        connect(stopBtn, &QPushButton::clicked, this, &VisionTestPage::OnStopStream);
        btnRow->addWidget(stopBtn);

        grid->addLayout(btnRow, 5, 0, 1, 2);
        layout->addWidget(MakeSection(QStringLiteral("相机控制"), grid));
    }

    // ---- 算法测试 ----
    {
        auto* v = new QVBoxLayout();
        v->setSpacing(8);

        auto* row = new QHBoxLayout();
        row->addWidget(MakeLabel(QStringLiteral("算法型号")));
        algoCombo_ = MakeCombo();
        row->addWidget(algoCombo_, 1);
        v->addLayout(row);

        auto* algoBtnRow = new QHBoxLayout();
        algoBtnRow->setSpacing(8);
        auto* detectBtn = new QPushButton(QStringLiteral("单次检测"));
        detectBtn->setObjectName("singleDetectBtn");
        SetButtonActionStyle(detectBtn, "#2f6f9f");
        connect(detectBtn, &QPushButton::clicked, this, &VisionTestPage::OnSingleDetect);
        algoBtnRow->addWidget(detectBtn);

        continuousBtn_ = new QPushButton(QStringLiteral("连续检测"));
        continuousBtn_->setObjectName("continuousBtn");
        continuousBtn_->setCheckable(true);
        SetButtonCheckedStyle(continuousBtn_);
        connect(continuousBtn_, &QPushButton::clicked, this, &VisionTestPage::OnToggleContinuous);
        algoBtnRow->addWidget(continuousBtn_);

        auto* loadImgBtn = new QPushButton(QStringLiteral("加载图片"));
        loadImgBtn->setObjectName("loadImgBtn");
        SetButtonActionStyle(loadImgBtn, "#3a4450");
        connect(loadImgBtn, &QPushButton::clicked, this, &VisionTestPage::OnLoadImage);
        algoBtnRow->addWidget(loadImgBtn);
        v->addLayout(algoBtnRow);

        resultTable_ = new QTableWidget();
        resultTable_->setColumnCount(10);
        resultTable_->setHorizontalHeaderLabels({
            QStringLiteral("序号"), QStringLiteral("置信度"),
            QStringLiteral("X/mm"), QStringLiteral("Y/mm"), QStringLiteral("Z/mm"),
            QStringLiteral("偏航/°"),
            QStringLiteral("U"), QStringLiteral("V"), QStringLiteral("宽"), QStringLiteral("高")
        });
        resultTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        resultTable_->horizontalHeader()->setStretchLastSection(true);
        resultTable_->horizontalHeader()->setDefaultSectionSize(56);
        resultTable_->verticalHeader()->setVisible(false);
        resultTable_->setMaximumHeight(190);
        v->addWidget(resultTable_);

        layout->addWidget(MakeSection(QStringLiteral("算法测试"), v), 1);
    }

    // ---- 参数 ----
    {
        auto* grid = new QGridLayout();
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(8);

        auto makeSpin = [](double min, double max, double step, double val) {
            auto* s = new QDoubleSpinBox();
            s->setRange(min, max);
            s->setDecimals(2);
            s->setSingleStep(step);
            s->setValue(val);
            s->setStyleSheet("background: #111a22; border: 1px solid #3f4e5e; color: #dbe6f0; padding: 4px 8px; border-radius: 6px; font-size: 13px;");
            return s;
        };

        auto& cfg = ConfigManager::instance();

        grid->addWidget(MakeLabel(QStringLiteral("置信度阈值")), 0, 0);
        confSpin_ = makeSpin(0.0, 1.0, 0.01, cfg.getValue<double>("vision.confidenceThreshold", 0.85));
        connect(confSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [](double v) {
            ConfigManager::instance().set("vision.confidenceThreshold", v);
        });
        grid->addWidget(confSpin_, 0, 1);

        grid->addWidget(MakeLabel(QStringLiteral("深度 Z min")), 1, 0);
        zminSpin_ = makeSpin(0.0, 5000.0, 1.0, cfg.getValue<double>("vision.depthZMin", 10.0));
        connect(zminSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [](double v) {
            ConfigManager::instance().set("vision.depthZMin", v);
        });
        grid->addWidget(zminSpin_, 1, 1);

        grid->addWidget(MakeLabel(QStringLiteral("深度 Z max")), 2, 0);
        zmaxSpin_ = makeSpin(0.0, 5000.0, 1.0, cfg.getValue<double>("vision.depthZMax", 500.0));
        connect(zmaxSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [](double v) {
            ConfigManager::instance().set("vision.depthZMax", v);
        });
        grid->addWidget(zmaxSpin_, 2, 1);

        grid->addWidget(MakeLabel(QStringLiteral("曝光")), 3, 0);
        exposureSpin_ = makeSpin(1.0, 10000.0, 50.0, cfg.getValue<double>("vision.exposure", 500.0));
        connect(exposureSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [](double v) {
            ConfigManager::instance().set("vision.exposure", v);
        });
        grid->addWidget(exposureSpin_, 3, 1);

        layout->addWidget(MakeSection(QStringLiteral("参数"), grid));
    }

    statusLabel_ = new QLabel(QStringLiteral("就绪"));
    statusLabel_->setStyleSheet("color: #7f9fc0; font-size: 12px; background: transparent; border: none;");
    statusLabel_->setWordWrap(true);
    layout->addWidget(statusLabel_);

    return panel;
}

QWidget* VisionTestPage::CreatePreviewArea()
{
    auto* area = new QWidget();
    auto* layout = new QVBoxLayout(area);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    previewLabel_ = new QLabel(QStringLiteral("相机画面将显示在此处"));
    previewLabel_->setAlignment(Qt::AlignCenter);
    previewLabel_->setMinimumSize(640, 420);
    previewLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    previewLabel_->setStyleSheet(R"(
        QLabel {
            background: #0b0d0f; border-radius: 12px;
            border: 1px solid #3a424e; color: #556677; font-size: 16px;
        }
    )");

    layout->addWidget(previewLabel_, 1);

    auto* bar = new QHBoxLayout();
    bar->setSpacing(10);

    rgbBtn_ = new QPushButton(QStringLiteral("RGB"));
    rgbBtn_->setObjectName("rgbBtn");
    rgbBtn_->setCheckable(true);
    rgbBtn_->setChecked(true);
    SetButtonCheckedStyle(rgbBtn_);
    connect(rgbBtn_, &QPushButton::clicked, this, &VisionTestPage::OnShowRGB);
    bar->addWidget(rgbBtn_);

    depthBtn_ = new QPushButton(QStringLiteral("深度"));
    depthBtn_->setObjectName("depthBtn");
    depthBtn_->setCheckable(true);
    SetButtonCheckedStyle(depthBtn_);
    connect(depthBtn_, &QPushButton::clicked, this, &VisionTestPage::OnShowDepth);
    bar->addWidget(depthBtn_);

    overlayBtn_ = new QPushButton(QStringLiteral("识别框"));
    overlayBtn_->setObjectName("overlayBtn");
    overlayBtn_->setCheckable(true);
    overlayBtn_->setChecked(true);
    SetButtonCheckedStyle(overlayBtn_);
    connect(overlayBtn_, &QPushButton::clicked, this, &VisionTestPage::OnToggleOverlay);
    bar->addWidget(overlayBtn_);

    bar->addStretch();

    infoLabel_ = new QLabel(QStringLiteral("FPS: -- | 时间戳: -- | 分辨率: --"));
    infoLabel_->setStyleSheet("color: #9bb3cf; font-size: 13px; font-family: 'Consolas', monospace; background: transparent; border: none;");
    bar->addWidget(infoLabel_);

    bar->addStretch();

    auto* saveBtn = new QPushButton(QStringLiteral("保存截图"));
    saveBtn->setObjectName("saveBtn");
    SetButtonActionStyle(saveBtn, "#27b95c");
    connect(saveBtn, &QPushButton::clicked, this, &VisionTestPage::OnSnapshot);
    bar->addWidget(saveBtn);

    layout->addLayout(bar);

    return area;
}

void VisionTestPage::UpdateCameraState()
{
    auto& hw = HardwareManager::instance();
    bool opened = hw.camera() && hw.camera()->IsOpened();

    if (opened && streaming_) {
        cameraStateDot_->setStyleSheet("background: #27b95c; border-radius: 6px;");
        cameraStateText_->setText(QStringLiteral("采集运行中"));
    } else if (opened) {
        cameraStateDot_->setStyleSheet("background: #f0b429; border-radius: 6px;");
        cameraStateText_->setText(QStringLiteral("相机已打开"));
    } else {
        cameraStateDot_->setStyleSheet("background: #556677; border-radius: 6px;");
        cameraStateText_->setText(QStringLiteral("相机未打开"));
    }
}

void VisionTestPage::OnOpenCamera()
{
    auto& hw = HardwareManager::instance();
    if (!hw.camera()) {
        SetStatus(QStringLiteral("相机未注册，无法打开"));
        return;
    }
    QString res = resolutionCombo_->currentText();
    auto parts = res.split('x');
    int w = parts.size() == 2 ? parts[0].toInt() : 640;
    int h = parts.size() == 2 ? parts[1].toInt() : 480;
    int fps = fpsCombo_->currentText().toInt();

    deviceIdEdit_->setText(QStringLiteral("CAM-SIM-001"));
    if (hw.CameraOpen(w, h, fps)) {
        SetStatus(QStringLiteral("相机已打开 %1 @ %2fps").arg(res).arg(fps));
    } else {
        SetStatus(QStringLiteral("相机打开失败"));
    }
    UpdateCameraState();
}

void VisionTestPage::OnCloseCamera()
{
    auto& hw = HardwareManager::instance();
    hw.CameraClose();
    streaming_ = false;
    SetStatus(QStringLiteral("相机已关闭"));
    UpdateCameraState();
}

void VisionTestPage::OnStartStream()
{
    auto& hw = HardwareManager::instance();
    if (!hw.camera()) {
        SetStatus(QStringLiteral("相机未注册，无法采集"));
        return;
    }
    if (!hw.camera()->IsOpened()) {
        QString res = resolutionCombo_->currentText();
        auto parts = res.split('x');
        int w = parts.size() == 2 ? parts[0].toInt() : 640;
        int h = parts.size() == 2 ? parts[1].toInt() : 480;
        hw.CameraOpen(w, h, fpsCombo_->currentText().toInt());
    }
    int fps = fpsCombo_->currentText().toInt();
    if (hw.StartCameraStream(fps)) {
        streaming_ = true;
        SetStatus(QStringLiteral("开始采集 @ %1fps").arg(fps));
    } else {
        SetStatus(QStringLiteral("开始采集失败"));
    }
    UpdateCameraState();
}

void VisionTestPage::OnStopStream()
{
    auto& hw = HardwareManager::instance();
    hw.StopCameraStream();
    streaming_ = false;
    SetStatus(QStringLiteral("采集已停止"));
    UpdateCameraState();
}

void VisionTestPage::OnFrameReady(const CameraFrame& frame)
{
    latestFrame_ = frame;
    hasFrame_ = true;
    frameCount_++;
    RenderLatest();
    if (continuous_)
        RunDetection(frame);
}

void VisionTestPage::OnSingleDetect()
{
    if (!hasFrame_) {
        SetStatus(QStringLiteral("无画面可检测，请先开始采集或加载图片"));
        return;
    }
    RunDetection(latestFrame_);
}

void VisionTestPage::RunDetection(const CameraFrame& frame)
{
    auto* algo = HardwareManager::instance().algorithm();
    if (!algo) {
        SetStatus(QStringLiteral("算法未注册，无法检测"));
        return;
    }
    auto results = ToQVector(algo->Detect(frame));
    ShowResults(results);
    lastResults_ = results;
    RenderLatest();

    if (results.empty()) {
        SetStatus(QStringLiteral("检测完成：未找到目标"));
    } else {
        SetStatus(QStringLiteral("检测完成：发现 %1 个目标").arg(results.size()));
    }
}

void VisionTestPage::ShowResults(const QVector<PuffResult>& results)
{
    resultTable_->setRowCount(results.size());
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
    }
}

void VisionTestPage::OnToggleContinuous()
{
    continuous_ = continuousBtn_->isChecked();
    SetStatus(continuous_ ? QStringLiteral("连续检测已开启") : QStringLiteral("连续检测已关闭"));
}

void VisionTestPage::OnToggleOverlay()
{
    showOverlay_ = overlayBtn_->isChecked();
    RenderLatest();
}

void VisionTestPage::OnShowRGB()
{
    rgbBtn_->setChecked(true);
    depthBtn_->setChecked(false);
    depthView_ = false;
    RenderLatest();
}

void VisionTestPage::OnShowDepth()
{
    depthBtn_->setChecked(true);
    rgbBtn_->setChecked(false);
    depthView_ = true;
    RenderLatest();
}

void VisionTestPage::RenderLatest()
{
    if (!hasFrame_) return;
    QImage img = BuildDisplayImage();
    if (img.isNull()) return;

    int w = previewLabel_->width();
    int h = previewLabel_->height();
    if (w <= 10) w = 640;
    if (h <= 10) h = 420;
    QPixmap pm = QPixmap::fromImage(img).scaled(
        QSize(w, h), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    previewLabel_->setPixmap(pm);

    infoLabel_->setText(QStringLiteral("FPS: %1 | 时间戳: %2 | 分辨率: %3x%4")
                            .arg(QString::number(fps_, 'f', 1))
                            .arg(CameraTimeText(latestFrame_.timestamp))
                            .arg(latestFrame_.width)
                            .arg(latestFrame_.height));
}

QImage VisionTestPage::BuildDisplayImage()
{
    if (!hasFrame_) return {};
    QImage img;
    if (depthView_)
        img = FrameConverter::DepthToQImage(latestFrame_, 0.0f, 500.0f);
    else
        img = FrameConverter::ColorToQImage(latestFrame_);

    if (showOverlay_ && !img.isNull()) {
        double thr = ConfigManager::instance().getValue<double>("vision.confidenceThreshold", 0.0);
        FrameConverter::DrawOverlays(img, lastResults_, thr);
    }
    return img;
}

void VisionTestPage::OnSnapshot()
{
    if (!hasFrame_) {
        SetStatus(QStringLiteral("当前无画面，无法保存截图"));
        return;
    }
    QImage img = BuildDisplayImage();
    if (img.isNull()) {
        SetStatus(QStringLiteral("截图生成失败"));
        return;
    }
    QMetaObject::invokeMethod(frameSaver_, "SaveImage", Qt::QueuedConnection,
                              Q_ARG(QImage, img), Q_ARG(QString, QStringLiteral("snapshots")));
    SetStatus(QStringLiteral("截图已加入保存队列"));
}

void VisionTestPage::OnLoadImage()
{
    QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择离线图片"),
        QString(), QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp)"));
    if (path.isEmpty()) return;

    QImage raw(path);
    if (raw.isNull()) {
        SetStatus(QStringLiteral("无法加载图片: %1").arg(path));
        return;
    }
    QImage img = raw.convertToFormat(QImage::Format_RGB888);

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
    RunDetection(latestFrame_);
}

void VisionTestPage::OnFpsTick()
{
    fps_ = frameCount_;
    frameCount_ = 0;
}

void VisionTestPage::OnImageSaved(const QString& path)
{
    SetStatus(QStringLiteral("截图已保存: %1").arg(path));
}

void VisionTestPage::OnSaveError(const QString& message)
{
    SetStatus(message);
}

void VisionTestPage::SetStatus(const QString& text)
{
    statusLabel_->setText(QStringLiteral("● %1").arg(text));
    SPDLOG_INFO("[VisionTestPage] {}", text.toStdString());
}
