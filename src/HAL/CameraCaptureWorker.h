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
    void frameReady(const CameraFrame& frame);

private slots:
    void CaptureTick();

private:
    ICamera* camera_ = nullptr;
    QTimer*  timer_  = nullptr;
    bool     running_ = false;
};
