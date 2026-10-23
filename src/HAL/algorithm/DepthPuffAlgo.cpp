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
constexpr double kNearestPercentileMax = 0.005;   // 上钳位（TR-144：再大就会锚到背景面）
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

// ---- 种子（TR-144，2026-10-20）：顶层带厚的锚点 ----
// 同一层目标的顶点高度容差；实测同一桌面的泡芙顶点相差 ≤ 9 mm，而背景面比顶点低 ≥ 19 mm。
constexpr double kSeedBandMm      = 10.0;
constexpr int    kSeedMinPixels   = 20;   // 种子连通域最少像素（滤单像素噪点）
constexpr int    kSeedMorphKernel = 3;    // 种子开运算核（只开不闭，避免把相邻顶点桥接）

// ---- 连通域级锚点（TR-145，2026-10-21）：近→远逐层抬高截断深度 ----
// 原始 `validMask` 里目标与承载面在 2D 上连成一片 ⇒ 单次连通域永远是贴边界的大域，
// 面积/边界过滤后一无所获。必须**从近到远抬高截断深度**，目标会在与背景合并之前先
// 长成一个面积合格的独立连通域。
constexpr double kAnchorStepMm    = 2.0;  // 截断深度步长（锚点最终取实际最低深度，不受步长量化）
constexpr int    kAnchorFineIters = 8;    // 前 N 次用细步长（近处目标优先）
constexpr double kAnchorCoarseStepMm = 10.0;  // 之后换粗步长（快速越过噪声带，见下）
constexpr int    kAnchorMaxIters  = 64;   // 迭代上限（8×2 + 56×10 = 576 mm 行程）

// **TR-151（2026-10-21）多锚点**：`NextAnchor` 逐个产出合格锚点，`Detect` 对每个锚点各跑
// 一遍「种子 → 生长 → 过滤」，取第一个出结果的。三条配套常量：
constexpr int    kAnchorMaxAttempts   = 8;    // 单帧最多尝试的锚点数（防病态场景无限扫描）
constexpr double kAnchorNearClipTolMm = 1.0;  // 近端截断判据：minDepth ≤ zMin + 此值 ⇒ 不作锚点
constexpr double kAnchorDedupMm       = 2.0;  // 锚点去重容差（同一面片在相邻层重复出现）

// **TR-149（2026-10-21）起点与步长**：原实现从「ROI 内全局最小有效深度」起步、恒定 2 mm
// 步长 ⇒ 迭代预算 128 mm 全部耗在噪声带上。实测 `depth_20261021135309_0`（球顶 586 mm）：
// ROI 里 430~480 mm 有 300 余个零星噪声像素，`lo=430`，球顶需要 78 次迭代 > 上限 64 ⇒
// 锚点永远够不到球 ⇒ `no seeds in ROI`。
// 修法两条：① 起点改用 `nearestPercentile` 分位（分位**只定起点**，锚点仍由面积 + 边界
// 过滤后的连通面片给出，不承担 TR-144 的「锚点」职责）；② 细步长 8 次后转 10 mm 粗步长
// —— 泡芙球顶到承载面的深度跨度实测 ≥ 36 mm，粗步长不会跨过整个目标。

// ---- 目标几何上限（TR-147/148，2026-10-21）：挡「近处大平面被当目标」----
// 深度法没有外观信息，场景里比球更近的大平面（A4 纸叠、桌面残片）会被锚点正常捕获——
// 「最近物体」语义下这是**正确行为**，错在把整个平面报成目标。几何判据：候选包围框
// 最长边(px) × 球顶深度(mm) ≈ 物理尺寸 × fx，**对工作距离不敏感**（px 随距离缩、深度
// 随距离涨，乘积守恒）。实测真机（fx≈1037）：单球 ≤ 105px×530 ≈ 5.6 万；三球合并域
// 157px×550 ≈ 8.6 万；A4 纸叠（占 ROI 44% 的 567~573 mm 平面）切出的假目标 ≥
// 305px×568 ≈ 17.3 万。
// **TR-148（2026-10-21）配置化**：`maxExtentProduct`（`vision.algorithms.DepthPuffAlgo.
// maxExtentProduct`，默认 130000，**≤ 0 = 禁用**）；此处不再有硬编码常量。标定依据与
// 换安装高度 / 球径的重核算要求见 `TEST_RECORD.md` TR-147。

