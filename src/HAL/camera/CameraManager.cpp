#include "CameraManager.h"

#include "CameraCaptureWorker.h"
#include "ConfigManager.h"
#include "ConfigPaths.h"
#include "ICamera.h"

#include <QMetaObject>
#include <QThread>

#include <spdlog/spdlog.h>

namespace {

// 从 `communication.camera.*` 读出流参数（P3 / S0 统一后的**唯一**归属，
// 见 doc/camera_integration_plan.md §六）。
//
// **为什么读点在这里而不是相机实现内**：沿用「HAL 设备实现不主动读配置」的既有边界
// （与 `BoPaiCard` / `XRServo` 一致）——`CameraManager` 本来就是 `communication.camera.sn`
// 的读取点，把同族的流参数一并收在这里，相机实现只通过 `Configure()` 被动接收。
//
// 幻影键已删：`simulation.cameraDeviceId` 与 `vision.frameWidth/frameHeight/frameFps`
// 在 `config.json` 中**根本不存在**（P1 C4 核实），原读取永远走硬编码默认值。
CameraStreamConfig ReadCameraStreamConfig()
{
    ConfigPaths::ensureConfigLoaded();
    auto& cfg = ConfigManager::instance();

    CameraStreamConfig c;
    c.colorWidth   = cfg.getValue<int>("communication.camera.colorWidth",  c.colorWidth);
    c.colorHeight  = cfg.getValue<int>("communication.camera.colorHeight", c.colorHeight);
    c.colorFps     = cfg.getValue<double>("communication.camera.colorFps", c.colorFps);
    c.colorFormat  = cfg.getValue<std::string>("communication.camera.colorFormat", c.colorFormat);
    c.depthWidth   = cfg.getValue<int>("communication.camera.depthWidth",  c.depthWidth);
    c.depthHeight  = cfg.getValue<int>("communication.camera.depthHeight", c.depthHeight);
    c.depthFps     = cfg.getValue<double>("communication.camera.depthFps", c.depthFps);
    c.depthFormat  = cfg.getValue<std::string>("communication.camera.depthFormat", c.depthFormat);
    c.d2cAlign     = cfg.getValue<std::string>("communication.camera.d2cAlign", c.d2cAlign);
    c.autoExposure = cfg.getValue<bool>("communication.camera.autoExposure", c.autoExposure);
    return c;
}

}  // namespace

CameraManager::CameraManager(QObject* parent)
    : QObject(parent)
{
    cameraThread_ = new QThread(this);
    cameraWorker_ = new CameraCaptureWorker();
    cameraWorker_->moveToThread(cameraThread_);
    connect(cameraThread_, &QThread::finished, cameraWorker_, &QObject::deleteLater);
    connect(cameraWorker_, &CameraCaptureWorker::frameReady, this, &CameraManager::frameReady);
}

CameraManager::~CameraManager()
{
    StopStream();
    if (cameraThread_ && cameraThread_->isRunning()) {
        cameraThread_->quit();
        cameraThread_->wait(2000);
    }
}

void CameraManager::SetCamera(ICamera* camera)
{
    camera_ = camera;
}

std::vector<CameraDeviceInfo> CameraManager::EnumerateDevices()
{
    if (!camera_) return {};
    return camera_->EnumerateDevices();
}

bool CameraManager::Open(int width, int height, double fps)
{
    if (!camera_) {
        SPDLOG_WARN("[CameraManager] Open failed: no camera instance");
        return false;
    }
    // 相机设备标识的唯一键是 `communication.camera.sn`（C2）。
    // 原实现读 `simulation.cameraDeviceId` —— 该键在 `config.json` 中**根本不存在**，
    // 于是永远走硬编码默认值，与配置页/视觉页填的 SN 完全不相通
    // （doc/config_ownership_plan.md §3.4）。缺键时回落到空串，
    // 语义为「由底层实现自行选择设备」，不再伪造一个假 SN。
    ConfigPaths::ensureConfigLoaded();
    const std::string deviceId = ConfigManager::instance().getValue<std::string>(
        "communication.camera.sn", std::string());

    // 流参数：config 是权威默认值；调用方显式传入的非 0 值覆盖它
    // （视觉页的分辨率/帧率下拉框走这条覆盖路径；HardwareManager 传 0 表示"用配置"）。
    CameraStreamConfig cfg = ReadCameraStreamConfig();
    if (width  > 0) cfg.colorWidth  = width;
    if (height > 0) cfg.colorHeight = height;
    if (fps    > 0) cfg.colorFps    = fps;
    // **必须在 Open() 之前下发**（ICamera::Configure 的契约）。
    // 对 SimCamera 是空实现（非纯虚默认），对 OrbbecCamera 决定实际开流 profile。
    camera_->Configure(cfg);

    SPDLOG_INFO("[CameraManager] 生效流参数: color={}x{}@{} fmt='{}' depth={}x{}@{} fmt='{}' "
                "d2cAlign={} autoExposure={}",
                cfg.colorWidth, cfg.colorHeight, cfg.colorFps,
                cfg.colorFormat.empty() ? "(auto)" : cfg.colorFormat,
                cfg.depthWidth, cfg.depthHeight, cfg.depthFps, cfg.depthFormat,
                cfg.d2cAlign, cfg.autoExposure);

    bool ok = camera_->Open(deviceId, cfg.colorWidth, cfg.colorHeight, cfg.colorFps);
    if (!ok)
        SPDLOG_WARN("[CameraManager] Open FAILED: {}", camera_->GetLastError());
    else
        SPDLOG_INFO("[CameraManager] Camera opened (deviceId='{}')", deviceId);
    return ok;
}

void CameraManager::Close()
{
    StopStream();
    if (camera_) camera_->Close();
}

bool CameraManager::StartStream(int fps)
{
    if (!camera_) {
        SPDLOG_WARN("[CameraManager] StartStream failed: no camera instance");
        return false;
    }
    // fps <= 0 = 「用 config 的 communication.camera.colorFps」（P3）。
    // 采集 worker 的 tick 间隔由此决定，须与实际出流帧率同量级，否则要么空转要么积压。
    if (fps <= 0) {
        const CameraStreamConfig c = ReadCameraStreamConfig();
        fps = c.colorFps > 0.0 ? static_cast<int>(c.colorFps + 0.5) : 30;
    }
    if (!camera_->IsOpened())
        Open(0, 0, 0);
    if (!camera_->StartStream()) {
        SPDLOG_WARN("[CameraManager] StartStream FAILED: {}", camera_->GetLastError());
        return false;
    }
    if (!cameraThread_->isRunning()) {
        cameraWorker_->SetCamera(camera_);
        cameraThread_->start();
    }
    QMetaObject::invokeMethod(cameraWorker_, "Start", Qt::QueuedConnection,
                              Q_ARG(int, fps));
    cameraStreaming_ = true;
    return true;
}

void CameraManager::StopStream()
{
    cameraStreaming_ = false;
    if (camera_) camera_->StopStream();
    if (cameraWorker_)
        QMetaObject::invokeMethod(cameraWorker_, "Stop", Qt::QueuedConnection);
}

bool CameraManager::IsStreaming() const
{
    return cameraStreaming_;
}
