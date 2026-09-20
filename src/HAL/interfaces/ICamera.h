#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CameraIntrinsics
{
    double fx = 0.0, fy = 0.0;
    double cx = 0.0, cy = 0.0;
    double k1 = 0.0, k2 = 0.0, p1 = 0.0, p2 = 0.0;
    int width  = 640;
    int height = 480;
};

struct CameraFrame
{
    int64_t timestamp = 0;
    int width  = 0;
    int height = 0;
    std::vector<uint8_t> colorData;   // RGB/BGR
    std::vector<float>   depthData;   // 深度图 (mm)
    std::vector<float>   pointCloud;  // 可选: XYZ 点云
};

// 相机设备信息（P3 / S2 设备枚举用）。
// `id` 是**设备选择用的唯一键**，对 Orbbec 即序列号（SN）——与
// `communication.camera.sn` 同语义。
struct CameraDeviceInfo
{
    std::string id;          // 序列号（SN）
    std::string name;        // 型号名，如 "Orbbec Gemini 336"
    std::string connection;  // 连接方式，如 "USB3.2"
};

// 相机流参数（P3 新增，schema 见 doc/camera_integration_plan.md §六）。
//
// **由 `CameraManager` 从 config 的 `communication.camera.*` 读出后经
// `Configure()` 下发**——相机实现**不自己读 config**，沿用「HAL 设备实现不主动
// 读配置」的既有边界（与 `BoPaiCard` / `XRServo` 一致；配置读取集中在
// `CameraManager`，它已经是 `communication.camera.sn` 的读取点）。
struct CameraStreamConfig
{
    // 彩色流
    int    colorWidth   = 1280;
    int    colorHeight  = 720;
    double colorFps     = 30.0;
    // 首选彩色格式（"MJPG" / "YUYV" / "RGB"）。**空串 = 用实现内置的尝试队列**。
    // 高分辨率彩色在 Gemini 336 上实际多为 MJPG（E7），故配置默认 MJPG。
    std::string colorFormat;

    // 深度流
    int    depthWidth   = 848;
    int    depthHeight  = 480;
    double depthFps     = 30.0;
    std::string depthFormat = "Y16";

    // D2C 对齐："SW"（软件，ob::Align 逐帧 process）/ "HW"（仅 640×480+848×480 档
    // 可用）/ "OFF"。**高分辨率必须 SW**（E3 / 决策 2）。
    std::string d2cAlign = "SW";

    // 本版固定 true（决策 4：移除曝光控件，固定自动曝光）
    bool autoExposure = true;
};

// 单个可用流 profile（P4 新增）。
//
// 用途：视觉页「彩色流 / 深度流」的分辨率 / 格式 / 帧率 6 个下拉框**必须按 SDK 实际
// 枚举结果填充**（`doc/vision_test_page_redesign_plan.md` 的 V1 / V2 修正）——需求里的
// 预置项（如彩色格式含 `Y16`、分辨率含 `1920x1080`）都是**假设**，硬编码选到该机型
// 不支持的组合会直接开流失败。
//
// `SimCamera` 返回空列表（非纯虚默认实现）→ UI 侧回落为「只显示 config 现值」。
struct CameraStreamProfile
{
    int         width  = 0;
    int         height = 0;
    double      fps    = 0.0;
    std::string format;   // "MJPG" / "YUYV" / "RGB" / "Y16" ...
};

// 相机链路状态快照（P3，见 doc/camera_integration_plan.md §4.3）。
//
// `ICamera` 不是 `QObject`，无法直接发信号——由 `HardwareManager::PollTick`
// 边沿检测后经 `cameraStateChanged` 广播给 UI。**不在相机实现里碰 Qt 对象**，
// 沿用「HAL 底层不依赖 Qt 信号」的既有边界。
enum class CameraLinkState
{
    Unknown = 0,    // 该实现不提供链路状态（SimCamera 即此情形）→ 不做边沿检测与自动恢复
    Disconnected,   // 未连接 / 已掉线
    Connecting,     // 正在连接或正在恢复
    Streaming,      // 正常出帧
    Degraded,       // 在流但帧率异常 / 刚执行过恢复动作
};

class ICamera
{
public:
    virtual ~ICamera() = default;

    /// 链路状态快照。**非纯虚**：默认 `Unknown` = 「该实现不提供链路状态」，
    /// 此时 `HardwareManager` 不做相机状态边沿检测与自动恢复——`SimCamera` 零改动。
    virtual CameraLinkState GetLinkState() const { return CameraLinkState::Unknown; }

    /// 下发流参数。**必须在 `Open()` 之前调用**才生效。
    ///
    /// **非纯虚**（P3 新增）：默认空实现，`SimCamera` 无需改动。
    /// 加非纯虚方法是本项目的既定做法——`ICamera` 的 9 个纯虚方法语义已够用，
    /// 需求的 `Connect`/`Disconnect` 与 `Open`/`Close` 重复，重命名只会牵动 5 处
    /// 调用点且换不来任何能力（见 `doc/camera_integration_plan.md` 评审 E1）。
    virtual void Configure(const CameraStreamConfig& config) { (void)config; }

    /// 该实现是否**支持**设备枚举（P4 / 方案 V9）。
    ///
    /// 为什么不靠 `EnumerateDevices()` 返回空来判断：**空列表有歧义**——既可能是
    /// 「实现不支持」，也可能是「支持但当前一台都没插」。UI 需要区分这两者才能决定
    /// 「刷新设备」是置灰还是提示「未枚举到设备」，故显式声明能力。
    /// **非纯虚**：默认 `false`，`SimCamera` 无需改动。
    virtual bool SupportsDeviceEnumeration() const { return false; }

    /// 枚举当前可用设备。**非纯虚**：默认返回空列表，`SimCamera` 无需改动。
    /// 用于「刷新设备」填充 SN 下拉框（S2）。
    virtual std::vector<CameraDeviceInfo> EnumerateDevices() { return {}; }

    /// 查询某个流的可用 profile 列表。**非纯虚**：默认返回空列表（`SimCamera` 即此情形）。
    ///
    /// `isDepth = false` 查彩色流，`true` 查深度流。**实现方自行保证不干扰正在跑的流**：
    /// 已开流时从既有 pipeline 查，未开流时才临时建一个（`OrbbecCamera` 的做法）。
    virtual std::vector<CameraStreamProfile> GetStreamProfiles(bool isDepth) { (void)isDepth; return {}; }

    virtual bool Open(const std::string& deviceId, int width, int height, double fps) = 0;
    virtual void Close() = 0;
    virtual bool IsOpened() const = 0;

    virtual bool StartStream() = 0;
    virtual void StopStream() = 0;
    virtual bool IsStreaming() const = 0;

    virtual CameraFrame CaptureFrame() = 0;

    virtual CameraIntrinsics GetIntrinsics() const = 0;
    virtual bool SetIntrinsics(const CameraIntrinsics& intrinsics) = 0;

    virtual std::string GetLastError() const = 0;
};