/// 单个种子（**深度域**坐标）：最近层里的一块连通面片 = 一个目标的顶点。
struct Seed
{
    int         label = 0;
    cv::Rect    bbox;
    cv::Point2f center;              // 质心——用于在候选掩码里定位所属连通域
    double      minDepthMm = 0.0;    // 面片最低深度 = 该目标的顶点深度
};

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

/// ①b 锚点枚举器（TR-145 逐层扫描 + **TR-151 不早退**）。
///
/// **为什么必须从近到远抬高截断深度**：`validMask` 里目标与承载面在 2D 上连成一片，
/// 直接做一次连通域永远是「贴 ROI 边界的巨大连通域」，面积/边界过滤后一无所获。
/// 从 `lo` 起步、每次抬高截断深度，目标会在与背景合并之前先长成一个面积合格的独立
/// 连通域。
///
/// **为什么不能「首个合格面片即返回」（TR-151，2026-10-21）**：TR-145 一找到合格面片就
/// 把锚点定死，于是**任何**比目标更近、又不贴 ROI 边界的物体都能否决整帧。实测
/// `depth_20261021150749_0` 等 5 帧：机械臂末端（白色连杆 + 黑色夹爪）侵入 ROI 右下角
/// `x1631~1650 / y647~670`，深度 421~433 mm，被 `depthZMin=430` 切成一层 3 mm 厚的
/// **近端截断薄壳**；这层壳与 ROI 边界是否 8-连通只差 5~6 px，帧间深度噪声一抖就断开
/// ⇒ 独立小块 123~244 px > `minArea=120` ⇒ 锚点落在 430 ⇒ 种子带 430~440 够不到球顶
/// 584 ⇒ 只长出那块小面片（conf 0.42~0.47 < 0.65）⇒「未检出」。同一场景连拍 8 帧，
/// 5 帧漏检 / 3 帧正常，**差别只在这块薄壳有没有断开**（11 帧 100% 相关）。
/// 现在改为**逐个产出**全部合格锚点，调用方取「第一个能出过阈值目标的锚点」。
///
/// **近端截断守卫（TR-151）**：连通域最低深度 ≤ `depthZMin + kAnchorNearClipTolMm`
/// ⇒ 该面片被近端量程切掉，真实深度与真实面积都不可知，一律不作锚点。
struct AnchorSweep
{
    cv::Mat depthRoi;                 // ROI 视图（共享像素，不拷贝）
    cv::Mat validRoi;
    int     width   = 0;
    int     height  = 0;
    double  lo      = 0.0;            // 起点 = `nearestPercentile` 分位（TR-149）
    double  hi      = 0.0;            // ROI 内最大有效深度
    double  zMin    = 0.0;
    int     minArea = 0;
    int     edge    = 0;

    int     it         = 0;           // 下一个待算层号
    bool    finished   = false;
    size_t  pendingPos = 0;
    std::vector<double> pending;      // 当前层待产出（升序）
    std::vector<double> yielded;      // 已产出（按深度去重）
};

/// 初始化枚举器。返回 false = ROI 内无有效像素。
bool InitAnchorSweep(const cv::Mat& depthMm, const cv::Mat& validMask, const cv::Rect& roi,
                     const AlgorithmParams& p, AnchorSweep& s)
{
    if (roi.width <= 0 || roi.height <= 0)
        return false;

    s.depthRoi = depthMm(roi);
    s.validRoi = validMask(roi);
    s.width    = roi.width;
    s.height   = roi.height;
    s.zMin     = p.depthZMin;
    s.edge     = p.edgeMargin;
    s.minArea  = std::max(kSeedMinPixels, p.minArea);

    double hi = std::numeric_limits<double>::lowest();
    std::vector<float> values;
    values.reserve(static_cast<size_t>(cv::countNonZero(s.validRoi)));
    for (int y = 0; y < s.height; ++y) {
        const uchar* validRow = s.validRoi.ptr<uchar>(y);
        const float* depthRow = s.depthRoi.ptr<float>(y);
        for (int x = 0; x < s.width; ++x) {
            if (!validRow[x])
                continue;
            values.push_back(depthRow[x]);
            hi = std::max(hi, static_cast<double>(depthRow[x]));
        }
    }
    if (values.empty())
        return false;

    // 起点 = `nearestPercentile` 分位（TR-149）：跳过零星噪声，避免迭代预算被耗在噪声带。
    // ⚠ 分位在这里**只定起点**，锚点仍由下面的「面积 + 边界 + 近端截断」过滤给出。
    const double pct = std::clamp(p.nearestPercentile,
                                  kNearestPercentileMin, kNearestPercentileMax);
    const size_t nth = static_cast<size_t>((values.size() - 1) * pct);
    std::nth_element(values.begin(), values.begin() + nth, values.end());
    s.lo = values[nth];
    s.hi = hi;
    return s.lo <= s.hi;
}

