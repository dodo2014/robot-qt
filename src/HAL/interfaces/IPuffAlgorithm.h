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

    /// 目标**球顶**在**彩色图**的像素坐标（原点左上，x 右、y 下）。
    ///
    /// ⚠ 语义 = **球顶**（连通域内「最高点所在面片」的质心），**不是**连通域质心——真机帧上
    /// 二者实测相距 **~47 px（≈23 mm）**（2026-10-16，`depth_20261016103952_0`）。
    /// 选球顶的理由：`z` 也是球顶深度，二者必须指向**同一个物理点**；否则
    /// `PixelToRobot(pixelU, pixelV, z)` 反投影出的是一个**不在目标表面上**的点。
    /// ⚠ 画包围框请用 `bboxX` / `bboxY`——本点 = 球顶，**不等于**包围框中心。
    int     pixelU    = 0;
    int     pixelV    = 0;

    /// 包围框（**彩色系**）：左上角 `(bboxX, bboxY)` + 尺寸 `(width, height)`。
    /// 独立给出原点而非「以某点为中心」：球顶与包围框中心本就相差 ~47 px，
    /// 只给中心 + 尺寸无法还原包围框（TR-132）。
    int     bboxX     = 0;
    int     bboxY     = 0;
    int     width     = 0;
    int     height    = 0;

    /// 目标像素面积（C3，2026-10-14）。⚠ **坐标系 = 深度图**——算法在深度域做连通域，
    /// 而 `pixelU` / `pixelV` / `bboxX` / `bboxY` / `width` / `height` 归一到**彩色系**。
    /// D2C = `SW` 时两分辨率相同、二者等价；D2C 关闭时不等价（`algorithm_config_plan.md` §11.3）。
    double  area      = 0.0;

    /// 连通域内的**原始最小**深度（C3）。
    /// ⚠ 与 `z` **不再恒等**（TR-132）：`z` = 球顶面片**均值**（抗单像素噪点），本字段保留
    /// 原始极值 ⇒ 二者之差即该连通域上的**深度噪点 / 尖点幅度**（实测某球 `min = 516 mm`
    /// 而其 0.5% 分位 = `522 mm`，差 6 mm 全属噪点）。供交叉校验与调参。
    double  minDepthMm = 0.0;
};

/// 感兴趣区域（ROI）。
///
/// ⚠ **坐标系 = 深度图**（算法在深度域做连通域）；`w` 或 `h` ≤ 0 视为**全图**。
///
/// 用独立 POD 而非 `cv::Rect`：本头文件是**纯接口**，不引入 OpenCV 依赖——契约要求
/// `AlgorithmParams` 可被完整序列化（`algorithm_config_plan.md` §11.5 的适配器前提）。
struct RoiRect
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
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
    // ---- 通用域：跨算法共享（`vision.*`）----
    double confidenceThreshold = 0.85;
    double depthZMin = 0.0;
    double depthZMax = 500.0;

    // ---- 深度法私有域（`vision.algorithms.DepthPuffAlgo.*`，C3 / 2026-10-14）----
    //
    // ⚠ 覆盖关系 = **算法节点优先、通用域作默认**（`algorithm_config_plan.md` §十 裁定 3）。
    // ⚠ `roi` / `minArea` / `maxArea` / `edgeMargin` 的坐标系 = **深度图**。
    // ⚠ 只加**当前已注册实现**的量；改本结构体属头文件改动 ⇒ 必须 `-t clean` 全清重建。
    double  nearestPercentile   = 0.03;    // 最近层分位数；代码内 clamp(0.001, 0.5)
    double  heightBandMm        = 35.0;    // 最高层带厚；代码内 max(1.0, 值)

    /// 球顶面片比例（TR-132，2026-10-16）：取连通域内**深度最小的这一比例**的像素，
    /// 其质心 ⇒ `pixelU` / `pixelV`，其均值深度 ⇒ `z`。代码内 `clamp(0.005, 0.5)`。
    ///
    /// ⚠ 用**比例**而非「绝对毫米带」：比例自适应连通域大小与深度梯度，无需按球径重标定。
    /// ⚠ **不可取到 0**：极小值可能是**单像素噪点**——实测某球 `min = 516 mm` 而其
    /// 0.5% 分位 = `522 mm`，若直接取 `min` 所在像素，球顶会偏 6 mm 且被噪点牵引。
    double  apexPercentile      = 0.02;
    int     minArea             = 120;     // 连通域面积下限 px²（深度系，滤噪声）
    int     maxArea             = 250000;  // 连通域面积上限 px²（深度系，排除非单目标）
    int     targetArea          = 0;       // 目标泡芙面积；**0 = 禁用面积评分**
    int     targetAreaTolerance = 0;       // 容差；**0 = 自动取 max(1, targetArea/2)**
    int     morphKernelSize     = 7;       // 形态学核（开 + 闭）；偶数自动 +1，≤1 时跳过
    int     edgeMargin          = 8;       // 边缘过滤距离 px（深度系）
    RoiRect roi;                           // 感兴趣区域；w/h ≤ 0 = 全图
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
