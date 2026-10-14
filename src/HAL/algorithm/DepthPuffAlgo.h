#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "IPuffAlgorithm.h"

/// 深度法泡芙定位算法（移植自老项目 `robot-puff/Framework/VisionModule` 的 `CPuffLocator`）。
///
/// **路线**：纯深度图定位——有效深度掩码 → 最高层 `mask` → 连通域分析 → 综合打分。
/// 老实现本就是纯深度算法（`Locate` 的 `color` 形参**匿名且完全未使用**），故 RGB 入口不移植。
///
/// **边界**（契约见 `algorithm_config_plan.md` §十一）：
/// - ❌ **不做**坐标转换——像素 + 深度 → 基座坐标由下游 `CoordTransform::PixelToRobot` 负责
/// - ❌ **不持有**相机内参
/// - ❌ **不读** `ConfigManager`——参数由 `SnapshotParams()` 在**主线程**快照后经 `Detect` 传入
///
/// **移植方案**见 `doc/depth_puff_algorithm_plan.md`；**参数初值表**见同篇 §四。
class DepthPuffAlgo : public IPuffAlgorithm
{
public:
    std::string Name() const override;

    /// 参数快照（C4）。⚠ **主线程专用**——内部读 `ConfigManager`。
    AlgorithmParams SnapshotParams() const override;

    /// 检测。**不读全局配置**——所有参数由 `params` 传入，可在工作线程调用。
    ///
    /// 返回按 `score` **降序**排列的结果（契约 §11.3 约定 1：`front()` = 最佳目标）；
    /// 空 vector = 未检出（**不是异常**，约定 2）；不满足阈值的目标**丢弃**（约定 3）。
    std::vector<PuffResult> Detect(const CameraFrame& frame, const AlgorithmParams& params) override;
    PuffResult LocateBest(const CameraFrame& frame, const AlgorithmParams& params) override;

    bool LoadConfig(const std::string& jsonConfig) override;

    std::string GetLastError() const override;

private:
    /// 串行化：视觉页检测线程与 `SequenceWorker` 识别动作可能共用同一实例，
    /// `lastError_` 是唯一被写入的成员，并发写 `std::string` 是 UB（同 `SimAlgo.h:17-19`）。
    mutable std::mutex mutex_;
    std::string lastError_;
    std::string config_;

    /// `LoadConfig` 解析出的**兜底参数**（C5）。运行时以 `SnapshotParams()` 的实时读为准；
    /// 算法节点缺失（调用方传 `{}`）时保留结构体内置默认值，**不报错**（§十 裁定 1）。
    AlgorithmParams fallback_;

    /// 「深度分辨率 ≠ 彩色分辨率（D2C 关闭）」的告警去重位（TR-116）。
    /// 该情形下像素输出只能做**比例映射**（非像素级对应），需提示一次以免被当成精确对应。
    bool depthAlignWarned_ = false;
};