/// 取下一个锚点（近→远）。返回 false = 扫描结束。
bool NextAnchor(AnchorSweep& s, double& anchorMm)
{
    for (;;) {
        if (s.pendingPos < s.pending.size()) {
            anchorMm = s.pending[s.pendingPos++];
            return true;
        }
        if (s.finished)
            return false;

        s.pending.clear();
        s.pendingPos = 0;

        // 逐层抬高截断深度，直到本层出现「合格面片」为止
        bool produced = false;
        while (!produced) {
            if (s.it >= kAnchorMaxIters) {
                s.finished = true;
                return false;
            }
            // 分级步长（TR-149）：前 kAnchorFineIters 次 2 mm 精扫近处，之后 10 mm 粗扫。
            const int fine   = std::min(s.it, kAnchorFineIters);
            const int coarse = std::max(0, s.it - kAnchorFineIters);
            const double cut = s.lo + fine * kAnchorStepMm + coarse * kAnchorCoarseStepMm;
            ++s.it;
            if (cut > s.hi) {
                s.finished = true;
                return false;
            }

            cv::Mat mask(s.validRoi.size(), CV_8UC1);
            for (int y = 0; y < s.height; ++y) {
                const uchar* validRow = s.validRoi.ptr<uchar>(y);
                const float* depthRow = s.depthRoi.ptr<float>(y);
                uchar* maskRow = mask.ptr<uchar>(y);
                for (int x = 0; x < s.width; ++x)
                    maskRow[x] = (validRow[x] && depthRow[x] <= cut) ? 255 : 0;
            }

            cv::Mat labels, stats, centroids;
            const int labelCount = cv::connectedComponentsWithStats(
                mask, labels, stats, centroids, 8, CV_32S);
            if (labelCount <= 1)
                continue;

            for (int label = 1; label < labelCount; ++label) {
                if (stats.at<int>(label, cv::CC_STAT_AREA) < s.minArea)
                    continue;

                const int bx = stats.at<int>(label, cv::CC_STAT_LEFT);
                const int by = stats.at<int>(label, cv::CC_STAT_TOP);
                const int bw = stats.at<int>(label, cv::CC_STAT_WIDTH);
                const int bh = stats.at<int>(label, cv::CC_STAT_HEIGHT);
                // 贴 ROI 边界的面片被裁断，其最低点不可信（散热器 / 背景残片都在这一侧）。
                if (bx <= s.edge || by <= s.edge ||
                    bx + bw >= s.width - s.edge || by + bh >= s.height - s.edge)
                    continue;

                double minDepth = std::numeric_limits<double>::max();
                for (int y = by; y < by + bh; ++y) {
                    const int* labelRow = labels.ptr<int>(y);
                    const float* depthRow = s.depthRoi.ptr<float>(y);
                    for (int x = bx; x < bx + bw; ++x) {
                        if (labelRow[x] == label)
                            minDepth = std::min(minDepth, static_cast<double>(depthRow[x]));
                    }
                }
                if (minDepth == std::numeric_limits<double>::max())
                    continue;

                // TR-151 近端截断守卫：被近端量程切掉的面片，深度与面积都不可信。
                if (minDepth <= s.zMin + kAnchorNearClipTolMm)
                    continue;

                // 同一面片会在相邻层重复出现（最低深度不变）⇒ 按深度去重。
                bool dup = false;
                for (double v : s.yielded) {
                    if (std::abs(v - minDepth) <= kAnchorDedupMm) {
                        dup = true;
                        break;
                    }
                }
                if (dup)
                    continue;

                s.yielded.push_back(minDepth);
                s.pending.push_back(minDepth);
            }
            produced = !s.pending.empty();
        }

        std::sort(s.pending.begin(), s.pending.end());
    }
}

