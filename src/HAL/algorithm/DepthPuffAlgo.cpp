#include "DepthPuffAlgo.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "ConfigManager.h"
#include "HALFactory.h"

REGISTER_PUFF_ALGORITHM("DepthPuffAlgo", DepthPuffAlgo)

namespace {

// ---- 代码内硬编码、不可配的量（`depth_puff_algorithm_plan.md` §四）----
// 如将来需调，须改代码。
constexpr double kNearestPercentileMin = 0.001;   // nearestPercentile 下钳位
constexpr double kNearestPercentileMax = 0.5;     // 上钳位
constexpr double kMinHeightBandMm      = 1.0;     // heightBandMm 下限
constexpr double kApexPercentileMin    = 0.005;   // apexPercentile 下钳位（TR-132）
constexpr double kApexPercentileMax    = 0.5;     // 上钳位
constexpr int    kApexMinPixels        = 8;       // 球顶面片最少像素数（小连通域兜底）
constexpr int    kMinValidPixelsFloor  = 10;      // 连通域最少有效像素下限
constexpr int    kMinValidPixelsDiv     = 4;      // …与 minArea 的除数（取 max）
constexpr double kScoreConfWeight      = 0.60;    // score = conf*0.60 + near*0.25 + area*0.15
constexpr double kScoreNearWeight      = 0.25;
constexpr double kScoreAreaWeight      = 0.15;
constexpr double kConfAreaWeight       = 0.45;    // confidence = area*0.45 + stability*0.35 + center*0.20
constexpr double kConfStabilityWeight  = 0.35;
constexpr double kConfCenterWeight     = 0.20;
constexpr int    kAreaScoreScale       = 8;       // 面积评分退化分支：area / (minArea * 8)

// ---- 组件内峰切分（TR-135，2026-10-16）：三球挤在一起时的「解合并」----
// 判据由 15 帧真机深度图扫参定死（`TEST_RECORD.md` TR-135）：单球恒 1 峰、三球合并域恒 3 峰。
constexpr double kSplitSepRatio          = 1.0;   // 峰间最小间距 = kSplitSepRatio  × dist.max()
constexpr double kSplitPeakRatio         = 0.55;  // 峰最小强度   = kSplitPeakRatio × dist.max()
constexpr int    kSplitLocalKernel       = 7;     // 局部极大值比较核（偶数自动 +1）
constexpr double kSplitLocalEps          = 0.5;   // 局部极大值容差（px，抗距离变换平台抖动）
constexpr double kSplitPlateauFloorRatio = 0.25;  // 候选峰预筛：dist > 0.25 × dist.max()
constexpr int    kSplitMinMarkers        = 2;     // 触发切分 / 判定切分成功的最低峰数

/// 单个候选连通域（**深度域**坐标）。
struct Candidate
{
    int         label = 0;
    cv::Rect    bbox;
    cv::Point2f center;              // 连通域质心——**仅用于边缘过滤**，不再作为输出中心
    int         area = 0;
    double      meanDepthMm = 0.0;   // 区域均值——仅入日志，供调参对照穹顶高度
    double      minDepthMm = 0.0;    // 区域**原始**最小深度——仅入日志 / 交叉校验（TR-132）

