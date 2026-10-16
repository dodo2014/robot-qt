#include "SimAlgo.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "SimVision.h"
#include "ConfigManager.h"
#include "HALFactory.h"

REGISTER_PUFF_ALGORITHM("SimAlgo", SimAlgo)

std::string SimAlgo::Name() const
{
    return "SimAlgo";
}

AlgorithmParams SimAlgo::SnapshotParams() const
{
    // ⚠ 主线程专用：这里读 `ConfigManager`（`json` 读写非线程安全）。
    //
    // 覆盖语义（C4，2026-10-14）= **算法节点优先、通用域作默认**
    // （`algorithm_config_plan.md` §十 裁定 3）：先查 `vision.algorithms.SimAlgo.*`，
    // 缺则回落通用域。`SimAlgo` **没有私有键**，节点里通常什么都没有 ⇒ 行为与改造前一致。
    const auto& cfg = ConfigManager::instance();

    AlgorithmParams p;
    p.confidenceThreshold = cfg.getValue<double>("vision.algorithms.SimAlgo.confidenceThreshold",
                                                 EffectiveThreshold());
    p.depthZMin = cfg.getValue<double>("vision.algorithms.SimAlgo.depthZMin",
                                       cfg.getValue<double>("vision.depthZMin", 0.0));
    p.depthZMax = cfg.getValue<double>("vision.algorithms.SimAlgo.depthZMax",
                                       cfg.getValue<double>("vision.depthZMax", 500.0));
    return p;
}

std::vector<PuffResult> SimAlgo::Detect(const CameraFrame& frame, const AlgorithmParams& params)
{
    // 串行化（P1）：视觉页的检测线程与 `SequenceWorker` 的视觉识别动作可能同时在跑，
    // 二者共用一个算法实例；`lastError_` 是唯一被写入的成员，并发写 `std::string` 是 UB。
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<PuffResult> results;
    lastError_.clear();

    if (frame.colorData.empty() || frame.width <= 0 || frame.height <= 0) {
        lastError_ = "empty color frame";
        return results;
    }

    // P1：参数由调用方在**主线程**快照后传入——本函数不再读 `ConfigManager`，
    // 因此可以在工作线程安全调用（worker 线程绝不读 ConfigManager）。
    const double zMin = params.depthZMin;
    const double zMax = params.depthZMax;

    const int w = frame.width;
    const int h = frame.height;
    const double cx = w * 0.5;
    const double cy = h * 0.5;
    const double fx = 520.0;
    const double fy = 520.0;

    struct Acc {
        long long count = 0;
        double sumX = 0.0, sumY = 0.0;
        int minX = std::numeric_limits<int>::max();
        int minY = std::numeric_limits<int>::max();
        int maxX = std::numeric_limits<int>::min();
        int maxY = std::numeric_limits<int>::min();
    };

    Acc acc[SimVision::kTargetCount];
    int zFiltered = 0;   // 深度过滤计数（供与 confidenceThreshold 对称的汇总日志）

    const int stride = w * 3;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = frame.colorData.data() + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {
            int r = row[x * 3 + 0];
            int g = row[x * 3 + 1];
            int b = row[x * 3 + 2];
            for (int i = 0; i < SimVision::kTargetCount; ++i) {
                const auto& t = SimVision::kTargets[i];
                if (std::abs(r - t.r) <= 60 && std::abs(g - t.g) <= 60 && std::abs(b - t.b) <= 60) {
                    Acc& a = acc[i];
                    a.count++;
                    a.sumX += x;
                    a.sumY += y;
                    a.minX = std::min(a.minX, x);
                    a.minY = std::min(a.minY, y);
                    a.maxX = std::max(a.maxX, x);
                    a.maxY = std::max(a.maxY, y);
                }
            }
        }
    }

    for (int i = 0; i < SimVision::kTargetCount; ++i) {
        const auto& t  = SimVision::kTargets[i];
        const Acc&  a  = acc[i];
        const double fullArea = 3.14159265 * t.radius * t.radius;
        if (a.count < fullArea * 0.15)
            continue;

        PuffResult res;
        res.pixelU = static_cast<int>(std::lround(a.sumX / static_cast<double>(a.count)));
        res.pixelV = static_cast<int>(std::lround(a.sumY / static_cast<double>(a.count)));
        // 包围框原点（TR-132，2026-10-16）：契约要求「原点 + 尺寸」独立给出，画框才不依赖
        // 「以某点为中心」的假设。本算法的 `pixelU/pixelV` 是连通域质心，与框中心本就只差
        // 几 px；新字段填上后与 `DepthPuffAlgo`（球顶 ≠ 框中心）走同一条绘制路径。
        res.bboxX  = a.minX;
        res.bboxY  = a.minY;
        res.width  = a.maxX - a.minX + 1;
        res.height = a.maxY - a.minY + 1;

        double z = 0.0;
        // 深度取样（TR-116，2026-09-23）：`res.pixelU/pixelV` 是**彩色**坐标系，而
        // `depthData` 可能是**深度**坐标系——D2C 关闭时深度保持原生分辨率
        // （常用档 848×480 vs 彩色 1280×720）。改造前直接用彩色 `w`/`h` 索引深度数组
        // ⇒ 越界读（与 `FrameConverter::DepthToQImage` 同根因、同次崩溃）。
        const int dw = (frame.depthWidth  > 0) ? frame.depthWidth  : w;
        const int dh = (frame.depthHeight > 0) ? frame.depthHeight : h;
        const bool depthUsable = !frame.depthData.empty() &&
                                 frame.depthData.size() >=
                                     static_cast<size_t>(dw) * static_cast<size_t>(dh);
        if (depthUsable && res.pixelV >= 0 && res.pixelV < h &&
            res.pixelU >= 0 && res.pixelU < w) {
            // 彩色像素 → 深度像素：比例最近邻。D2C 开启时 dw==w && dh==h，退化为恒等映射；
            // 关闭时是**无外参标定下的近似**——两传感器视场不同，像素级对应关系并不严格
            // 成立，故 z 只作参考。首次遇到时打一条 warn 留痕，不静默。
            const int du = std::min(res.pixelU * dw / w, dw - 1);
            const int dv = std::min(res.pixelV * dh / h, dh - 1);
            if ((dw != w || dh != h) && !depthAlignWarned_) {
                depthAlignWarned_ = true;
                SPDLOG_WARN("[SimAlgo] 深度 {}x{} 与彩色 {}x{} 分辨率不同（D2C 关闭）→ "
                            "深度取样按比例近似，z 仅作参考（非像素级对应）", dw, dh, w, h);
            }
            float d = frame.depthData[static_cast<size_t>(dv) * dw + du];
            if (d > 0.0f && d >= zMin && d <= zMax) {
                z = d;
            } else if (d > 0.0f) {
                // 深度有效但落在 [zMin, zMax] 之外 → 该目标不在工作范围内，**丢弃**。
                // 语义与下方 confidenceThreshold 一致（都是「过滤掉」，不是「降级保留」）。
                // 修复前：z 保持 0 仍入表 → 结果表出现 x=y=z=0 的伪目标，且会被
                // SequenceWorker 识别分支的 results.front() 取走并喂给视觉引导（TR-103）。
                ++zFiltered;
                continue;
            }
            // d <= 0（深度缺失 / 无深度图）→ 保持 z = 0，仍报告 2D 位置（纯 2D 检测语义）
        }
        res.z   = z;
        res.x   = (res.pixelU - cx) / fx * z;
        res.y   = (res.pixelV - cy) / fy * z;
        res.yaw = 0.0;

        double coverage = a.count / fullArea;
        res.confidence = std::clamp(coverage * 1.08, 0.0, 1.0);

        results.push_back(res);
    }

    // 深度过滤汇总（TR-103）：与下方置信度过滤的日志对称，便于验收时确认生效。
    if (zFiltered > 0) {
        SPDLOG_INFO("[SimAlgo] depthZ[{:.1f},{:.1f}]: filtered {} of {} detection(s)",
                    zMin, zMax, zFiltered, SimVision::kTargetCount);
    }

    // 置信度阈值（C1）：低于阈值的结果**丢弃**。
    // 这才是「置信度阈值」应有的行为 —— 原实现只在视觉页绘制叠加框时用它
    // （`VisionTestPage.cpp` 的 `DrawOverlays`），算法侧完全不用，配置形同虚设。
    // 副作用是部分遮挡的目标不再被报出，属预期语义（该目标本就不该被抓取）。
    const double thr = params.confidenceThreshold;
    if (thr > 0.0) {
        const auto before = results.size();
        results.erase(std::remove_if(results.begin(), results.end(),
                                         [thr](const PuffResult& r) { return r.confidence < thr; }),
                      results.end());
        if (results.size() != before) {
            SPDLOG_INFO("[SimAlgo] confidenceThreshold={:.3f}: filtered {} of {} detection(s)",
                        thr, before - results.size(), before);
        }
    }

    return results;
}