/// ② 种子提取（TR-144，2026-10-20）：给定**一个**锚点深度，取 `[anchor, anchor+band]`
/// 内的连通面片作为候选目标顶点。返回 false = 无合格种子。
///
/// **旧实现的问题**：顶层上限锚在**全 ROI 有效深度的分位**上（`nearestPercentile` 默认
/// 0.03）——目标只占画面一小块时，分位落在背景面上，顶层把背景一起圈进来。实测
/// `depth_20261020152105_0`：球顶 526 mm、背景面约 567 mm，3% 分位 = 561 mm ⇒ 上限
/// 581 mm 覆盖背景，球与背景连成 **218487 px** 的连通域，绿框 579×406（球本身只有
/// 91×102）。根因是基准随「目标占画面比例」漂移；去掉右侧深度传感器遮挡后背景有效
/// 像素变多，分位从球顶掉到背景面，问题才暴露。
///
/// **新做法**：锚点只用来定「**种子**」——该锚点附近的一小块面片（`anchor + kSeedBandMm`）。
/// 每个种子代表一个目标顶点；后续每个目标**各自**以 `种子最低深度 + heightBandMm` 定带，
/// 不再受其它目标 / 背景占画面比例的影响。
///
/// ⚠ 贴 ROI 边界的种子一律丢弃：被裁掉的目标顶点不可信，且实测会在 ROI 边界产生
///   细长条假种子（背景面被边界切断）。
///
/// **锚点来源（TR-145 + TR-151，2026-10-21）**：由 `AnchorSweep` 逐层扫描逐个产出，
/// 调用方对每个锚点各跑一遍「种子 → 生长 → 过滤」，取第一个出结果的。
bool BuildSeedsForAnchor(const cv::Mat& depthMm, const cv::Mat& validMask, const cv::Rect& roi,
                         const AlgorithmParams& p, double anchorMm, std::vector<Seed>& seeds)
{
    cv::Mat seedMask = cv::Mat::zeros(depthMm.size(), CV_8UC1);
    const double seedCutMm = anchorMm + kSeedBandMm;
    for (int y = roi.y; y < roi.y + roi.height; ++y) {
        const float* depthRow = depthMm.ptr<float>(y);
        const uchar* validRow = validMask.ptr<uchar>(y);
        uchar* seedRow = seedMask.ptr<uchar>(y);
        for (int x = roi.x; x < roi.x + roi.width; ++x) {
            if (validRow[x] && depthRow[x] <= seedCutMm)
                seedRow[x] = 255;
        }
    }

    if (kSeedMorphKernel > 1) {
        const cv::Mat kernel = cv::getStructuringElement(
            cv::MORPH_ELLIPSE, cv::Size(kSeedMorphKernel, kSeedMorphKernel));
        cv::morphologyEx(seedMask, seedMask, cv::MORPH_OPEN, kernel);
    }

    cv::Mat labels, stats, centroids;
    const int labelCount = cv::connectedComponentsWithStats(
        seedMask, labels, stats, centroids, 8, CV_32S);
    if (labelCount <= 1)
        return false;

    seeds.clear();
    for (int label = 1; label < labelCount; ++label) {
        if (stats.at<int>(label, cv::CC_STAT_AREA) < kSeedMinPixels)
            continue;

        const cv::Rect bbox(stats.at<int>(label, cv::CC_STAT_LEFT),
                            stats.at<int>(label, cv::CC_STAT_TOP),
                            stats.at<int>(label, cv::CC_STAT_WIDTH),
                            stats.at<int>(label, cv::CC_STAT_HEIGHT));
        // 贴 ROI 边界的种子一律丢弃：被裁掉的目标顶点不可信，且实测会在边界产生
        // 细长条假种子（背景面被边界切断后，其残片会落进种子带）。
        // ⚠ `bbox` 是**图像坐标**（种子掩码与图像同尺寸），边界须按 `roi` 判，不能按整图。
        if (bbox.x <= roi.x + p.edgeMargin || bbox.y <= roi.y + p.edgeMargin ||
            bbox.x + bbox.width >= roi.x + roi.width - p.edgeMargin ||
            bbox.y + bbox.height >= roi.y + roi.height - p.edgeMargin)
            continue;

        double minDepth = std::numeric_limits<double>::max();
        for (int y = bbox.y; y < bbox.y + bbox.height; ++y) {
            const float* depthRow = depthMm.ptr<float>(y);
            const int* labelRow = labels.ptr<int>(y);
            for (int x = bbox.x; x < bbox.x + bbox.width; ++x) {
                if (labelRow[x] == label)
                    minDepth = std::min(minDepth, static_cast<double>(depthRow[x]));
            }
        }
        if (minDepth == std::numeric_limits<double>::max())
            continue;

        Seed seed;
        seed.label = label;
        seed.bbox = bbox;
        seed.center = cv::Point2f(static_cast<float>(centroids.at<double>(label, 0)),
                                  static_cast<float>(centroids.at<double>(label, 1)));
        seed.minDepthMm = minDepth;
        seeds.push_back(seed);
    }
    if (seeds.empty())
        return false;

    std::sort(seeds.begin(), seeds.end(),
              [](const Seed& a, const Seed& b) { return a.minDepthMm < b.minDepthMm; });
    return true;
}

