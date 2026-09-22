#pragma once

#include <QObject>

#include "ICamera.h"

class QTimer;

class CameraCaptureWorker : public QObject
{
    Q_OBJECT

public:
    explicit CameraCaptureWorker(QObject* parent = nullptr);
    ~CameraCaptureWorker() override;

    void SetCamera(ICamera* camera);

public slots:
    void Start(int fps = 30);
    void Stop();

signals:
    // P2：帧以共享只读指针广播（值语义会在跨线程入队时深拷贝 14.5 MB，见 `ICamera.h`）。
    void frameReady(const CameraFramePtr& frame);

private slots:
    void CaptureTick();

private:
    ICamera* camera_ = nullptr;
    QTimer*  timer_  = nullptr;
    bool     running_ = false;
};