PuffResult SimAlgo::LocateBest(const CameraFrame& frame, const AlgorithmParams& params)
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

bool SimAlgo::LoadConfig(const std::string& jsonConfig)
{
    config_ = jsonConfig;

    // 真正解析（C1 / C5）：原实现只把字符串存进 config_ 并打印字节数，**从不解析**，
    // 于是 `HardwareManager` 喂进来的 `confidenceThreshold` 是死数据 ——
    // 一条「看起来在工作、实际不工作」的通路。
    //
    // C5（2026-10-14）：调用方（C1）现在透传的是 `vision.algorithms.SimAlgo` **整棵子树**。
    // `SimAlgo` 没有私有键，子树里唯一有意义的是 `confidenceThreshold`（通用域键在算法
    // 节点内出现时**优先**，§十 裁定 3）；其余键一律忽略。
    // 解析出的值只作**兜底默认值**，运行时以 config.json 的实时值为准（见 EffectiveThreshold）。
    try {
        const auto j = nlohmann::json::parse(jsonConfig);
        if (j.is_object() && j.contains("confidenceThreshold") && j["confidenceThreshold"].is_number()) {
            confidenceThreshold_ = j["confidenceThreshold"].get<double>();
            SPDLOG_INFO("[SimAlgo] config loaded: confidenceThreshold={:.3f}", confidenceThreshold_);
            return true;
        }
        SPDLOG_INFO("[SimAlgo] config loaded but no usable confidenceThreshold ({} bytes)", config_.size());
        return true;
    } catch (const std::exception& e) {
        SPDLOG_WARN("[SimAlgo] LoadConfig parse error: {} (raw {} bytes)", e.what(), config_.size());
        return false;
    }
}

double SimAlgo::EffectiveThreshold() const
{
    // 实时读 config.json；键缺失（含 config 尚未加载）时 `getValue` 返回传入的默认值，
    // 即 LoadConfig 解析出的 confidenceThreshold_。
    return ConfigManager::instance().getValue<double>("vision.confidenceThreshold", confidenceThreshold_);
}

std::string SimAlgo::GetLastError() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}
