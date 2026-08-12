#pragma once

#include <mutex>
#include <string>
#include <chrono>

#include "ICamera.h"
#include "SimVision.h"

class SimCamera : public ICamera
{
public:
    SimCamera() = default;
    ~SimCamera() override;

    bool Open(const std::string& deviceId, int width, int height, double fps) override;
    void Close() override;
    bool IsOpened() const override;

    bool StartStream() override;
    void StopStream() override;
    bool IsStreaming() const override;

    CameraFrame CaptureFrame() override;

    CameraIntrinsics GetIntrinsics() const override;
    bool SetIntrinsics(const CameraIntrinsics& intrinsics) override;

    std::string GetLastError() const override;

private:
    void RenderFrame(CameraFrame& frame, double tSec);

    mutable std::mutex mutex_;
    bool opened_    = false;
    bool streaming_ = false;
    int width_      = SimVision::kWidth;
    int height_     = SimVision::kHeight;
    double fps_     = 30.0;
    std::string deviceId_;
    std::string lastError_;
    CameraIntrinsics intrinsics_;
    std::chrono::steady_clock::time_point start_;
};
