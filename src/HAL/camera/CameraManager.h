#pragma once

#include <QObject>

class QThread;
class ICamera;
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

    bool Open(int width, int height, double fps);
    void Close();
    bool StartStream(int fps = 30);
    void StopStream();
    bool IsStreaming() const;

signals:
    void frameReady(const CameraFrame& frame);

private:
    ICamera*             camera_          = nullptr;
    QThread*             cameraThread_    = nullptr;
    CameraCaptureWorker* cameraWorker_    = nullptr;
    bool                 cameraStreaming_ = false;
};
