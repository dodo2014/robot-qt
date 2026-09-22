#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "IPuffAlgorithm.h"

class SimAlgo : public IPuffAlgorithm
{
public:
    std::string Name() const override;

    /// 参数快照（P1）。⚠ 主线程专用——内部读 `ConfigManager`。
    AlgorithmParams SnapshotParams() const override;

    /// 检测（P1 起为纯函数式：参数由调用方传入，**不读全局配置**）。
    /// 本仓库中检测可能同时在「视觉页检测线程」与「SequenceWorker 视觉识别动作」两侧发生，
    /// 故用 `mutex_` 串行化——`lastError_` 是唯一被写入的成员，并发写 `std::string` 是 UB。
    std::vector<PuffResult> Detect(const CameraFrame& frame, const AlgorithmParams& params) override;
    PuffResult LocateBest(const CameraFrame& frame, const AlgorithmParams& params) override;

    bool LoadConfig(const std::string& jsonConfig) override;

    std::string GetLastError() const override;

private:
    /// 生效阈值：优先取 `config.json` 的 `vision.confidenceThreshold`（实时读，与
    /// `depthZMin` / `depthZMax` 同一惯用法），键缺失时回落到 `LoadConfig` 提供的值。
    /// ⚠ 主线程专用（读 `ConfigManager`），只由 `SnapshotParams()` 调用。
    double EffectiveThreshold() const;

    mutable std::mutex mutex_;
    std::string lastError_;
    std::string config_;
    /// `LoadConfig` 解析出的兜底阈值（C1）。默认值与 config.json 现状一致。
    double confidenceThreshold_ = 0.85;
};