    // ---- 球顶（TR-132，2026-10-16）----
    // 契约 §11.3：`pixelU`/`pixelV` = **球顶**、`z` = 球顶深度，二者必须是同一个物理点。
    cv::Point2f apex;                // 球顶面片质心（深度域）⇒ pixelU/pixelV
    double      apexDepthMm = 0.0;   // 球顶面片均值深度 ⇒ z（抗单像素噪点）
    int         apexCount   = 0;     // 面片像素数（0 = 退化，见 SelectRegions 兜底）
    double      score = 0.0;         // 三权重综合分，深度并列时的次级排序键
};

/// 面积评分。`targetArea > 0` 时按容差打分；`= 0` 时退化（⚠ 无法区分单泡芙与一堆泡芙，见 M3）。
double CalculateAreaScore(double area, const AlgorithmParams& p)
{
    if (p.targetArea > 0) {
        const int tolerance = p.targetAreaTolerance > 0
                                  ? p.targetAreaTolerance
                                  : std::max(1, p.targetArea / 2);
        const double diff = std::abs(area - static_cast<double>(p.targetArea));
        return std::clamp(1.0 - diff / tolerance, 0.0, 1.0);
    }
    return std::min(1.0, area / std::max(1, p.minArea * kAreaScoreScale));
}

/// 综合置信度：面积 0.45 + 深度稳定 0.35 + 居中 0.20。
/// ⚠ 注意：老实现最终写入结果的 `confidence` **不是本函数的输出**，而是下面 `score` 的
/// clamp 值（见 `SelectRegions`）——本函数只是 `score` 的一个输入项。
double EstimateConfidence(double area, double depthStdDev, const cv::Point2f& center,
                          const cv::Size& imageSize, const AlgorithmParams& p)
{
    const double areaScore = CalculateAreaScore(area, p);
    const double stabilityScore = 1.0 / (1.0 + depthStdDev / std::max(1.0, p.heightBandMm));

    const cv::Point2f imageCenter(imageSize.width * 0.5f, imageSize.height * 0.5f);
    const double maxDistance = std::sqrt(imageCenter.x * imageCenter.x +
                                         imageCenter.y * imageCenter.y);
    const double distance = cv::norm(center - imageCenter);
    const double centerScore = maxDistance > 0.0
                                   ? 1.0 - std::min(1.0, distance / maxDistance)
                                   : 1.0;

    return std::clamp(areaScore * kConfAreaWeight +
                          stabilityScore * kConfStabilityWeight +
                          centerScore * kConfCenterWeight,
                      0.0, 1.0);
}

/// 有效 ROI：与图像求交；`w` / `h` ≤ 0 视为全图。
cv::Rect EffectiveRoi(const RoiRect& roi, const cv::Size& imageSize)
{
    const cv::Rect imageRect(0, 0, imageSize.width, imageSize.height);
    if (roi.w <= 0 || roi.h <= 0)
        return imageRect;
    return cv::Rect(roi.x, roi.y, roi.w, roi.h) & imageRect;
}

/// ① 有效深度掩码：ROI 内、有限值、落在 `[zMin, zMax]`。返回有效像素数。
int BuildValidMask(const cv::Mat& depthMm, const cv::Rect& roi,
                   double zMin, double zMax, cv::Mat& validMask)
{
    validMask = cv::Mat::zeros(depthMm.size(), CV_8UC1);
    int validCount = 0;

    for (int y = roi.y; y < roi.y + roi.height; ++y) {
        const float* depthRow = depthMm.ptr<float>(y);
        uchar* maskRow = validMask.ptr<uchar>(y);
        for (int x = roi.x; x < roi.x + roi.width; ++x) {
            const float value = depthRow[x];
            if (std::isfinite(value) && value >= zMin && value <= zMax) {
                maskRow[x] = 255;
                ++validCount;
            }
        }
    }
    return validCount;
}

/// ② 最高层掩码 + 最近深度。返回 false = 无有效像素 / 形态学后无候选。
bool BuildTopRegionMask(const cv::Mat& depthMm, const cv::Mat& validMask,
                        const AlgorithmParams& p, cv::Mat& topMask,
                        double& nearestDepthMm)
{
    std::vector<float> values;
    values.reserve(static_cast<size_t>(cv::countNonZero(validMask)));

    for (int y = 0; y < depthMm.rows; ++y) {
        const float* depthRow = depthMm.ptr<float>(y);
        const uchar* maskRow = validMask.ptr<uchar>(y);
        for (int x = 0; x < depthMm.cols; ++x) {
            if (maskRow[x])
                values.push_back(depthRow[x]);
        }
    }
    if (values.empty())
        return false;

    const double percentile = std::clamp(p.nearestPercentile,
                                         kNearestPercentileMin, kNearestPercentileMax);
    const size_t nth = static_cast<size_t>((values.size() - 1) * percentile);
    std::nth_element(values.begin(), values.begin() + nth, values.end());
    nearestDepthMm = values[nth];

    const double thresholdMm = nearestDepthMm + std::max(kMinHeightBandMm, p.heightBandMm);
    topMask = cv::Mat::zeros(depthMm.size(), CV_8UC1);

    for (int y = 0; y < depthMm.rows; ++y) {
        const float* depthRow = depthMm.ptr<float>(y);
        const uchar* validRow = validMask.ptr<uchar>(y);
        uchar* topRow = topMask.ptr<uchar>(y);
        for (int x = 0; x < depthMm.cols; ++x) {
            if (validRow[x] && depthRow[x] <= thresholdMm)
                topRow[x] = 255;
        }
    }

    if (p.morphKernelSize > 1) {
        int kernelSize = p.morphKernelSize;
        if (kernelSize % 2 == 0)
            ++kernelSize;   // ⚠ 偶数自动 +1（M6）——避免「填了 6 实际 7」的困惑
        const cv::Mat kernel = cv::getStructuringElement(
            cv::MORPH_ELLIPSE, cv::Size(kernelSize, kernelSize));
        cv::morphologyEx(topMask, topMask, cv::MORPH_OPEN, kernel);
        cv::morphologyEx(topMask, topMask, cv::MORPH_CLOSE, kernel);
    }

    return cv::countNonZero(topMask) > 0;
}

/// 组件内「距离变换峰值」检测（TR-135，2026-10-16）。
///
/// **动机**：三个纸团挤在一起时，`BuildTopRegionMask` 末尾的 `MORPH_CLOSE` 会把它们**桥接**
/// 成单个连通域——真机实测 `depth_20261016153457_0`：合并域 `area = 18458` ≈ 三球之和、
/// `bbox = 160×177`；而三球分开时各 `area ≈ 6458/5909/5025`、`bbox ≈ 96×92`。
/// 组件级只能产出 1 个目标 ⇒ 用户报障「三个纸团只检测出一个」。
///
/// **做法**：对组件掩码做欧氏距离变换，其**局部极大值** ≈ 各球的「内切圆心」；再用
/// **尺度自适应 NMS** 去重。两个判据都相对 `dist.max()`（≈ 球半径）⇒ 尺度无关：
/// 换球径 / 换安装高度 / 换相机距离**无需重标定**。
/// - `minSep  = kSplitSepRatio  × dist.max()`（= 1.0）
/// - `minPeak = kSplitPeakRatio × dist.max()`（= 0.55）
///
/// **为什么这两个值安全（可证，非拟合）**：理想圆盘上 `dist(p) = r − |p − c|`；
/// `dist ≥ 0.55r` ⟺ 点落在圆心 `0.45r` 内 ⇒ 任意两个合格峰间距 ≤ `0.9r` < `r` = `minSep`
/// ⇒ **单个圆球几何上不可能产出 2 个峰**。实测 15 帧 / 20 个单球组件全部恰好 1 峰。
///
/// 返回峰点（**深度域绝对坐标**）；空 = 无峰（组件至少 1 px ⇒ `dist.max() > 0`，理论不可达）。
std::vector<cv::Point2f> FindMarkers(const cv::Mat& compMask, const cv::Rect& bbox)
{
    std::vector<cv::Point2f> markers;

    const cv::Rect imageRect(0, 0, compMask.cols, compMask.rows);
    const cv::Rect clipped = bbox & imageRect;
    if (clipped.width <= 0 || clipped.height <= 0)
        return markers;

    // 只在组件紧包围框内做距离变换——与全图结果等价，且省掉绝大多数像素。
    const cv::Mat crop = compMask(clipped);
    cv::Mat dist;
    cv::distanceTransform(crop, dist, cv::DIST_L2, 5);

    double dmax = 0.0;
    cv::minMaxLoc(dist, nullptr, &dmax);
    if (dmax <= 0.0)
        return markers;

    // ---- ① 候选峰掩码：局部极大（带 eps 容差，抗平台抖动）且强度 > 25% 峰值 ----
    int k = kSplitLocalKernel;
    if (k % 2 == 0)
        ++k;
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
    cv::Mat dilated;
    cv::dilate(dist, dilated, kernel);

    cv::Mat peakMask = cv::Mat::zeros(dist.size(), CV_8UC1);
    for (int y = 0; y < dist.rows; ++y) {
        const float* dRow = dist.ptr<float>(y);
        const float* mRow = dilated.ptr<float>(y);
        uchar* pRow = peakMask.ptr<uchar>(y);
        for (int x = 0; x < dist.cols; ++x) {
            if (dRow[x] > 0.0f &&
                dRow[x] >= mRow[x] - static_cast<float>(kSplitLocalEps) &&
                dRow[x] > static_cast<float>(kSplitPlateauFloorRatio * dmax))
                pRow[x] = 255;
        }
    }

    // ---- ② 距离变换的极大值常成「平台」（多个等距像素）⇒ 按 8 连通域归并、各取最强像素 ----
    cv::Mat pkLabels, pkStats, pkCentroids;
    const int pkCount = cv::connectedComponentsWithStats(peakMask, pkLabels, pkStats,
                                                         pkCentroids, 8, CV_32S);
    if (pkCount <= 1)
        return markers;

    struct Peak
    {
        double strength;
        float  x;
        float  y;
    };
    std::vector<Peak> cand;
    {
        std::vector<double>    bestV(static_cast<size_t>(pkCount), -1.0);
        std::vector<cv::Point> bestP(static_cast<size_t>(pkCount));
        for (int y = 0; y < peakMask.rows; ++y) {
            const uchar* pRow = peakMask.ptr<uchar>(y);
            const int*   lRow = pkLabels.ptr<int>(y);
            const float* dRow = dist.ptr<float>(y);
            for (int x = 0; x < peakMask.cols; ++x) {
                if (!pRow[x])
                    continue;
                const int    lb = lRow[x];
                const double v  = dRow[x];
                if (v > bestV[lb]) {
                    bestV[lb] = v;
                    bestP[lb] = cv::Point(x, y);
                }
            }
        }
        for (int i = 1; i < pkCount; ++i) {
            if (bestV[i] > 0.0)
                cand.push_back(Peak{bestV[i],
                                    static_cast<float>(bestP[i].x),
                                    static_cast<float>(bestP[i].y)});
        }
    }

    // ---- ③ 尺度自适应 NMS：强度降序；间距 < minSep 或强度 < minPeak 者丢弃 ----
    std::sort(cand.begin(), cand.end(),
              [](const Peak& a, const Peak& b) { return a.strength > b.strength; });
    const double sep    = kSplitSepRatio * dmax;
    const double floorV = kSplitPeakRatio * dmax;

    std::vector<cv::Point2f> kept;   // ⚠ 裁剪坐标——NMS 必须与候选同坐标系，最后再转绝对
    for (const auto& c : cand) {
        if (c.strength < floorV)
            continue;
        bool keep = true;
        for (const auto& m : kept) {
            const double dx = c.x - m.x;
            const double dy = c.y - m.y;
            if (dx * dx + dy * dy < sep * sep) {
                keep = false;
                break;
            }
        }
        if (keep)
            kept.emplace_back(c.x, c.y);
    }

    markers.reserve(kept.size());
    for (const auto& m : kept)
        markers.emplace_back(m.x + static_cast<float>(clipped.x),
                             m.y + static_cast<float>(clipped.y));
    return markers;
}

/// 按「**最近峰归属**」（Voronoi）把组件掩码切成 `markers.size()` 份（TR-135）。
///
/// 无参数、确定性：每个组件像素归给它最近的峰。三球合并域下，分界落在两球心的垂直平分
/// 面上 ⇒ 各子区域 ≈ 各球的可见 footprint（重叠部分归「更近的那个峰」，符合直觉）。
///
/// 输出 `subMasks[i]` 与图像**同尺寸**（CV_8UC1），`subBoxes[i]` 为对应紧包围框
/// （空区域 = 宽高 0）。
void VoronoiSplit(const cv::Mat& compMask, const cv::Rect& bbox,
                  const std::vector<cv::Point2f>& markers,
                  std::vector<cv::Mat>& subMasks, std::vector<cv::Rect>& subBoxes)
{
    const size_t n = markers.size();
    // ⚠ 必须逐个 `zeros`——`cv::Mat` 拷贝是**浅拷贝**，用 `assign(n, Mat)` 会让 n 份共享
    //   同一块数据（写第 i 份等于写全部）。
    subMasks.clear();
    subMasks.reserve(n);
    for (size_t i = 0; i < n; ++i)
        subMasks.push_back(cv::Mat::zeros(compMask.size(), CV_8UC1));
    subBoxes.assign(n, cv::Rect());

    const int intMax = std::numeric_limits<int>::max();
    std::vector<int> minX(n, intMax), minY(n, intMax), maxX(n, -1), maxY(n, -1);

    const cv::Rect imageRect(0, 0, compMask.cols, compMask.rows);
    const cv::Rect clipped = bbox & imageRect;

    for (int y = clipped.y; y < clipped.y + clipped.height; ++y) {
        const uchar* srcRow = compMask.ptr<uchar>(y);
        for (int x = clipped.x; x < clipped.x + clipped.width; ++x) {
            if (!srcRow[x])
                continue;
            size_t best  = 0;
            double bestD = std::numeric_limits<double>::max();
            for (size_t i = 0; i < n; ++i) {
                const double dx = x - markers[i].x;
                const double dy = y - markers[i].y;
                const double d  = dx * dx + dy * dy;
                if (d < bestD) {
                    bestD = d;
                    best  = i;
                }
            }
            subMasks[best].ptr<uchar>(y)[x] = 255;
            if (x < minX[best]) minX[best] = x;
            if (x > maxX[best]) maxX[best] = x;
            if (y < minY[best]) minY[best] = y;
            if (y > maxY[best]) maxY[best] = y;
        }
    }

    for (size_t i = 0; i < n; ++i) {
        if (maxX[i] < minX[i] || maxY[i] < minY[i])
            continue;
        subBoxes[i] = cv::Rect(minX[i], minY[i],
                               maxX[i] - minX[i] + 1, maxY[i] - minY[i] + 1);
    }
}

/// 球顶面片提取（TR-132，2026-10-16）。
///
/// 取**区域掩码内**深度最小的 `apexPercentile` 比例的像素，其质心 ⇒ `apex`（⇒ `pixelU`/`pixelV`）、
/// 其均值深度 ⇒ `apexDepthMm`（⇒ `z`）。**必须是同一个物理点**，否则 `PixelToRobot(u,v,z)`
/// 反投影出一个不在目标表面上的点（真机实测：质心与球顶相距 ~47 px ≈ 23 mm）。
///
/// ⚠ 用**比例**而非「绝对毫米带」：比例自适应连通域大小与深度梯度，换球径/换安装高度无需重标定。
/// ⚠ **不能**直接用 `minDepthMm` 所在像素——单像素极小值可能是噪点。实测 `depth_20261016103952_0`
///    的球 #1：`min = 516 mm`，但该连通域 0.5% 分位 = `522 mm`、`min + 3mm` 带内只有 **7** 个像素
///    （另一球同带有 818 个）⇒ 直接取 min 会让球顶偏 6 mm 且被噪点牵引。
///
/// ⚠ TR-135：入参由 `(labels, label)` 改为 `regionMask`——同一条测量路径要同时服务**整个连通域**
///    （未切分时）与**峰切分后的子区域**（三球挤在一起时）。
void ComputeApex(const cv::Mat& depthMm, const cv::Mat& regionMask, const cv::Rect& clipped,
                 std::vector<float>& values, const AlgorithmParams& p, Candidate& c)
{
    if (values.empty()) {
        c.apex        = c.center;
        c.apexDepthMm = c.meanDepthMm;
        c.apexCount   = 0;
        return;
    }

    const double pct = std::clamp(p.apexPercentile, kApexPercentileMin, kApexPercentileMax);
    const size_t n   = values.size();
    // 面片至少 `kApexMinPixels` 个像素——小连通域（minArea = 120 ⇒ 2% 只有 2 个像素）下
    // 质心会被单点噪声主导。下限与比例取较大者。
    const size_t nthWanted = static_cast<size_t>(static_cast<double>(n - 1) * pct);
    const size_t nthFloor  = static_cast<size_t>(std::min<size_t>(n - 1, kApexMinPixels - 1));
    size_t nth = std::min(n - 1, std::max(nthWanted, nthFloor));

    // 第 `nth` 小的深度 = 面片深度上限。`nth_element` 平均线性，不必整段排序。
    std::nth_element(values.begin(),
                     values.begin() + static_cast<std::ptrdiff_t>(nth),
                     values.end());
    const double cutDepth = values[nth];

    double sumX = 0.0;
    double sumY = 0.0;
    double sumD = 0.0;
    int    apexN = 0;
    for (int y = clipped.y; y < clipped.y + clipped.height; ++y) {
        const uchar* maskRow  = regionMask.ptr<uchar>(y);
        const float* depthRow = depthMm.ptr<float>(y);
        for (int x = clipped.x; x < clipped.x + clipped.width; ++x) {
            if (!maskRow[x])
                continue;
            const double value = depthRow[x];
            if (!std::isfinite(value) || value < p.depthZMin || value > p.depthZMax)
                continue;
            if (value > cutDepth)
                continue;
            sumX += x;
            sumY += y;
            sumD += value;
            ++apexN;
        }
    }

    if (apexN > 0) {
        c.apex        = cv::Point2f(static_cast<float>(sumX / apexN),
                                    static_cast<float>(sumY / apexN));
        c.apexDepthMm = sumD / apexN;
        c.apexCount   = apexN;
    } else {
        // 退化兜底（面片为空，理论上不可达：count ≥ minValidCount ≥ 10 且 pct ≥ 0.005）
        // ⇒ 退回连通域质心 + 均值深度，并把 `apexCount = 0` 暴露给日志。
        c.apex        = c.center;
        c.apexDepthMm = c.meanDepthMm;
        c.apexCount   = 0;
    }
}

/// 对**单个区域掩码**完成全部测量并填充 `Candidate`（TR-135 抽出，供「整连通域」与「峰切分子区域」
/// 共用同一条测量路径）。
///
/// 返回 `false` = 未过过滤（有效像素不足 / 质心落在边缘带）；此时 `c` 内容未定义。
///
/// ⚠ `regionMask` 与图像**同尺寸**（255 = 属于该区域），只在 `bbox` 内遍历；
///   `bbox` 须为区域的**紧包围框**。
/// ⚠ `area` 直接取区域像素数：`topMask ⊆ validMask`（掩码已保证深度有限且落在 `[zMin, zMax]`）
///   ⇒ 区域像素**恒**有有效深度，与 `connectedComponentsWithStats` 的 `CC_STAT_AREA` 恒等。
bool MeasureRegion(const cv::Mat& depthMm, const cv::Mat& regionMask, const cv::Rect& bbox,
                   const cv::Rect& safeRect, double nearestDepthMm,
                   const AlgorithmParams& p, Candidate& c)
{
    const cv::Rect imageRect(0, 0, depthMm.cols, depthMm.rows);
    const cv::Rect clipped = bbox & imageRect;
    if (clipped.width <= 0 || clipped.height <= 0)
        return false;

    double sum = 0.0;
    double sumSq = 0.0;
    double minDepth = std::numeric_limits<double>::max();
    double sumX = 0.0;
    double sumY = 0.0;
    int count = 0;
    std::vector<float> values;   // 有效深度值（TR-132：供球顶分位选取）
    values.reserve(static_cast<size_t>(clipped.width) * static_cast<size_t>(clipped.height));

    for (int y = clipped.y; y < clipped.y + clipped.height; ++y) {
        const uchar* maskRow  = regionMask.ptr<uchar>(y);
        const float* depthRow = depthMm.ptr<float>(y);
        for (int x = clipped.x; x < clipped.x + clipped.width; ++x) {
            if (!maskRow[x])
                continue;

            const double value = depthRow[x];
            if (!std::isfinite(value) || value < p.depthZMin || value > p.depthZMax)
                continue;

            sum += value;
            sumSq += value * value;
            minDepth = std::min(minDepth, value);
            sumX += x;
            sumY += y;
            values.push_back(static_cast<float>(value));
            ++count;
        }
    }

    const int minValidCount = std::max(kMinValidPixelsFloor, p.minArea / kMinValidPixelsDiv);
    if (count < minValidCount)
        return false;

    const cv::Point2f center(static_cast<float>(sumX / count), static_cast<float>(sumY / count));

    // 边缘过滤：质心落在 [margin, 边长−margin] 之外则丢弃
    if (!safeRect.contains(cv::Point(static_cast<int>(center.x),
                                     static_cast<int>(center.y))))
        return false;

    const double mean = sum / count;
    const double variance = std::max(0.0, sumSq / count - mean * mean);
    const double stdDev = std::sqrt(variance);

    c.bbox        = bbox;
    c.center      = center;
    c.area        = count;
    c.meanDepthMm = mean;
    c.minDepthMm  = minDepth;

    ComputeApex(depthMm, regionMask, clipped, values, p, c);

    const double confidence = EstimateConfidence(count, stdDev, center, depthMm.size(), p);
    const double nearScore  = 1.0 / (1.0 + std::max(0.0, mean - nearestDepthMm));
    const double areaScore  = CalculateAreaScore(count, p);
    c.score = confidence * kScoreConfWeight +
              nearScore * kScoreNearWeight +
              areaScore * kScoreAreaWeight;
    return true;
}

/// ③ 遍历**全部**连通域，硬过滤后按**球顶深度升序**返回（`front()` = 最高）。
///
/// ⚠ 与老实现的差异：老实现只保留 `argmax` 一个（边扫边比），本仓 `Detect` 返回
/// `vector<PuffResult>`，故改为全量产出 + **显式排序**（契约 §11.3 约定 1）。
///
/// ⚠ TR-135（2026-10-16）：一个连通域**不一定**只有一个目标——三球挤在一起时 `MORPH_CLOSE`
///   会把它们桥接成单域。故先做**组件内峰切分**（`FindMarkers` + `VoronoiSplit`），切出
///   ≥ `kSplitMinMarkers` 个合格子目标时按子目标逐个产出，否则**回退**组件级单目标
///   （⇒ 单球行为与 TR-135 之前**逐位一致**）。
std::vector<Candidate> SelectRegions(const cv::Mat& depthMm, const cv::Mat& topMask,
                                     double nearestDepthMm, const AlgorithmParams& p)
{
    cv::Mat labels, stats, centroids;
    const int componentCount = cv::connectedComponentsWithStats(
        topMask, labels, stats, centroids, 8, CV_32S);

    std::vector<Candidate> out;
    if (componentCount <= 1)
        return out;

    const cv::Rect safeRect(
        p.edgeMargin,
        p.edgeMargin,
        std::max(0, depthMm.cols - p.edgeMargin * 2),
        std::max(0, depthMm.rows - p.edgeMargin * 2));

    for (int label = 1; label < componentCount; ++label) {
        const int area = stats.at<int>(label, cv::CC_STAT_AREA);
        if (area < p.minArea || area > p.maxArea)
            continue;

        const cv::Rect bbox(stats.at<int>(label, cv::CC_STAT_LEFT),
                            stats.at<int>(label, cv::CC_STAT_TOP),
                            stats.at<int>(label, cv::CC_STAT_WIDTH),
                            stats.at<int>(label, cv::CC_STAT_HEIGHT));

        const cv::Point2f center(static_cast<float>(centroids.at<double>(label, 0)),
                                 static_cast<float>(centroids.at<double>(label, 1)));

        // 边缘过滤：质心落在 [margin, 边长−margin] 之外则丢弃
        if (!safeRect.contains(cv::Point(static_cast<int>(center.x),
                                         static_cast<int>(center.y))))
            continue;

        // 组件掩码（与图像同尺寸）。`labels == label` 的比较结果即 255/0 的 CV_8U。
        const cv::Mat compMask = (labels == label);

        // ---- TR-135（2026-10-16）：组件内峰切分。三球挤在一起时 `MORPH_CLOSE` 把它们
        //      桥接成单域，组件级只会报 1 个目标 ⇒ 先试切分，成功则按子目标逐个产出。----
        const std::vector<cv::Point2f> markers = FindMarkers(compMask, bbox);
        if (static_cast<int>(markers.size()) >= kSplitMinMarkers) {
            std::vector<cv::Mat>  subMasks;
            std::vector<cv::Rect> subBoxes;
            VoronoiSplit(compMask, bbox, markers, subMasks, subBoxes);

            std::vector<Candidate> subs;
            subs.reserve(subMasks.size());
            for (size_t i = 0; i < subMasks.size(); ++i) {
                if (subBoxes[i].width <= 0 || subBoxes[i].height <= 0)
                    continue;   // 空子区域（该峰未分到像素）
                if (cv::countNonZero(subMasks[i](subBoxes[i])) < p.minArea)
                    continue;   // 子目标同样过面积下限——滤掉切分噪声
                Candidate sc;
                if (MeasureRegion(depthMm, subMasks[i], subBoxes[i], safeRect,
                                  nearestDepthMm, p, sc)) {
                    sc.label = label;
                    subs.push_back(sc);
                }
            }

            // 切出 ≥ `kSplitMinMarkers` 个**合格**子目标才算成功；否则**回退**组件级单目标
            // （⇒ 单球 / 切分失败时的行为与 TR-135 之前**逐位一致**）。
            if (static_cast<int>(subs.size()) >= kSplitMinMarkers) {
                SPDLOG_INFO("[DepthPuffAlgo] component #{} area={} → {} sub-target(s) "
                            "(markers={}; sep={:.2f}×dmax peak={:.2f}×dmax)",
                            label, area, subs.size(), markers.size(),
                            kSplitSepRatio, kSplitPeakRatio);
                out.insert(out.end(), subs.begin(), subs.end());
                continue;
            }
        }

        Candidate c;
        c.label = label;
        if (MeasureRegion(depthMm, compMask, bbox, safeRect, nearestDepthMm, p, c))
            out.push_back(c);
    }

    // ⚠ 排序键 = **球顶深度优先**（`apexDepthMm` 升序 = 越高越靠前），`score` 只作并列时的次级键。
    //   抓取语义下「最佳」= **最高**——夹爪自上方下探，最高者可达且不与其它目标干涉；
    //   这与 `PuffResult.z`（= `apexDepthMm`）同源。
    //   ⚠ TR-131（2026-10-16）：原按 `score` 降序，而 `score` 里的 `centerScore` 偏好**画面中心**、
    //   与「最高」无关 ⇒ 目标偏离画面中心时，**较低的球**可能被排到 `front()`。实测坐实：两球中
    //   较低的那个 `centerScore` 反而更高（0.756 > 0.718），仅因 `nearScore` 权重更大才未暴露。
    //   ⚠ TR-132：键由**原始最小深度** `minDepthMm` 改为**球顶面片均值** `apexDepthMm`——原始极小
    //   值受单像素噪点影响（实测某球 min 比其 0.5% 分位低 6 mm），会让「谁更高」判错。
    std::stable_sort(out.begin(), out.end(),
                     [](const Candidate& a, const Candidate& b) {
                         if (a.apexDepthMm != b.apexDepthMm)
                             return a.apexDepthMm < b.apexDepthMm;   // 球顶深度小 = 高 = 优先
                         return a.score > b.score;                    // 球顶深度并列时取质量更高者
                     });
    return out;
}

} // namespace

