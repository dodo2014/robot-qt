#pragma once

#include <string>
#include <vector>

#include "ICamera.h"

struct PuffResult
{
    double  x         = 0.0;
    double  y         = 0.0;
    double  z         = 0.0;
    double  yaw       = 0.0;
    double  confidence = 0.0;
    int     pixelU    = 0;
    int     pixelV    = 0;
    int     width     = 0;
    int     height    = 0;
};

class IPuffAlgorithm
{
public:
    virtual ~IPuffAlgorithm() = default;

    virtual std::string Name() const = 0;

    virtual std::vector<PuffResult> Detect(const CameraFrame& frame) = 0;
    virtual PuffResult LocateBest(const CameraFrame& frame) = 0;

    virtual bool LoadConfig(const std::string& jsonConfig) = 0;

    virtual std::string GetLastError() const = 0;
};
