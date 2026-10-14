#include "DepthPuffAlgo.h"

#include <algorithm>
#include <cmath>
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
constexpr int    kMinValidPixelsFloor  = 10;      // 连通域最少有效像素下限
constexpr int    kMinValidPixelsDiv     = 4;      // …与 minArea 的除数（取 max）
constexpr double kScoreConfWeight      = 0.60;    // score = conf*0.60 + near*0.25 + area*0.15
constexpr double kScoreNearWeight      = 0.25;
constexpr double kScoreAreaWeight      = 0.15;
constexpr double kConfAreaWeight       = 0.45;    // confidence = area*0.45 + stability*0.35 + center*0.20
constexpr double kConfStabilityWeight  = 0.35;
constexpr double kConfCenterWeight     = 0.20;
constexpr int    kAreaScoreScale       = 8;       // 面积评分退化分支：area / (minArea * 8)

/// 单个候选连通域（**深度域**坐标）。
struct Candidate
{
    int         label = 0;
    cv::Rect    bbox;
    cv::Point2f center;
    int         area = 0;
    double      meanDepthMm = 0.0;   // 区域均值——仅入日志，供调参对照穹顶高度
    double      minDepthMm = 0.0;    // 区域最高点——契约 §11.3 的 z
    double      score = 0.0;         // 三权重综合分，排序依据
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

/// ③ 遍历**全部**连通域，硬过滤后按 `score` 降序返回。
///
/// ⚠ 与老实现的差异：老实现只保留 `argmax` 一个（边扫边比），本仓 `Detect` 返回
/// `vector<PuffResult>`，故改为全量产出 + **显式降序排序**（契约 §11.3 约定 1）。
std::vector<Candidate> SelectRegions(const cv::Mat& depthMm, const cv::Mat& topMask,
                                     double nearestDepthMm, const AlgorithmParams& p)
{
    cv::Mat labels, stats, centroids;
    const int componentCount = cv::connectedComponentsWithStats(
        topMask, labels, stats, centroids, 8, CV_32S);

    std::vector<Candidate> out;
    if (componentCount <= 1)
        return out;

    const cv::Rect imageRect(0, 0, depthMm.cols, depthMm.rows);
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

        double sum = 0.0;
        double sumSq = 0.0;
        double minDepth = std::numeric_limits<double>::max();
        int count = 0;

        const cv::Rect clipped = bbox & imageRect;
        for (int y = clipped.y; y < clipped.y + clipped.height; ++y) {
            const int* labelRow = labels.ptr<int>(y);
            const float* depthRow = depthMm.ptr<float>(y);
            for (int x = clipped.x; x < clipped.x + clipped.width; ++x) {
                if (labelRow[x] != label)
                    continue;

                const double value = depthRow[x];
                if (!std::isfinite(value) || value < p.depthZMin || value > p.depthZMax)
                    continue;

                sum += value;
                sumSq += value * value;
                minDepth = std::min(minDepth, value);
                ++count;
            }
        }

        const int minValidCount = std::max(kMinValidPixelsFloor, p.minArea / kMinValidPixelsDiv);
        if (count < minValidCount)
            continue;

        const double mean = sum / count;
        const double variance = std::max(0.0, sumSq / count - mean * mean);
        const double stdDev = std::sqrt(variance);

        Candidate c;
        c.label       = label;
        c.bbox        = bbox;
        c.center      = center;
        c.area        = area;
        c.meanDepthMm = mean;
        c.minDepthMm  = minDepth;

        const double confidence = EstimateConfidence(area, stdDev, center, depthMm.size(), p);
        const double nearScore  = 1.0 / (1.0 + std::max(0.0, mean - nearestDepthMm));
        const double areaScore  = CalculateAreaScore(area, p);
        c.score = confidence * kScoreConfWeight +
                  nearScore * kScoreNearWeight +
                  areaScore * kScoreAreaWeight;

        out.push_back(c);
    }

    // ⚠ 契约 §11.3 约定 1：`results.front()` = 最佳目标（`SequenceWorker` 直接取 `front()`）。
    // 老实现靠「边扫边比」天然得到最优；改成 vector 后**必须显式排序**，否则静默抓错目标。
    std::stable_sort(out.begin(), out.end(),
                     [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
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
        res.pixelU = static_cast<int>(std::lround(c.center.x * sx));
        res.pixelV = static_cast<int>(std::lround(c.center.y * sy));
        res.width  = static_cast<int>(std::lround(c.bbox.width  * sx));
        res.height = static_cast<int>(std::lround(c.bbox.height * sy));

        // 契约 §11.3：`z` = 区域内**最高点**（抓取应取最高点），**不是**区域均值。
        // 均值只留在日志里——「均值 vs 最高点」的差值能反映泡芙穹顶高度。
        res.z          = c.minDepthMm;
        res.minDepthMm = c.minDepthMm;

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
        SPDLOG_INFO("[DepthPuffAlgo] {} candidate(s); best area={} meanDepth={:.1f}mm "
                    "topDepth={:.1f}mm score={:.3f}; nearestDepth={:.1f}mm zRange=[{:.1f},{:.1f}]",
                    cands.size(), best.area, best.meanDepthMm, best.minDepthMm, best.score,
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
    PuffResult best;
    double bestConf = 0.0;
    for (const auto& r : Detect(frame, params)) {
        if (r.confidence > bestConf) {
            bestConf = r.confidence;
            best = r;
        }
    }
    return best;
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
                    "heightBandMm={:.1f} minArea={} maxArea={} morphKernelSize={} edgeMargin={} "
                    "targetArea={} roi=({},{},{},{})",
                    config_.size(), fallback_.nearestPercentile, fallback_.heightBandMm,
                    fallback_.minArea, fallback_.maxArea, fallback_.morphKernelSize,
                    fallback_.edgeMargin, fallback_.targetArea,
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
