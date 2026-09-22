#include "CameraCaptureWorker.h"

#include <algorithm>

#include <QTimer>

#include <spdlog/spdlog.h>

namespace {
int g_cameraMetaRegistered = []() {
    qRegisterMetaType<CameraFrame>("CameraFrame");
    // 跨线程 queued connection 需要注册实参类型（P2：帧改共享只读指针）。
    qRegisterMetaType<CameraFramePtr>("CameraFramePtr");
    return 0;
}();
}

CameraCaptureWorker::CameraCaptureWorker(QObject* parent)
    : QObject(parent)
{
    (void)g_cameraMetaRegistered;
}

CameraCaptureWorker::~CameraCaptureWorker()
{
    Stop();
}

void CameraCaptureWorker::SetCamera(ICamera* camera)
{
    camera_ = camera;
}

void CameraCaptureWorker::Start(int fps)
{
    if (running_) return;
    if (!camera_) {
        SPDLOG_WARN("[CameraCaptureWorker] Start requested but no camera set");
        return;
    }
    if (!timer_) {
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, &CameraCaptureWorker::CaptureTick);
    }
    int interval = std::max(1, static_cast<int>(1000.0 / std::max(1, fps)));
    timer_->setInterval(interval);
    timer_->start();
    running_ = true;
    SPDLOG_INFO("[CameraCaptureWorker] Capture started @ {}fps", fps);
}

void CameraCaptureWorker::Stop()
{
    if (timer_) timer_->stop();
    running_ = false;
}

void CameraCaptureWorker::CaptureTick()
{
    if (!camera_) return;
    // P2：帧数据在采集线程构造**一次**，之后以共享只读指针传递。
    // 原先 `CameraFrame frame = camera_->CaptureFrame(); emit frameReady(frame);` 在跨线程
    // queued connection 上会让 Qt 把整个帧（≈14.5 MB）深拷贝进事件队列。
    auto frame = std::make_shared<CameraFrame>(camera_->CaptureFrame());
    if (frame->width > 0 && frame->height > 0 && !frame->colorData.empty())
        emit frameReady(frame);
}