std::string DepthPuffAlgo::Name() const
{
    return "DepthPuffAlgo";
}

AlgorithmParams DepthPuffAlgo::SnapshotParams() const
{
    // ⚠ 主线程专用：这里读 `ConfigManager`（`json` 读写非线程安全）。
    //
    // 覆盖语义（§十 裁定 3）= **算法节点优先、通用域作默认**：先取 `LoadConfig` 解析出的
    // 兜底值，再用 config.json 的实时值覆盖。键缺失时 `getValue` 返回传入的默认值。
    //
    // ⚠ `getValue<int>` 走 `json::get<int>()`，**类型不匹配会抛异常并被吞掉**（返回默认值）
    // ⇒ config.json 里的整数量（`minArea` / `roi.x` …）**必须写成整数**，写 `120.0` 会静默失效。
    AlgorithmParams p = fallback_;

    const auto& cfg = ConfigManager::instance();
    const std::string base = "vision.algorithms.DepthPuffAlgo.";

    // ⚠ 通用三键也走**算法节点优先**（§十 裁定 3），与 `SimAlgo::SnapshotParams` 同一形态：
    //   先查 `vision.algorithms.DepthPuffAlgo.<键>`，缺则回落通用域 `vision.<键>`，再缺才用
    //   `p`（= `fallback_`：LoadConfig 从节点解析出的值，或结构体内置默认）兜底。
    //   ⚠ 顺序写反会**静默失效**——`vision.confidenceThreshold` / `depthZMin` / `depthZMax`
    //     恒存在于 config.json，若先读通用域，节点里的同名覆盖键将永远读不到。
    p.confidenceThreshold = cfg.getValue<double>(base + "confidenceThreshold",
                            cfg.getValue<double>("vision.confidenceThreshold", p.confidenceThreshold));
    p.depthZMin = cfg.getValue<double>(base + "depthZMin",
                            cfg.getValue<double>("vision.depthZMin", p.depthZMin));
    p.depthZMax = cfg.getValue<double>(base + "depthZMax",
                            cfg.getValue<double>("vision.depthZMax", p.depthZMax));

    p.nearestPercentile   = cfg.getValue<double>(base + "nearestPercentile",   p.nearestPercentile);
    p.heightBandMm        = cfg.getValue<double>(base + "heightBandMm",        p.heightBandMm);
    p.apexPercentile      = cfg.getValue<double>(base + "apexPercentile",      p.apexPercentile);
    p.minArea             = cfg.getValue<int>(base + "minArea",                p.minArea);
    p.maxArea             = cfg.getValue<int>(base + "maxArea",                p.maxArea);
    p.targetArea          = cfg.getValue<int>(base + "targetArea",             p.targetArea);
    p.targetAreaTolerance = cfg.getValue<int>(base + "targetAreaTolerance",    p.targetAreaTolerance);
    p.morphKernelSize     = cfg.getValue<int>(base + "morphKernelSize",        p.morphKernelSize);
    p.edgeMargin          = cfg.getValue<int>(base + "edgeMargin",             p.edgeMargin);

    p.roi.x = cfg.getValue<int>(base + "roi.x", p.roi.x);
    p.roi.y = cfg.getValue<int>(base + "roi.y", p.roi.y);
    p.roi.w = cfg.getValue<int>(base + "roi.w", p.roi.w);
    p.roi.h = cfg.getValue<int>(base + "roi.h", p.roi.h);

    return p;
}

