#include "CameraManager.h"

#include "CameraCaptureWorker.h"
#include "ConfigManager.h"
#include "ICamera.h"

#include <QMetaObject>
#include <QThread>

#include <spdlog/spdlog.h>

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

bool CameraManager::Open(int width, int height, double fps)
{
    if (!camera_) {
        SPDLOG_WARN("[CameraManager] Open failed: no camera instance");
        return false;
    }
    std::string deviceId = ConfigManager::instance().getValue<std::string>(
        "simulation.cameraDeviceId", "CAM-SIM-001");
    bool ok = camera_->Open(deviceId, width, height, fps);
    if (!ok)
        SPDLOG_WARN("[CameraManager] Open FAILED: {}", camera_->GetLastError());
    else
        SPDLOG_INFO("[CameraManager] Camera opened");
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
