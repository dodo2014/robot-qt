#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CameraIntrinsics
{
    double fx = 0.0, fy = 0.0;
    double cx = 0.0, cy = 0.0;
    double k1 = 0.0, k2 = 0.0, p1 = 0.0, p2 = 0.0;
    int width  = 640;
    int height = 480;
};

struct CameraFrame
{
    int64_t timestamp = 0;
    int width  = 0;
    int height = 0;
    std::vector<uint8_t> colorData;   // RGB/BGR
    std::vector<float>   depthData;   // 深度图 (mm)
    std::vector<float>   pointCloud;  // 可选: XYZ 点云
};

class ICamera
{
public:
    virtual ~ICamera() = default;

    virtual bool Open(const std::string& deviceId, int width, int height, double fps) = 0;
    virtual void Close() = 0;
    virtual bool IsOpened() const = 0;

    virtual bool StartStream() = 0;
    virtual void StopStream() = 0;
    virtual bool IsStreaming() const = 0;

    virtual CameraFrame CaptureFrame() = 0;

    virtual CameraIntrinsics GetIntrinsics() const = 0;
    virtual bool SetIntrinsics(const CameraIntrinsics& intrinsics) = 0;

    virtual std::string GetLastError() const = 0;
};