/// ②b 兜底锚点（TR-144 全局分位）：整帧连一个合格连通面片都扫不出来时才会走到这里
/// （防御性路径，正常场景由 `AnchorSweep` 覆盖）。
bool BuildSeedsFromPercentile(const cv::Mat& depthMm, const cv::Mat& validMask,
                              const cv::Rect& roi, const AlgorithmParams& p,
                              std::vector<Seed>& seeds, double& anchorMm)
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
    anchorMm = values[nth];

    return BuildSeedsForAnchor(depthMm, validMask, roi, p, anchorMm, seeds);
}

/// ③ 以种子为起点生长该目标的顶层区域（TR-144）。
///
/// 候选 = `valid && depth <= seed.minDepthMm + heightBandMm`，取**包含种子**的那个连通域。
/// 带厚锚定在目标自己的顶点上，背景面在带外 ⇒ 不会被并进来。
bool GrowSeedRegion(const cv::Mat& depthMm, const cv::Mat& validMask, const cv::Rect& roi,
                    const Seed& seed, const AlgorithmParams& p,
                    cv::Mat& regionMask, cv::Rect& bbox)
{
    const double cutoffMm = seed.minDepthMm + std::max(kMinHeightBandMm, p.heightBandMm);

    cv::Mat cand = cv::Mat::zeros(depthMm.size(), CV_8UC1);
    for (int y = roi.y; y < roi.y + roi.height; ++y) {
        const float* depthRow = depthMm.ptr<float>(y);
        const uchar* validRow = validMask.ptr<uchar>(y);
        uchar* candRow = cand.ptr<uchar>(y);
        for (int x = roi.x; x < roi.x + roi.width; ++x) {
            if (validRow[x] && depthRow[x] <= cutoffMm)
                candRow[x] = 255;
        }
    }

    const cv::Point seedPoint(static_cast<int>(std::lround(seed.center.x)),
                              static_cast<int>(std::lround(seed.center.y)));
    if (seedPoint.x < 0 || seedPoint.x >= cand.cols ||
        seedPoint.y < 0 || seedPoint.y >= cand.rows ||
        cand.at<uchar>(seedPoint) == 0)
        return false;

    // 填充值 128 与掩码的 0/255 区分开：128 = 种子所在连通域
    cv::Rect filledRect;
    cv::floodFill(cand, seedPoint, cv::Scalar(128), &filledRect, cv::Scalar(), cv::Scalar(), 8);
    regionMask = (cand == 128);

    if (p.morphKernelSize > 1) {
        int kernelSize = p.morphKernelSize;
        if (kernelSize % 2 == 0)
            ++kernelSize;   // ⚠ 偶数自动 +1（M6）——避免「填了 6 实际 7」的困惑
        const cv::Mat kernel = cv::getStructuringElement(
            cv::MORPH_ELLIPSE, cv::Size(kernelSize, kernelSize));
        // 只在区域包围框（外扩核半径）内做形态学：整图做会随种子数线性放大开销，
        // 空桌帧实测有 30 个种子。
        const cv::Rect imageRect(0, 0, depthMm.cols, depthMm.rows);
        const int pad = kernelSize / 2 + 1;
        const cv::Rect grow =
            cv::Rect(filledRect.x - pad, filledRect.y - pad,
                     filledRect.width + pad * 2, filledRect.height + pad * 2) & imageRect;
        if (grow.width > 0 && grow.height > 0) {
            cv::Mat sub;
            regionMask(grow).copyTo(sub);
            cv::morphologyEx(sub, sub, cv::MORPH_OPEN, kernel);
            cv::morphologyEx(sub, sub, cv::MORPH_CLOSE, kernel);
            regionMask.setTo(0);
            sub.copyTo(regionMask(grow));
        }
    }

    if (cv::countNonZero(regionMask) <= 0)
        return false;
    bbox = cv::boundingRect(regionMask);
    return true;
}

