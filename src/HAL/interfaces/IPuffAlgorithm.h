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

/// 算法参数快照（P1，2026-09-22）。
///
/// 算法原先在 `Detect` 内部直接读 `ConfigManager`。把检测挪到工作线程后这条路径违反
/// 项目既有硬约束——**worker 线程绝不读 `ConfigManager`**（`json` 读写非线程安全，
/// 见 `SequenceWorker.cpp:106` 的同名约定）。故拆成两步：
/// ① `SnapshotParams()` **只能在主线程调**，取一份参数快照；
/// ② `Detect(frame, params)` 变成**纯函数**，不再依赖任何全局状态，可在任意线程调用。
struct AlgorithmParams
{
    double confidenceThreshold = 0.85;
    double depthZMin = 0.0;
    double depthZMax = 500.0;
};

class IPuffAlgorithm
{
public:
    virtual ~IPuffAlgorithm() = default;

    virtual std::string Name() const = 0;

    /// 参数快照。⚠ **只允许在主线程调用**（内部读 `ConfigManager`）。
    virtual AlgorithmParams SnapshotParams() const = 0;

    /// 检测。**不读全局配置**——所有参数由 `params` 传入，可在工作线程调用。
    virtual std::vector<PuffResult> Detect(const CameraFrame& frame, const AlgorithmParams& params) = 0;
    virtual PuffResult LocateBest(const CameraFrame& frame, const AlgorithmParams& params) = 0;

    virtual bool LoadConfig(const std::string& jsonConfig) = 0;

    virtual std::string GetLastError() const = 0;
};