std::vector<PuffResult> DepthPuffAlgo::Detect(const CameraFrame& frame, const AlgorithmParams& params)
{
    // 串行化：视觉页检测线程与 `SequenceWorker` 识别动作可能同时在跑，二者共用一个实例。
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<PuffResult> results;
    lastError_.clear();

    // ---- 深度图尺寸（TR-116）：必须按 `depthWidth`/`depthHeight`（**0 = 回落彩色**）----
    // 改造前用彩色 `width` 索引 `depthData`，D2C 关闭时越界读 → 0xC0000005。
    const int dw = (frame.depthWidth  > 0) ? frame.depthWidth  : frame.width;
    const int dh = (frame.depthHeight > 0) ? frame.depthHeight : frame.height;
    if (dw <= 0 || dh <= 0) {
        lastError_ = "invalid depth dimensions";
        return results;
    }
    const size_t need = static_cast<size_t>(dw) * static_cast<size_t>(dh);
    if (frame.depthData.size() < need) {
        lastError_ = "depth data smaller than depthWidth x depthHeight";
        SPDLOG_WARN("[DepthPuffAlgo] depthData.size()={} < depth {}x{} ({}) → skip frame",
                    frame.depthData.size(), dw, dh, need);
        return results;
    }

    // 深度已是 `float` mm（`ICamera.h:40`；`OrbbecCamera` 用 `getValueScale()` 转好）
    // ⇒ 老实现的 `depthScaleToMm` **不再需要**（移植方案 §五 1.3）。
    // 零拷贝**只读**包装：`cv::Mat` 不持有数据、不释放，本函数全程不写该缓冲。
    const cv::Mat depthMm(dh, dw, CV_32FC1, const_cast<float*>(frame.depthData.data()));

    const cv::Rect roi = EffectiveRoi(params.roi, depthMm.size());

    cv::Mat validMask;
    if (BuildValidMask(depthMm, roi, params.depthZMin, params.depthZMax, validMask) <= 0) {
        lastError_ = "no valid depth pixels in ROI";
        return results;
    }

    cv::Mat topMask;
    double nearestDepthMm = 0.0;
    if (!BuildTopRegionMask(depthMm, validMask, params, topMask, nearestDepthMm)) {
        lastError_ = "no top-region candidates";
        return results;
    }

    const std::vector<Candidate> cands = SelectRegions(depthMm, topMask, nearestDepthMm, params);
    if (cands.empty()) {
        lastError_ = "no candidate passed filtering";
        return results;
    }

    // ---- 坐标系归一：**深度域 → 彩色域**（契约 §11.3）----
    // 算法在深度域做连通域，而 `pixelU`/`pixelV` 要喂 `OrbbecCamera` 的**彩色**内参。
    // D2C = `SW` 时两分辨率相同、映射退化为恒等；D2C 关闭时必须生效，否则像素喂错内参。
    const int cw = (frame.width  > 0) ? frame.width  : dw;
    const int ch = (frame.height > 0) ? frame.height : dh;
    const bool needScale = (cw != dw || ch != dh);
    if (needScale && !depthAlignWarned_) {
        depthAlignWarned_ = true;
        SPDLOG_WARN("[DepthPuffAlgo] 深度 {}x{} 与彩色 {}x{} 分辨率不同（D2C 关闭）→ "
                    "像素输出按比例映射，非像素级对应（契约 §11.3）", dw, dh, cw, ch);
    }
    const double sx = needScale ? static_cast<double>(cw) / dw : 1.0;
    const double sy = needScale ? static_cast<double>(ch) / dh : 1.0;

    results.reserve(cands.size());
    for (const auto& c : cands) {
        PuffResult res;

        // 契约 §11.3（TR-132）：`pixelU`/`pixelV` = **球顶**，与 `z` 同源。
        // ⚠ 不是连通域质心——真机实测二者相距 ~47 px（≈23 mm）；若用质心配球顶深度，
        //   `PixelToRobot` 会反投影出一个**不在目标表面上**的点。
        res.pixelU = static_cast<int>(std::lround(c.apex.x * sx));
        res.pixelV = static_cast<int>(std::lround(c.apex.y * sy));

        // 包围框**独立给出原点**：球顶 ≠ 框中心，只给「中心 + 尺寸」画不出框（TR-132）。
        res.bboxX  = static_cast<int>(std::lround(c.bbox.x      * sx));
        res.bboxY  = static_cast<int>(std::lround(c.bbox.y      * sy));
        res.width  = static_cast<int>(std::lround(c.bbox.width  * sx));
        res.height = static_cast<int>(std::lround(c.bbox.height * sy));

        // 契约 §11.3：`z` = **球顶**深度（抓取应取最高点），**不是**区域均值。
        // ⚠ 取球顶**面片均值**而非单像素极小值——后者受噪点影响（实测某球 min 偏低 6 mm）。
        res.z          = c.apexDepthMm;
        res.minDepthMm = c.minDepthMm;   // 保留原始极小值供交叉校验（与 z 之差 = 噪点幅度）

        // `area` 的坐标系 = **深度图**（不随像素坐标归一），见 §11.3 与 M2。
        res.area = static_cast<double>(c.area);

        // ⚠ 保真老实现：结果的 `confidence` 存的是三权重综合 `score` 的 clamp 值，
        // **不是** `EstimateConfidence` 的单独输出（后者只是 score 的输入项）。
        res.confidence = std::clamp(c.score, 0.0, 1.0);

        // `x` / `y` 弃用（§十 裁定 2）：保留字段、恒 0；基座坐标由下游 `PixelToRobot` 算。
        res.x   = 0.0;
        res.y   = 0.0;
        res.yaw = 0.0;

        results.push_back(res);
    }

    {
        const Candidate& best = cands.front();
        // ⚠ `topDepth` = 原始最小深度（保留原名，兼容既有 grep 配方）；
        //   `apexDepth` = 球顶面片均值 = 真正写进 `z` 的值；二者之差 = 该连通域的噪点幅度。
        SPDLOG_INFO("[DepthPuffAlgo] {} candidate(s); best area={} meanDepth={:.1f}mm "
                    "topDepth={:.1f}mm apexDepth={:.1f}mm apex=({:.0f},{:.0f})[{}px] score={:.3f}; "
                    "nearestDepth={:.1f}mm zRange=[{:.1f},{:.1f}]",
                    cands.size(), best.area, best.meanDepthMm, best.minDepthMm,
                    best.apexDepthMm, best.apex.x, best.apex.y, best.apexCount, best.score,
                    nearestDepthMm, params.depthZMin, params.depthZMax);
    }

    // 置信度过滤：低于阈值**丢弃**（契约 §11.3 约定 3「过滤即丢弃」，TR-103 语义）。
    // ⚠ 两套算法的 `confidence` 语义不同（`SimAlgo` 是面积覆盖率、本算法是加权综合分）
    // ⇒ 同一个 `confidenceThreshold` 在两者上的松紧并不等价（移植方案 §七 M1）。
    const double thr = params.confidenceThreshold;
    if (thr > 0.0) {
        const auto before = results.size();
        results.erase(std::remove_if(results.begin(), results.end(),
                                     [thr](const PuffResult& r) { return r.confidence < thr; }),
                      results.end());
        if (results.size() != before) {
            SPDLOG_INFO("[DepthPuffAlgo] confidenceThreshold={:.3f}: filtered {} of {} detection(s)",
                        thr, before - results.size(), before);
        }
    }

    return results;
}