/// 组件内「距离变换峰值」检测（TR-135，2026-10-16）。
///
/// **动机**：三个纸团挤在一起时，`GrowSeedRegion` 末尾的 `MORPH_CLOSE` 会把它们**桥接**
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
/// 返回 `false` = 未过过滤（有效像素不足 / 质心落在边缘带 / TR-147 几何超限）；
/// 此时 `c` 内容未定义。
///
/// ⚠ `regionMask` 与图像**同尺寸**（255 = 属于该区域），只在 `bbox` 内遍历；
///   `bbox` 须为区域的**紧包围框**。
/// ⚠ `area` 直接取区域像素数：`regionMask ⊆ validMask`（掩码已保证深度有限且落在 `[zMin, zMax]`）
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

    // TR-147（2026-10-21）/ TR-148（配置化）：几何超限过滤。须在 `ComputeApex` 之后
    // （要用 `apexDepthMm`）。「整连通域」与「峰切分子区域」走同一条路径 ⇒ 巨域无论
    // 是否被切开都会被拒。⚠ `maxExtentProduct ≤ 0` = 禁用（与 `targetArea = 0` 同风格）。
    if (p.maxExtentProduct > 0.0 &&
        std::max(c.bbox.width, c.bbox.height) * c.apexDepthMm > p.maxExtentProduct)
        return false;

    const double confidence = EstimateConfidence(count, stdDev, center, depthMm.size(), p);
    const double nearScore  = 1.0 / (1.0 + std::max(0.0, mean - nearestDepthMm));
    const double areaScore  = CalculateAreaScore(count, p);
    c.score = confidence * kScoreConfWeight +
              nearScore * kScoreNearWeight +
              areaScore * kScoreAreaWeight;
    return true;
}

