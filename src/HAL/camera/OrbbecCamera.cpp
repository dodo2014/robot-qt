// ============================================================
// OrbbecCamera 实现。设计依据 doc/camera_integration_plan.md。
//
// ⚠ 三条硬约束（用户实测，勿违）：
//   ① 不注册 SDK 插拔回调（registerDeviceChangedCallback 会崩）→ 轮询探测；
//   ② 高分辨率必须**软件对齐**（ob::Align 逐帧 process）；不用 set_align_mode(SW_MODE)；
//   ③ 拔线瞬间直接取帧会崩 → 取帧前必须先探活。
//
// 本文件是本项目中唯一接触 libobsensor 的地方。
// ============================================================

#include "OrbbecCamera.h"

#include <libobsensor/ObSensor.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "HALFactory.h"

// 工厂注册：`simulation.cameraType = "OrbbecCamera"` 时由 HALFactory 创建。
// **USE_ORBBEC=OFF 时本文件不参与编译**，工厂里就没有这个类型名 ——
// 此时 config 若写 "OrbbecCamera" 会命中 HardwareManager 的
// 「Camera type '{}' not registered」告警（camera_ 为空，不崩）。
REGISTER_CAMERA("OrbbecCamera", OrbbecCamera)

namespace {

// ---- 分级恢复阈值（§4.2）----
constexpr int kFailL1 = 5;    // 连续取帧失败 → 丢积压帧 + 退避
constexpr int kFailL2 = 20;   // → pipeline 软重启
constexpr int kFailL3 = 40;   // → 全量重建 Context
constexpr auto kReconnectCooldown = std::chrono::seconds(3);   // 冷却：3 s 内不重复发起
constexpr auto kProbeInterval     = std::chrono::milliseconds(500);  // L0 探活降频

// 字符串 ↔ OBFormat。空串/未知 → OB_FORMAT_ANY（交给 profile 枚举放宽）
OBFormat formatFromName(const std::string& s)
{
    if (s == "MJPG" || s == "MJPEG") return OB_FORMAT_MJPG;
    if (s == "YUYV")                 return OB_FORMAT_YUYV;
    if (s == "RGB" || s == "RGB888") return OB_FORMAT_RGB;
    if (s == "BGR" || s == "BGR888") return OB_FORMAT_BGR;
    if (s == "Y16")                  return OB_FORMAT_Y16;
    return OB_FORMAT_ANY;
}

const char* formatName(int f)
{
    switch (static_cast<OBFormat>(f)) {
    case OB_FORMAT_MJPG: return "MJPG";
    case OB_FORMAT_YUYV: return "YUYV";
    case OB_FORMAT_RGB:  return "RGB";
    case OB_FORMAT_BGR:  return "BGR";
    case OB_FORMAT_Y16:  return "Y16";
    default:             return "?";
    }
}

// 从 pipeline 读出某个 sensor 的**全部**可用 profile（P4）。
//
// 只读查询：不做 `enableStream` / `start`，故对**已开流的 pipeline 无副作用**——
// 这是「已开流时直接问既有 pipeline_」这条路径成立的前提。
void collectStreamProfiles(const std::shared_ptr<ob::Pipeline>& pipe, OBSensorType sensor,
                           std::vector<CameraStreamProfile>& out)
{
    if (!pipe) return;

    std::shared_ptr<ob::StreamProfileList> list;
    try {
        list = pipe->getStreamProfileList(sensor);
    } catch (const std::exception& e) {
        SPDLOG_WARN("[OrbbecCamera] getStreamProfileList 失败: {}", e.what());
        return;
    }
    if (!list) return;

    const uint32_t n = list->getCount();
    for (uint32_t i = 0; i < n; ++i) {
        // 单个 profile 解析失败不影响其余（SDK 的 getProfile 在个别项上会抛）
        try {
            auto prof = list->getProfile(i);
            auto vsp  = prof ? prof->as<ob::VideoStreamProfile>() : nullptr;
            if (!vsp) continue;
            CameraStreamProfile sp;
            sp.width  = static_cast<int>(vsp->getWidth());
            sp.height = static_cast<int>(vsp->getHeight());
            sp.fps    = static_cast<double>(vsp->getFps());
            sp.format = formatName(static_cast<int>(vsp->getFormat()));
            out.push_back(sp);
        } catch (...) {
        }
    }
}

// 逐级放宽地选一个可用 profile：精确 → 放宽 fps → 放宽格式 → 放宽分辨率 → 任意。
// **必须这么做**：硬编码一个不被支持的组合会直接开流失败（E8）。
// getVideoStreamProfile 在无匹配时抛异常，故每个组合都要单独 try。
std::shared_ptr<ob::VideoStreamProfile> pickProfile(
    const std::shared_ptr<ob::Pipeline>& pipe, OBSensorType sensor,
    int w, int h, OBFormat fmt, int fps)
{
    struct Try { int w, h, fps; OBFormat fmt; };
    const Try tries[] = {
        {w,             h,             fps,         fmt},
        {w,             h,             OB_FPS_ANY,  fmt},
        {w,             h,             OB_FPS_ANY,  OB_FORMAT_ANY},
        {OB_WIDTH_ANY,  OB_HEIGHT_ANY, OB_FPS_ANY,  fmt},
        {OB_WIDTH_ANY,  OB_HEIGHT_ANY, OB_FPS_ANY,  OB_FORMAT_ANY},
    };

    std::shared_ptr<ob::StreamProfileList> list;
    try {
        list = pipe->getStreamProfileList(sensor);
    } catch (const std::exception& e) {
        SPDLOG_WARN("[OrbbecCamera] getStreamProfileList 失败: {}", e.what());
        return nullptr;
    }
    if (!list || list->getCount() == 0) return nullptr;

    for (const auto& t : tries) {
        try {
            auto p = list->getVideoStreamProfile(t.w, t.h, t.fmt,
                                                 static_cast<uint32_t>(t.fps < 0 ? 0 : t.fps));
            if (p) return p;
        } catch (...) {
            // 该组合不被支持 → 继续放宽
        }
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
OrbbecCamera::OrbbecCamera() = default;

OrbbecCamera::~OrbbecCamera()
{
    Close();
}

void OrbbecCamera::Configure(const CameraStreamConfig& config)
{
    cfg_ = config;
}

void OrbbecCamera::SetError(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(errMutex_);
    lastError_ = msg;
}

std::string OrbbecCamera::GetLastError() const
{
    std::lock_guard<std::mutex> lk(errMutex_);
    return lastError_;
}

// ---------------------------------------------------------------------------
// S2 设备枚举
// ---------------------------------------------------------------------------
// 查询某个流的可用 profile 列表（P4）——视觉页 6 个流参数下拉框的数据源。
//
// 两条路径，**都不干扰正在跑的流**：
//   ① 已开流（`pipeline_` 存在）→ 直接问它（`getStreamProfileList` 是只读查询）；
//   ② 未开流 → 临时建 Device+Pipeline 查完即销毁，与 `EnumerateDevices()` 同为只读探测。
// 查不到时返回空列表 → UI 回落为「只显示 config 现值」，**不阻断开流**。
std::vector<CameraStreamProfile> OrbbecCamera::GetStreamProfiles(bool isDepth)
{
    std::vector<CameraStreamProfile> out;
    const OBSensorType sensor = isDepth ? OB_SENSOR_DEPTH : OB_SENSOR_COLOR;

    if (pipeline_) {
        collectStreamProfiles(pipeline_, sensor, out);
        return out;
    }

    try {
        if (!context_ && !CreateContext()) return out;

        auto list = context_->queryDeviceList();
        if (!list || list->getCount() == 0) return out;

        std::shared_ptr<ob::Device> dev;
        if (!currentSn_.empty()) {
            try {
                dev = list->getDeviceBySN(currentSn_.c_str());
            } catch (...) {
                dev.reset();
            }
        }
        if (!dev) dev = list->getDevice(0);
        if (!dev) return out;

        auto tmpPipe = std::make_shared<ob::Pipeline>(dev);
        collectStreamProfiles(tmpPipe, sensor, out);
        // 没 start 过，stop() 幂等；仍 try 保护，避免 SDK 内部留下半开状态
        try { tmpPipe->stop(); } catch (...) {
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("[OrbbecCamera] 查询 profile 列表失败: {}", e.what());
    }
    return out;
}

std::vector<CameraDeviceInfo> OrbbecCamera::EnumerateDevices()
{
    std::vector<CameraDeviceInfo> out;
    try {
        if (!context_ && !CreateContext()) return out;

        auto list = context_->queryDeviceList();
        if (!list) return out;

        const uint32_t n = list->getCount();
        for (uint32_t i = 0; i < n; ++i) {
            std::shared_ptr<ob::Device> dev;
            try {
                dev = list->getDevice(i);
            } catch (...) {
                continue;
            }
            if (!dev) continue;

            // 僵尸设备探测：主动 getDeviceInfo() 触发一次真实 USB 通讯，抛异常即判假死
            // （参考实现 :243-256）。枚举是只读操作，只跳过该设备，不销毁 Context。
            try {
                auto info = dev->getDeviceInfo();
                if (!info) continue;
                CameraDeviceInfo d;
                if (info->getSerialNumber()) d.id = info->getSerialNumber();
                if (info->getName())         d.name = info->getName();
                if (info->getConnectionType()) d.connection = info->getConnectionType();
                out.push_back(d);
            } catch (const std::exception& e) {
                SPDLOG_WARN("[OrbbecCamera] 设备 #{} 探活失败（判为僵尸设备，已跳过）: {}", i, e.what());
            }
        }
    } catch (const std::exception& e) {
        SetError(std::string("设备枚举失败: ") + e.what());
        SPDLOG_WARN("[OrbbecCamera] 设备枚举失败: {}", e.what());
    }
    return out;
}

bool OrbbecCamera::probeDevices()
{
    try {
        if (!context_) return false;
        auto list = context_->queryDeviceList();
        if (!list || list->getCount() == 0) return false;

        // 僵尸设备探测：枚举到不等于可用，必须主动触发一次真实通讯
        const uint32_t n = list->getCount();
        for (uint32_t i = 0; i < n; ++i) {
            try {
                auto dev = list->getDevice(i);
                if (dev && dev->getDeviceInfo()) return true;
            } catch (...) {
                // 该设备假死 → 试下一个
            }
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool OrbbecCamera::IsDevicePresent()
{
    // §4.2 L0：探活降频到约 500 ms 一次，避免每帧都做一次 USB 枚举
    const auto now = std::chrono::steady_clock::now();
    if (now - lastProbeAt_ < kProbeInterval) return lastProbeResult_;
    lastProbeAt_ = now;
    lastProbeResult_ = probeDevices();
    return lastProbeResult_;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
bool OrbbecCamera::CreateContext()
{
    try {
        // ⚠ SDK 默认把 **INFO 级**日志写到进程 CWD 的 `OrbbecSDK.log.txt`（实测 60 s 取流即
        // 37 KB，且无大小上限/轮转）。提到 WARN 后 INFO 刷屏消失，而 WARN/ERROR 仍保留——
        // 拔插线排障正需要后者。该接口是**全局静态**的，重复调用幂等，故放在每次建 Context 前。
        // 刻意**不**做「把 SDK 日志并进工程 log/ 目录」：路径解析是 `main.cpp::ResolveLogDir()`
        // 的职责（TR-096），HAL 设备实现不该自己解析路径。
        ob::Context::setLoggerSeverity(OB_LOG_SEVERITY_WARN);

        context_ = std::make_shared<ob::Context>();
        // 网口相机也纳入枚举（参考实现 :226）
        context_->enableNetDeviceEnumeration(true);
        return true;
    } catch (const std::exception& e) {
        SetError(std::string("Context 创建失败: ") + e.what());
        SPDLOG_WARN("[OrbbecCamera] Context 创建失败: {}", e.what());
        context_.reset();
        return false;
    }
}

void OrbbecCamera::DestroyContext()
{
    // 无设备时强行销毁 Context，逼 SDK 重新扫描 USB 枚举——解决
    // 「插上了但软件认不出」（参考实现 :232-235）
    context_.reset();
}

void OrbbecCamera::DestroyPipeline()
{
    // 拔线后 stop() 会抛 Bad Magic 甚至段错误 → 必须吞异常 + 强制置空
    // （参考实现 :294-333）
    if (pipeline_) {
        try {
            pipeline_->stop();
        } catch (...) {
            SPDLOG_WARN("[OrbbecCamera] pipeline->stop() 抛异常（已吞；设备可能已拔出）");
        }
        pipeline_.reset();
    }
    align_.reset();
    colorConv_.reset();
    convSrcFormat_ = -1;
    device_.reset();
    opened_ = false;
    streaming_ = false;
}

bool OrbbecCamera::ConfigureStreams()
{
    // ---- 彩色：按「首选格式 + 尝试队列」动态枚举（E7 / E8）----
    std::vector<OBFormat> colorFormats;
    if (!cfg_.colorFormat.empty()) colorFormats.push_back(formatFromName(cfg_.colorFormat));
    // 参考实现 :74-94 的尝试队列；RGB 优先（免解码），MJPG 兜底（高分辨率常见）
    for (OBFormat f : {OB_FORMAT_RGB, OB_FORMAT_YUYV, OB_FORMAT_MJPG}) {
        if (std::find(colorFormats.begin(), colorFormats.end(), f) == colorFormats.end())
            colorFormats.push_back(f);
    }

    std::shared_ptr<ob::VideoStreamProfile> colorProfile;
    for (OBFormat f : colorFormats) {
        colorProfile = pickProfile(pipeline_, OB_SENSOR_COLOR,
                                   cfg_.colorWidth, cfg_.colorHeight, f,
                                   static_cast<int>(cfg_.colorFps));
        if (colorProfile) { colorFormat_ = static_cast<int>(f); break; }
    }
    if (!colorProfile) {
        SetError("彩色流无可用 profile（已按 精确→放宽 逐级尝试）");
        SPDLOG_WARN("[OrbbecCamera] 彩色流无可用 profile");
        return false;
    }

    // ---- 深度 ----
    const OBFormat depthFmt = formatFromName(cfg_.depthFormat.empty() ? "Y16" : cfg_.depthFormat);
    auto depthProfile = pickProfile(pipeline_, OB_SENSOR_DEPTH,
                                    cfg_.depthWidth, cfg_.depthHeight, depthFmt,
                                    static_cast<int>(cfg_.depthFps));
    if (!depthProfile) {
        SetError("深度流无可用 profile");
        SPDLOG_WARN("[OrbbecCamera] 深度流无可用 profile");
        return false;
    }

    auto cfg = std::make_shared<ob::Config>();
    cfg->enableStream(colorProfile);
    cfg->enableStream(depthProfile);
    // 保证 frameset 里彩色与深度同时存在，否则 getDepthFrame() 可能为 null
    cfg->setFrameAggregateOutputMode(OB_FRAME_AGGREGATE_OUTPUT_ALL_TYPE_FRAME_REQUIRE);

    pipeline_->start(cfg);
    pipeline_->enableFrameSync();   // 按时间戳做帧同步（参考实现 / sync_align 示例）

    colorW_ = static_cast<int>(colorProfile->getWidth());
    colorH_ = static_cast<int>(colorProfile->getHeight());
    depthW_ = static_cast<int>(depthProfile->getWidth());
    depthH_ = static_cast<int>(depthProfile->getHeight());

    // 内参（供手眼换算链路使用；本项目当前的手眼矩阵为占位值，标定待实现）
    try {
        const auto in = colorProfile->getIntrinsic();   // OBCameraIntrinsic: fx/fy/cx/cy/width/height
        intrinsics_.fx = in.fx;  intrinsics_.fy = in.fy;
        intrinsics_.cx = in.cx;  intrinsics_.cy = in.cy;
        // ⚠ 畸变系数**不在** `OBCameraIntrinsic` 里（SDK 2.9.3 实测：该结构只有
        // fx/fy/cx/cy/width/height，`ObTypes.h:466-473`），必须单独取
        // `OBCameraDistortion`（`ObTypes.h:504-513`，含 k1..k6/p1/p2/model）
        const auto dist = colorProfile->getDistortion();
        intrinsics_.k1 = dist.k1;  intrinsics_.k2 = dist.k2;
        intrinsics_.p1 = dist.p1;  intrinsics_.p2 = dist.p2;
        intrinsics_.width  = colorW_;
        intrinsics_.height = colorH_;
    } catch (const std::exception& e) {
        SPDLOG_WARN("[OrbbecCamera] 读取彩色内参失败（不影响取流）: {}", e.what());
    }

    SPDLOG_INFO("[OrbbecCamera] profiles: color {}x{} fmt={} / depth {}x{} fmt={}",
                colorW_, colorH_, formatName(colorFormat_),
                depthW_, depthH_, formatName(static_cast<int>(depthFmt)));
    return true;
}

void OrbbecCamera::ConfigureAutoExposure()
{
    if (!device_) return;
    // 决策 4：本版固定自动曝光（曝光控件已移除）。失败不致命——只是沿用设备当前设置
    try {
        device_->setBoolProperty(OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, cfg_.autoExposure);
    } catch (const std::exception& e) {
        SPDLOG_WARN("[OrbbecCamera] 设置自动曝光失败（沿用设备当前值）: {}", e.what());
    }
}

void OrbbecCamera::WarmUp()
{
    // 连接成功后等光学传感器预热，否则立刻取图会「假掉帧」（参考实现 :145）
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    // 排空积压帧，给自动曝光留时间（参考实现 flush_frames）
    for (int i = 0; i < 5; ++i) {
        try {
            if (!pipeline_->waitForFrameset(200)) break;
        } catch (...) {
            break;
        }
    }
}

bool OrbbecCamera::BuildAndStartPipeline(const std::string& deviceId)
{
    try {
        auto list = context_->queryDeviceList();
        if (!list || list->getCount() == 0) {
            SetError("未枚举到任何设备");
            return false;
        }

        if (!deviceId.empty()) {
            try {
                device_ = list->getDeviceBySN(deviceId.c_str());
            } catch (...) {
                device_.reset();
            }
            if (!device_) {
                // 配的 SN 与实物不符时**回落到首个设备**而不是直接失败：
                // 单机单相机场景下这比「配错就完全不可用」更有用，但必须显式告警
                SPDLOG_WARN("[OrbbecCamera] 未找到 SN='{}' 的设备，回落到首个设备", deviceId);
            }
        }
        if (!device_) device_ = list->getDevice(0);
        if (!device_) {
            SetError("无可用设备");
            return false;
        }

        {
            auto info = device_->getDeviceInfo();
            currentSn_ = (info && info->getSerialNumber()) ? info->getSerialNumber() : deviceId;
        }

        pipeline_ = std::make_shared<ob::Pipeline>(device_);
        if (!ConfigureStreams()) return false;
        ConfigureAutoExposure();

        // 决策 2：高分辨率必须软件对齐（HW 对齐仅 640×480 彩色 + 848×480 深度档可用）。
        // 不用 config.set_align_mode(SW_MODE)——参考实现明确记录该路径走不通。
        if (cfg_.d2cAlign == "SW") {
            align_ = std::make_shared<ob::Align>(OB_STREAM_COLOR);
        } else {
            align_.reset();
        }

        WarmUp();
        return true;
    } catch (const std::exception& e) {
        SetError(std::string("开流失败: ") + e.what());
        SPDLOG_WARN("[OrbbecCamera] 开流失败: {}", e.what());
        return false;
    }
}

bool OrbbecCamera::Open(const std::string& deviceId, int width, int height, double fps)
{
    Close();   // 幂等：先清干净（重复 Open 不泄漏）

    // 显式传入的流参数优先于 Configure() 下发的（CameraManager 两者都会给）
    if (width  > 0) cfg_.colorWidth  = width;
    if (height > 0) cfg_.colorHeight = height;
    if (fps    > 0) cfg_.colorFps    = fps;

    linkState_ = CameraLinkState::Connecting;

    if (!CreateContext()) {
        ++openFailures_;
        linkState_ = CameraLinkState::Disconnected;
        return false;
    }
    if (!BuildAndStartPipeline(deviceId)) {
        ++openFailures_;
        DestroyPipeline();
        linkState_ = CameraLinkState::Disconnected;
        return false;
    }

    opened_ = true;
    openFailures_ = 0;
    ResetFailureCounters();
    linkState_ = CameraLinkState::Streaming;
    SPDLOG_INFO("[OrbbecCamera] Opened SN='{}' color={}x{}@{} fmt={} depth={}x{} align={}",
                currentSn_, colorW_, colorH_, static_cast<int>(cfg_.colorFps),
                formatName(colorFormat_), depthW_, depthH_,
                cfg_.d2cAlign.empty() ? "OFF" : cfg_.d2cAlign);
    return true;
}

void OrbbecCamera::Close()
{
    wantStreaming_ = false;   // 先清意图位，再拆管线（TR-111；顺序不能反）
    DestroyPipeline();
    DestroyContext();
    linkState_ = CameraLinkState::Disconnected;
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        lastGoodFrame_ = CameraFrame{};
    }
}

bool OrbbecCamera::IsOpened() const { return opened_.load(); }
bool OrbbecCamera::IsStreaming() const { return streaming_.load(); }

bool OrbbecCamera::StartStream()
{
    if (!opened_ || !pipeline_) {
        SetError("StartStream：设备未打开");
        return false;
    }
    streaming_ = true;
    wantStreaming_ = true;   // 意图位：此后 `CaptureFrame()` 拿不到管线会主动重连（TR-111）
    linkState_ = CameraLinkState::Streaming;
    // 重新开始采集前排空积压帧，避免首帧是几秒前的陈旧画面
    for (int i = 0; i < 3; ++i) {
        try {
            if (!pipeline_->waitForFrameset(100)) break;
        } catch (...) {
            break;
        }
    }
    return true;
}

void OrbbecCamera::StopStream()
{
    // 只停「取帧」，**不拆 pipeline**——这样重新开始采集是瞬时的。
    // 真正释放设备在 Close()。
    streaming_ = false;
    // ⚠ 意图位必须一起清（TR-111）：否则 `CaptureFrame()` 会把「用户主动停流」误判成
    // 「恢复失败」，每 15 fps 触发一次重连。
    wantStreaming_ = false;
}

// ---------------------------------------------------------------------------
// 取帧
// ---------------------------------------------------------------------------
bool OrbbecCamera::NormalizeColor(const std::shared_ptr<ob::Frame>& src, CameraFrame& out)
{
    try {
        const OBFormat fmt = src->getFormat();
        std::shared_ptr<ob::Frame> rgb = src;

        if (fmt != OB_FORMAT_RGB) {
            // FrameConverter::ColorToQImage 只认 RGB888（FrameConverter.cpp:14-18）——
            // 所有非 RGB 输入必须在相机实现内归一化，否则 UI 花屏。
            // 参考实现漏了 YUYV 分支（:479-494 注释），这里补上。
            if (!colorConv_ || convSrcFormat_ != static_cast<int>(fmt)) {
                colorConv_ = std::make_shared<ob::FormatConvertFilter>();
                colorConv_->setFormatConvertType(fmt == OB_FORMAT_YUYV ? FORMAT_YUYV_TO_RGB
                                                                      : FORMAT_MJPG_TO_RGB);
                convSrcFormat_ = static_cast<int>(fmt);
            }
            rgb = colorConv_->process(src);
            if (!rgb) return false;
        }

        // ⚠ `ob::Frame` **没有** getWidth/getHeight（SDK 2.9.3 实测：只有
        // `ob::VideoFrame` 有，`Frame.hpp:524/537`）→ 先 as<VideoFrame>() 再取尺寸
        auto vf = rgb->as<ob::VideoFrame>();
        if (!vf) return false;
        out.width  = static_cast<int>(vf->getWidth());
        out.height = static_cast<int>(vf->getHeight());
        const uint32_t sz = rgb->getDataSize();
        if (sz == 0) return false;
        out.colorData.assign(rgb->getData(), rgb->getData() + sz);
        out.timestamp = static_cast<int64_t>(rgb->getTimeStampUs() / 1000);
        return true;
    } catch (const std::exception& e) {
        SetError(std::string("彩色归一化失败: ") + e.what());
        return false;
    }
}

void OrbbecCamera::StoreLastGood(const CameraFrame& f)
{
    std::lock_guard<std::mutex> lk(frameMutex_);
    lastGoodFrame_ = f;
    lastGoodAt_ = std::chrono::steady_clock::now();
}

CameraFrame OrbbecCamera::TakeLastGood()
{
    std::lock_guard<std::mutex> lk(frameMutex_);
    // 只回放 1 s 内的上一帧：更旧的画面没有意义，且会掩盖掉线
    if (!lastGoodFrame_.colorData.empty() &&
        std::chrono::steady_clock::now() - lastGoodAt_ < std::chrono::seconds(1)) {
        return lastGoodFrame_;
    }
    return CameraFrame{};
}

CameraFrame OrbbecCamera::CaptureFrame()
{
    CameraFrame out;

    // 主动停流（`StopStream()` / `Close()`）→ 什么都不做，也**不触发重连**。
    if (!wantStreaming_) return out;

    // ⚠ 期望在流、但当前拿不到管线 —— 这里必须进恢复状态机（TR-111，2026-09-23）。
    // 改造前是 `if (!opened_ || !streaming_ || !pipeline_) return out;`，而
    // `HandleGrabFailure()` 在**本守卫之下**：L2/L3 一旦失败，其失败分支只写
    // `linkState_ = Disconnected`、**不复位 `streaming_`**（`DestroyPipeline()` 已把它清零），
    // 于是此后每一次 `CaptureFrame()` 都在这里早退 → `HandleGrabFailure()` 再也进不来 →
    // 无重试、无状态边沿、无日志，**永久死锁**。
    // 用例 11.28 实测：11:11:43.194 L3 重建失败（`state:1`）之后日志彻底停止，与此吻合。
    // 恢复路径本身在 `HandleGrabFailure()` 内部按 3 s 冷却节流，不会形成重连风暴。
    if (!opened_ || !streaming_ || !pipeline_) {
        HandleGrabFailure();
        return TakeLastGood();
    }

    // ③ 取帧前先探活——拔线瞬间直接取帧会导致底层 waitForFrameset 崩溃
    // （参考实现 :127-128）。这是本实现里最不能省的一步。
    if (!IsDevicePresent()) {
        linkState_ = CameraLinkState::Disconnected;
        SetError("设备不在位（已拔出或掉线）");
        HandleGrabFailure();
        return TakeLastGood();
    }

    std::shared_ptr<ob::FrameSet> fs;
    try {
        fs = pipeline_->waitForFrameset(1000);
    } catch (const std::exception& e) {
        SetError(std::string("waitForFrameset 失败: ") + e.what());
        fs = nullptr;
    }
    if (!fs) {
        HandleGrabFailure();
        return TakeLastGood();
    }

    // 软对齐：逐帧 process()。**失败只跳过该帧，绝不判为断线**——
    // 光线突变时偶发失败是已知现象（参考实现 :524-536）
    if (align_) {
        try {
            auto aligned = align_->process(fs);
            if (aligned) {
                auto afs = aligned->as<ob::FrameSet>();
                if (afs) fs = afs;
            }
        } catch (const std::exception& e) {
            if (!alignWarned_) {
                SPDLOG_WARN("[OrbbecCamera] 软对齐失败（只跳帧，不判断线）: {}", e.what());
                alignWarned_ = true;
            }
        }
    }

    bool gotColor = false;
    try {
        auto color = fs->getColorFrame();
        if (color) gotColor = NormalizeColor(color, out);
    } catch (const std::exception& e) {
        SetError(std::string("取彩色帧失败: ") + e.what());
    }

    try {
        auto depth = fs->getDepthFrame();
        if (depth) {
            const uint32_t w = depth->getWidth();
            const uint32_t h = depth->getHeight();
            const float scale = depth->getValueScale();   // 原始值 × scale = mm
            const uint8_t* raw = depth->getData();
            const uint32_t n = w * h;
            if (raw && n > 0 && depth->getDataSize() >= n * sizeof(uint16_t)) {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(raw);
                out.depthData.resize(n);
                for (uint32_t i = 0; i < n; ++i)
                    out.depthData[i] = static_cast<float>(src[i]) * scale;
                if (out.width == 0) { out.width = static_cast<int>(w); out.height = static_cast<int>(h); }
            }
        }
    } catch (const std::exception& e) {
        SetError(std::string("取深度帧失败: ") + e.what());
    }

    if (!gotColor) {
        HandleGrabFailure();
        return TakeLastGood();
    }

    ResetFailureCounters();
    linkState_ = CameraLinkState::Streaming;
    StoreLastGood(out);
    return out;
}

// ---------------------------------------------------------------------------
// 分级恢复（§4.2）
// ---------------------------------------------------------------------------
void OrbbecCamera::ResetFailureCounters()
{
    consecutiveFailures_ = 0;
    lastAttemptLevel_ = 0;   // 恢复成功 / 重新开流后，档位重新从 L1 起（TR-111）
}

// 可中断睡眠（TR-111）：`wantStreaming_` 变假（用户停流 / 关设备）时立即返回 false。
// 必要性：本函数只在采集线程调用，而 L3 要睡 6 s；若不可中断，用户在重连过程中停流会让
// `CameraManager` 析构里的 `cameraThread_->wait(2000)` 超时。
bool OrbbecCamera::InterruptibleSleep(std::chrono::milliseconds total)
{
    const auto deadline = std::chrono::steady_clock::now() + total;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!wantStreaming_) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

void OrbbecCamera::HandleGrabFailure()
{
    ++consecutiveFailures_;

    const auto now = std::chrono::steady_clock::now();
    // 冷却：3 s 内不重复发起重连动作，防重连风暴（参考实现 :31-32）
    if (now - lastReconnectAt_ < kReconnectCooldown) return;

    // ⚠ **先定档位、再逐级升**（TR-111，2026-09-23）。改造前冷却闸门在档位判定**之前** `return`，
    // 而计数在冷却期内**继续累加**：L1（第 5 次）记上 `lastReconnectAt_` 之后，3 s 冷却一过
    // 计数已越过 40 → 直接命中 L3，**L2「pipeline 软重启」永不执行**。
    // 用例 11.28 实测：11:11:35 的日志是「连续 42 次取帧失败 → L3」，全程没有 L2 字样。
    // 现按「每个冷却窗口最多升一档」推进：L1 → L2 → L3；失败后停在当前档，下个窗口继续。
    int level = 0;
    if (consecutiveFailures_ >= kFailL3)      level = 3;
    else if (consecutiveFailures_ >= kFailL2) level = 2;
    else if (consecutiveFailures_ >= kFailL1) level = 1;
    if (level == 0) return;                          // 还没到 L1，仅累计
    if (level > lastAttemptLevel_ + 1) level = lastAttemptLevel_ + 1;
    lastAttemptLevel_ = level;

    if (level == 3) {
        SPDLOG_WARN("[OrbbecCamera] 连续 {} 次取帧失败 → L3 全量重建 Context", consecutiveFailures_);
        lastReconnectAt_ = now;
        DestroyPipeline();
        DestroyContext();
        if (!InterruptibleSleep(std::chrono::seconds(6))) {
            linkState_ = CameraLinkState::Disconnected;   // 用户在重连中停流 → 放弃本次
            return;
        }
        linkState_ = CameraLinkState::Connecting;
        if (CreateContext() && BuildAndStartPipeline(currentSn_)) {
            opened_ = true;
            consecutiveFailures_ = 2;   // 计数退回低档，避免立即再次升级
            // ⚠ 必须重新置 `streaming_`（2026-09-23 修复，TR-110）。`DestroyPipeline()` 已把它
            // 清零（`:333`），而 `CaptureFrame()` 开头是
            // `if (!opened_ || !streaming_ || !pipeline_) return out;`（`:621`）——原实现只写
            // `linkState_ = Streaming`，于是恢复「成功」却一帧都取不到：画面冻结，且再也走不到
            // 探活与 `HandleGrabFailure()` → 后续拔插**无任何日志、无任何状态变化**。
            // 用例 11.28 实测复现：首次 L3 恢复后画面定格、第三次拔插零日志。
            StartStream();   // 置 streaming_ + linkState_=Streaming + 排空 3 帧积压
            SPDLOG_INFO("[OrbbecCamera] L3 恢复成功");
        } else {
            // ⚠ 失败必须留痕（TR-111）：改造前这里只有一行 `linkState_ = Disconnected`，
            // 全程静默 —— 用户只能看到 PollCamera 广播的状态码，无法判断卡在哪一步。
            SPDLOG_WARN("[OrbbecCamera] L3 恢复失败（CreateContext / BuildAndStartPipeline 之一失败）"
                        " → Disconnected，下个冷却窗口重试");
            linkState_ = CameraLinkState::Disconnected;
        }
        return;
    }

    if (level == 2) {
        SPDLOG_WARN("[OrbbecCamera] 连续 {} 次取帧失败 → L2 pipeline 软重启", consecutiveFailures_);
        lastReconnectAt_ = now;
        DestroyPipeline();
        if (!InterruptibleSleep(std::chrono::seconds(2))) {
            linkState_ = CameraLinkState::Disconnected;   // 用户在重连中停流 → 放弃本次
            return;
        }
        linkState_ = CameraLinkState::Connecting;
        if (BuildAndStartPipeline(currentSn_)) {
            opened_ = true;
            ResetFailureCounters();
            // ⚠ 同 L3：必须重新置 `streaming_`，否则恢复后 `CaptureFrame()` 在 `:621` 早退
            // （`DestroyPipeline()` 已把 streaming_ 清零）→ 画面冻结 + 后续拔插零日志（TR-110）。
            StartStream();   // 置 streaming_ + linkState_=Streaming + 排空 3 帧积压
            SPDLOG_INFO("[OrbbecCamera] L2 恢复成功");
        } else {
            SPDLOG_WARN("[OrbbecCamera] L2 恢复失败（pipeline 软重启未成功）"
                        " → Disconnected，下个冷却窗口升到 L3");
            linkState_ = CameraLinkState::Disconnected;
        }
        return;
    }

    // L1 轻度：丢积压帧 + 短暂退避，不重建。
    // ⚠ 补日志（TR-111）：改造前 L1 分支完全静默，分级过程对用户不可见。
    SPDLOG_WARN("[OrbbecCamera] 连续 {} 次取帧失败 → L1 退避（不重建）", consecutiveFailures_);
    linkState_ = CameraLinkState::Degraded;
    lastReconnectAt_ = now;
    InterruptibleSleep(std::chrono::milliseconds(200));
}

// ---------------------------------------------------------------------------
CameraIntrinsics OrbbecCamera::GetIntrinsics() const { return intrinsics_; }

bool OrbbecCamera::SetIntrinsics(const CameraIntrinsics& intrinsics)
{
    intrinsics_ = intrinsics;
    return true;
}

void OrbbecCamera::GetActiveProfiles(int& cw, int& ch, int& dw, int& dh) const
{
    cw = colorW_; ch = colorH_; dw = depthW_; dh = depthH_;
}
