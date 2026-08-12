#include "SimAlgo.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <spdlog/spdlog.h>

#include "SimVision.h"
#include "ConfigManager.h"
#include "HALFactory.h"

REGISTER_PUFF_ALGORITHM("SimAlgo", SimAlgo)

std::string SimAlgo::Name() const
{
    return "SimAlgo";
}

std::vector<PuffResult> SimAlgo::Detect(const CameraFrame& frame)
{
    std::vector<PuffResult> results;
    lastError_.clear();

    if (frame.colorData.empty() || frame.width <= 0 || frame.height <= 0) {
        lastError_ = "empty color frame";
        return results;
    }

    auto& cfg = ConfigManager::instance();
    double zMin = cfg.getValue<double>("vision.depthZMin", 0.0);
    double zMax = cfg.getValue<double>("vision.depthZMax", 500.0);

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
        res.width  = a.maxX - a.minX + 1;
        res.height = a.maxY - a.minY + 1;

        double z = 0.0;
        if (!frame.depthData.empty() && res.pixelV >= 0 && res.pixelV < h &&
            res.pixelU >= 0 && res.pixelU < w) {
            float d = frame.depthData[static_cast<size_t>(res.pixelV) * w + res.pixelU];
            if (d > 0.0f && d >= zMin && d <= zMax)
                z = d;
        }
        res.z   = z;
        res.x   = (res.pixelU - cx) / fx * z;
        res.y   = (res.pixelV - cy) / fy * z;
        res.yaw = 0.0;

        double coverage = a.count / fullArea;
        res.confidence = std::clamp(coverage * 1.08, 0.0, 1.0);

        results.push_back(res);
    }

    return results;
}

PuffResult SimAlgo::LocateBest(const CameraFrame& frame)
{
    PuffResult best;
    double bestConf = 0.0;
    for (const auto& r : Detect(frame)) {
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
    SPDLOG_INFO("[SimAlgo] config loaded ({} bytes)", config_.size());
    return true;
}

std::string SimAlgo::GetLastError() const
{
    return lastError_;
}