/// ④ 逐个种子生长目标区域，硬过滤后按**球顶深度升序**返回（`front()` = 最高）。
///
/// ⚠ 与老实现的差异：老实现只保留 `argmax` 一个（边扫边比），本仓 `Detect` 返回
/// `vector<PuffResult>`，故改为全量产出 + **显式排序**（契约 §11.3 约定 1）。
///
/// ⚠ TR-135（2026-10-16）：一个区域**不一定**只有一个目标——三球挤在一起时 `MORPH_CLOSE`
///   会把它们桥接成单域。故先做**区域内的峰切分**（`FindMarkers` + `VoronoiSplit`），切出
///   ≥ `kSplitMinMarkers` 个合格子目标时按子目标逐个产出，否则**回退**区域级单目标。
/// ⚠ TR-144（2026-10-20）：区域改为**按种子生长**（`GrowSeedRegion`），带厚锚定在目标
///   自己的顶点上；同一区域覆盖的其它种子一并消费（合并域交给上面的峰切分）。
std::vector<Candidate> SelectRegions(const cv::Mat& depthMm, const cv::Mat& validMask,
                                     const cv::Rect& roi, const std::vector<Seed>& seeds,
                                     double nearestDepthMm, const AlgorithmParams& p)
{
    std::vector<Candidate> out;

    const cv::Rect safeRect(
        p.edgeMargin,
        p.edgeMargin,
        std::max(0, depthMm.cols - p.edgeMargin * 2),
        std::max(0, depthMm.rows - p.edgeMargin * 2));

    std::vector<bool> consumed(seeds.size(), false);
    cv::Mat claimed = cv::Mat::zeros(depthMm.size(), CV_8UC1);
    for (size_t si = 0; si < seeds.size(); ++si) {
        if (consumed[si])
            continue;
        const Seed& seed = seeds[si];

        cv::Mat regionMask;
        cv::Rect bbox;
        if (!GrowSeedRegion(depthMm, validMask, roi, seed, p, regionMask, bbox))
            continue;

        // 本区域覆盖的其它种子一并消费：合并域由峰切分产出多个目标，避免重复计
        for (size_t sj = si + 1; sj < seeds.size(); ++sj) {
            if (consumed[sj])
                continue;
            const cv::Point c(static_cast<int>(std::lround(seeds[sj].center.x)),
                              static_cast<int>(std::lround(seeds[sj].center.y)));
            if (regionMask.at<uchar>(c) != 0)
                consumed[sj] = true;
        }

        const int area = cv::countNonZero(regionMask);
        if (area < p.minArea || area > p.maxArea)
            continue;

        // 与已接受区域重叠过半 ⇒ 同一片表面被多个种子重复生长（平背景上常见），只留一个。
        // 正常目标（分离或已由峰切分拆开）区域互不重叠，不会触发。
        if (cv::countNonZero(regionMask & claimed) * 2 > area)
            continue;
        cv::bitwise_or(claimed, regionMask, claimed);

        // ---- TR-135（2026-10-16）：区域内峰切分。三球挤在一起时 `MORPH_CLOSE` 把它们
        //      桥接成单域，区域级只会报 1 个目标 ⇒ 先试切分，成功则按子目标逐个产出。----
        const std::vector<cv::Point2f> markers = FindMarkers(regionMask, bbox);
        if (static_cast<int>(markers.size()) >= kSplitMinMarkers) {
            std::vector<cv::Mat>  subMasks;
            std::vector<cv::Rect> subBoxes;
            VoronoiSplit(regionMask, bbox, markers, subMasks, subBoxes);

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
                    sc.label = seed.label;
                    subs.push_back(sc);
                }
            }

            // 切出 ≥ `kSplitMinMarkers` 个**合格**子目标才算成功；否则**回退**区域级单目标
            // （⇒ 单球 / 切分失败时的行为与 TR-135 之前**逐位一致**）。
            if (static_cast<int>(subs.size()) >= kSplitMinMarkers) {
                SPDLOG_INFO("[DepthPuffAlgo] seed #{} area={} → {} sub-target(s) "
                            "(markers={}; sep={:.2f}×dmax peak={:.2f}×dmax)",
                            seed.label, area, subs.size(), markers.size(),
                            kSplitSepRatio, kSplitPeakRatio);
                out.insert(out.end(), subs.begin(), subs.end());
                continue;
            }
        }

        Candidate c;
        c.label = seed.label;
        if (MeasureRegion(depthMm, regionMask, bbox, safeRect, nearestDepthMm, p, c))
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
    p.maxExtentProduct    = cfg.getValue<double>(base + "maxExtentProduct",    p.maxExtentProduct);

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

    // ---- TR-151：逐个锚点尝试，取第一个能出「过阈值目标」的锚点 ----
    // 锚点 = 逐层扫描产出的合格连通面片最低深度（近→远）。某个锚点的候选**全被**
    // `confidenceThreshold` 滤掉时换下一个，而不是直接报「未检出」——单个近景干扰物
    // （机械臂末端、线缆、夹具）不再能否决整帧。
    AnchorSweep sweep;
    if (!InitAnchorSweep(depthMm, validMask, roi, params, sweep)) {
        lastError_ = "no seeds in ROI";
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

    std::vector<Seed> seeds;
    std::vector<Candidate> cands;
    double nearestDepthMm = 0.0;
    bool anchorFromComponent = true;
    bool triedPercentile = false;
    int attempt = 0;
    const double thr = params.confidenceThreshold;

    while (attempt < kAnchorMaxAttempts) {
        seeds.clear();

        if (NextAnchor(sweep, nearestDepthMm)) {
            ++attempt;
            anchorFromComponent = true;
            if (!BuildSeedsForAnchor(depthMm, validMask, roi, params, nearestDepthMm, seeds))
                continue;   // 该锚点长不出合格种子 → 换下一个锚点
        } else if (!triedPercentile) {
            // 扫描结束仍无任何合格面片 ⇒ 回退全局分位（TR-144 防御性路径），整帧只试一次。
            triedPercentile = true;
            anchorFromComponent = false;
            if (!BuildSeedsFromPercentile(depthMm, validMask, roi, params, seeds, nearestDepthMm)) {
                lastError_ = "no seeds in ROI";
                break;
            }
        } else {
            break;
        }

        cands = SelectRegions(depthMm, validMask, roi, seeds, nearestDepthMm, params);
        if (cands.empty()) {
            lastError_ = "no candidate passed filtering";
            continue;   // 该锚点无合格候选 → 换下一个锚点
        }

        results.clear();
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
            SPDLOG_INFO("[DepthPuffAlgo] attempt#{}/{} {} seed(s) → {} candidate(s); "
                        "best area={} meanDepth={:.1f}mm "
                        "topDepth={:.1f}mm apexDepth={:.1f}mm apex=({:.0f},{:.0f})[{}px] score={:.3f}; "
                        "nearestDepth={:.1f}mm anchor={} zRange=[{:.1f},{:.1f}]",
                        attempt, kAnchorMaxAttempts, seeds.size(), cands.size(),
                        best.area, best.meanDepthMm, best.minDepthMm,
                        best.apexDepthMm, best.apex.x, best.apex.y, best.apexCount, best.score,
                        nearestDepthMm, anchorFromComponent ? "component" : "percentile",
                        params.depthZMin, params.depthZMax);
        }

        // 置信度过滤：低于阈值**丢弃**（契约 §11.3 约定 3「过滤即丢弃」，TR-103 语义）。
        // ⚠ 两套算法的 `confidence` 语义不同（`SimAlgo` 是面积覆盖率、本算法是加权综合分）
        // ⇒ 同一个 `confidenceThreshold` 在两者上的松紧并不等价（移植方案 §七 M1）。
        if (thr > 0.0) {
            const auto before = results.size();
            results.erase(std::remove_if(results.begin(), results.end(),
                                         [thr](const PuffResult& r) { return r.confidence < thr; }),
                          results.end());
            if (results.size() != before) {
                SPDLOG_INFO("[DepthPuffAlgo] attempt#{}/{} anchor={:.1f}mm: confidenceThreshold={:.3f} "
                            "filtered {} of {} detection(s) → 换下一个锚点",
                            attempt, kAnchorMaxAttempts, nearestDepthMm, thr,
                            before - results.size(), before);
            }
        }

        if (!results.empty())
            break;
        lastError_ = "all candidates below confidenceThreshold";
    }

    if (results.empty() && attempt >= kAnchorMaxAttempts) {
        SPDLOG_WARN("[DepthPuffAlgo] 锚点尝试达上限 {} 仍未出目标 → 停止扫描"
                    "（检查 ROI 内近景干扰物，或收紧 `roi` / `depthZMin`）", kAnchorMaxAttempts);
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
        takeD("maxExtentProduct", fallback_.maxExtentProduct);   // TR-148：≤ 0 = 禁用

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
                    "morphKernelSize={} edgeMargin={} targetArea={} maxExtentProduct={:.0f} "
                    "roi=({},{},{},{})",
                    config_.size(), fallback_.nearestPercentile, fallback_.heightBandMm,
                    fallback_.apexPercentile, fallback_.minArea, fallback_.maxArea,
                    fallback_.morphKernelSize, fallback_.edgeMargin, fallback_.targetArea,
                    fallback_.maxExtentProduct,
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