PuffResult DepthPuffAlgo::LocateBest(const CameraFrame& frame, const AlgorithmParams& params)
{
    // ⚠ 与 `Detect().front()` **同源**（TR-131，2026-10-16）。原实现取 `confidence` 最大者，
    // 与 `SelectRegions` 的排序键（现为「深度优先」）**不一致** ⇒ 同一帧上两个入口会给出不同
    // 目标。本方法当前**全仓无调用者**（`grep -rn "LocateBest" src/` 只有定义），保留为接口
    // 占位；语义仍须与 `Detect` 对齐，否则将来一旦被接线就会引入第二套「最佳」定义。
    const std::vector<PuffResult> results = Detect(frame, params);
    return results.empty() ? PuffResult{} : results.front();
}

bool DepthPuffAlgo::LoadConfig(const std::string& jsonConfig)
{
    config_ = jsonConfig;

    // 认整棵子树（C5）。调用方（C1）透传的是 `vision.algorithms.<algoType>` 整块；
    // 节点缺失时传进来的是 `{}` ⇒ 保留结构体内置默认值，**不报错**（§十 裁定 1）。
    if (jsonConfig.empty() || jsonConfig == "{}") {
        SPDLOG_INFO("[DepthPuffAlgo] config empty ({} bytes) → built-in defaults", config_.size());
        return true;
    }

    try {
        const auto j = nlohmann::json::parse(jsonConfig);
        if (!j.is_object()) {
            SPDLOG_WARN("[DepthPuffAlgo] config is not an object ({} bytes) → ignored", config_.size());
            return true;
        }

        // 解析出的值只作**兜底**；运行时以 `SnapshotParams()` 读 config.json 的实时值为准。
        auto takeD = [&j](const char* key, double& dst) {
            if (j.contains(key) && j[key].is_number())
                dst = j[key].get<double>();
        };
        auto takeI = [&j](const char* key, int& dst) {
            if (j.contains(key) && j[key].is_number_integer())
                dst = j[key].get<int>();
        };

        takeD("confidenceThreshold", fallback_.confidenceThreshold);
        takeD("depthZMin", fallback_.depthZMin);
        takeD("depthZMax", fallback_.depthZMax);
        takeD("nearestPercentile", fallback_.nearestPercentile);
        takeD("heightBandMm", fallback_.heightBandMm);
        takeD("apexPercentile", fallback_.apexPercentile);
        takeI("minArea", fallback_.minArea);
        takeI("maxArea", fallback_.maxArea);
        takeI("targetArea", fallback_.targetArea);
        takeI("targetAreaTolerance", fallback_.targetAreaTolerance);
        takeI("morphKernelSize", fallback_.morphKernelSize);
        takeI("edgeMargin", fallback_.edgeMargin);

        if (j.contains("roi") && j["roi"].is_object()) {
            const auto& r = j["roi"];
            auto takeRi = [&r](const char* key, int& dst) {
                if (r.contains(key) && r[key].is_number_integer())
                    dst = r[key].get<int>();
            };
            takeRi("x", fallback_.roi.x);
            takeRi("y", fallback_.roi.y);
            takeRi("w", fallback_.roi.w);
            takeRi("h", fallback_.roi.h);
        }

        SPDLOG_INFO("[DepthPuffAlgo] config loaded ({} bytes): nearestPercentile={:.3f} "
                    "heightBandMm={:.1f} apexPercentile={:.3f} minArea={} maxArea={} "
                    "morphKernelSize={} edgeMargin={} targetArea={} roi=({},{},{},{})",
                    config_.size(), fallback_.nearestPercentile, fallback_.heightBandMm,
                    fallback_.apexPercentile, fallback_.minArea, fallback_.maxArea,
                    fallback_.morphKernelSize, fallback_.edgeMargin, fallback_.targetArea,
                    fallback_.roi.x, fallback_.roi.y, fallback_.roi.w, fallback_.roi.h);
        return true;
    } catch (const std::exception& e) {
        SPDLOG_WARN("[DepthPuffAlgo] LoadConfig parse error: {} (raw {} bytes)", e.what(), config_.size());
        return false;
    }
}

std::string DepthPuffAlgo::GetLastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}
