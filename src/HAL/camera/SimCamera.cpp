#include "SimCamera.h"

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

#include "HALFactory.h"

REGISTER_CAMERA("SimCamera", SimCamera)

SimCamera::~SimCamera()
{
    Close();
}

bool SimCamera::Open(const std::string& deviceId, int width, int height, double fps)
{
    std::lock_guard<std::mutex> lock(mutex_);
    deviceId_ = deviceId;
    width_  = (width  > 0)  ? width  : SimVision::kWidth;
    height_ = (height > 0)  ? height : SimVision::kHeight;
    fps_    = (fps > 0.0)   ? fps    : 30.0;

    intrinsics_.width  = width_;
    intrinsics_.height = height_;
    intrinsics_.fx = 520.0;
    intrinsics_.fy = 520.0;
    intrinsics_.cx = width_  * 0.5;
    intrinsics_.cy = height_ * 0.5;

    start_    = std::chrono::steady_clock::now();
    opened_   = true;
    lastError_.clear();
    SPDLOG_INFO("[SimCamera] Opened device='{}' {}x{} @ {:.0f}fps", deviceId_, width_, height_, fps_);
    return true;
}

void SimCamera::Close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    streaming_ = false;
    opened_    = false;
    SPDLOG_INFO("[SimCamera] Closed");
}

bool SimCamera::IsOpened() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return opened_;
}

bool SimCamera::StartStream()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!opened_) {
        lastError_ = "camera not opened";
        return false;
    }
    streaming_ = true;
    SPDLOG_INFO("[SimCamera] Stream started");
    return true;
}

void SimCamera::StopStream()
{
    std::lock_guard<std::mutex> lock(mutex_);
    streaming_ = false;
}

bool SimCamera::IsStreaming() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return streaming_;
}

CameraFrame SimCamera::CaptureFrame()
{
    std::lock_guard<std::mutex> lock(mutex_);
    CameraFrame frame;
    if (!opened_) {
        lastError_ = "camera not opened";
        return frame;
    }
    auto now   = std::chrono::steady_clock::now();
    double t   = std::chrono::duration<double>(now - start_).count();
    RenderFrame(frame, t);
    return frame;
}

CameraIntrinsics SimCamera::GetIntrinsics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return intrinsics_;
}

bool SimCamera::SetIntrinsics(const CameraIntrinsics& intrinsics)
{
    std::lock_guard<std::mutex> lock(mutex_);
    intrinsics_ = intrinsics;
    return true;
}

std::string SimCamera::GetLastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

void SimCamera::RenderFrame(CameraFrame& frame, double tSec)
{
    frame.width  = width_;
    frame.height = height_;
    frame.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const int w = width_;
    const int h = height_;
    frame.colorData.assign(static_cast<size_t>(w) * h * 3, 0);
    frame.depthData.assign(static_cast<size_t>(w) * h, 600.0f);

    struct Blob { double cx, cy; int r, g, b; float depth; };
    Blob blobs[SimVision::kTargetCount];
    for (int i = 0; i < SimVision::kTargetCount; ++i) {
        const auto& t = SimVision::kTargets[i];
        double px = w * 0.5 + t.ampX * std::sin(SimVision::MotionPhase(tSec, t.speed) + t.phase);
        double py = h * 0.5 + t.ampY * std::cos(SimVision::MotionPhase(tSec, t.speed * 0.7) + t.phase);
        blobs[i] = { px, py, t.r, t.g, t.b, t.depthMm };
    }

    const int stride = w * 3;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = frame.colorData.data() + static_cast<size_t>(y) * stride;
        float*   dep = frame.depthData.data()  + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            int cr = 24, cg = 28, cb = 34;
            if (((x / 32) + (y / 32)) % 2 == 0) { cr = 28; cg = 32; cb = 39; }
            if (x % 64 == 0 || y % 64 == 0) { cr = 44; cg = 52; cb = 62; }

            float depth = 600.0f;
            for (int i = 0; i < SimVision::kTargetCount; ++i) {
                const Blob& b = blobs[i];
                double dx = static_cast<double>(x) - b.cx;
                double dy = static_cast<double>(y) - b.cy;
                double r2 = dx * dx + dy * dy;
                int radius = SimVision::kTargets[i].radius;
                if (r2 <= static_cast<double>(radius * radius)) {
                    cr = b.r; cg = b.g; cb = b.b;
                    depth = b.depth;
                    break;
                }
            }

            cr += ((x * 7 + y * 13) % 5) - 2;
            cg += ((x * 11 + y * 5) % 5) - 2;
            cb += ((x * 3 + y * 17) % 5) - 2;

            row[x * 3 + 0] = static_cast<uint8_t>(std::clamp(cr, 0, 255));
            row[x * 3 + 1] = static_cast<uint8_t>(std::clamp(cg, 0, 255));
            row[x * 3 + 2] = static_cast<uint8_t>(std::clamp(cb, 0, 255));
            dep[x] = depth;
        }
    }
}
