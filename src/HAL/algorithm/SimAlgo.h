#pragma once

#include <string>
#include <vector>

#include "IPuffAlgorithm.h"

class SimAlgo : public IPuffAlgorithm
{
public:
    std::string Name() const override;

    std::vector<PuffResult> Detect(const CameraFrame& frame) override;
    PuffResult LocateBest(const CameraFrame& frame) override;

    bool LoadConfig(const std::string& jsonConfig) override;

    std::string GetLastError() const override;

private:
    std::string lastError_;
    std::string config_;
};
