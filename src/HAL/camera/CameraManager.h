#pragma once

#include <QObject>

#include <vector>

#include "ICamera.h"

class QThread;
class CameraCaptureWorker;
struct CameraFrame;

// 相机生命周期管理器（采集线程 + 采集 worker + 流状态）。
// 与 HardwareManager 解耦：只负责 Open/Close/StartStream/StopStream 与帧广播。
// camera_ 实例由外部（HardwareManager）注入，本类不持有所有权、不负责销毁。
class CameraManager : public QObject
{
    Q_OBJECT

public:
    explicit CameraManager(QObject* parent = nullptr);
    ~CameraManager() override;

    void SetCamera(ICamera* camera);

    /// 枚举可用设备（P3 / S2）。空相机实例时返回空列表。
    std::vector<CameraDeviceInfo> EnumerateDevices();

    bool Open(int width, int height, double fps);
    void Close();
    bool StartStream(int fps = 30);
    void StopStream();
    bool IsStreaming() const;

signals:
    void frameReady(const CameraFramePtr& frame);

private:
    ICamera*             camera_          = nullptr;
    QThread*             cameraThread_    = nullptr;
    CameraCaptureWorker* cameraWorker_    = nullptr;
    bool                 cameraStreaming_ = false;
};
