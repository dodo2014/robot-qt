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
    /// 生效阈值：优先取 `config.json` 的 `vision.confidenceThreshold`（实时读，与
    /// `depthZMin` / `depthZMax` 同一惯用法），键缺失时回落到 `LoadConfig` 提供的值。
    double EffectiveThreshold() const;

    std::string lastError_;
    std::string config_;
    /// `LoadConfig` 解析出的兜底阈值（C1）。默认值与 config.json 现状一致。
    double confidenceThreshold_ = 0.85;
};
