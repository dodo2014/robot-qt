#pragma once

// ============================================================
// OrbbecCamera — Orbbec Gemini 336（3D 相机）HAL 实现
//
// 设计依据：doc/camera_integration_plan.md（§三 事实核实、§四 去抖与分级恢复、
// S1–S4 实现方案、§六 配置 schema）。**相机侧的三条硬约束（用户实测，勿违）**：
//   ① 不注册 SDK 插拔回调（registerDeviceChangedCallback 会崩）→ 改轮询探测；
//   ② 高分辨率必须**软件对齐**（ob::Align 逐帧 process），不是 set_align_mode(SW_MODE)；
//   ③ 拔线瞬间直接取帧会崩 → 取帧前必须先探活。
//
// 本文件**不包含 libobsensor 头文件**（只用前向声明）——SDK 只在 .cpp 内可见，
// 避免 SDK 头污染整个 HAL 与上层。`USE_ORBBEC=OFF` 时本文件不参与编译。
// ============================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "ICamera.h"

namespace ob {
class Context;
class Device;
class Pipeline;
class Align;
class FormatConvertFilter;
class Frame;
}  // namespace ob

class OrbbecCamera : public ICamera
{
public:
    OrbbecCamera();
    ~OrbbecCamera() override;

    void Configure(const CameraStreamConfig& config) override;
    bool SupportsDeviceEnumeration() const override { return true; }
    std::vector<CameraDeviceInfo> EnumerateDevices() override;
    // P4：视觉页 6 个流参数下拉框的数据源（V1/V2 修正——按 SDK 实际枚举填充，
    // 不照抄需求里的预置项）。只读查询，对已开流无副作用。
    std::vector<CameraStreamProfile> GetStreamProfiles(bool isDepth) override;

    bool Open(const std::string& deviceId, int width, int height, double fps) override;
    void Close() override;
    bool IsOpened() const override;

    bool StartStream() override;
    void StopStream() override;
    bool IsStreaming() const override;

    CameraFrame CaptureFrame() override;

    // TR-123（2026-10-12）：取「最近一帧」快照，**只读最新帧槽、不触碰 pipeline**。
    // `SequenceWorker` 的「识别」动作改走本路径 ⇒ 消除「采集线程与 worker 线程同时进
    // 同一 pipeline」的并发 UB（原走 `CaptureFrame()`，会调 `HandleGrabFailure()`
    // → `DestroyPipeline()` / 重建管线）。恢复状态机由采集线程独占驱动。
    CameraFrame TakeLatestFrame() override;

    CameraIntrinsics GetIntrinsics() const override;
    bool SetIntrinsics(const CameraIntrinsics& intrinsics) override;

    std::string GetLastError() const override;

    // 链路状态快照（§4.3）：ICamera 不是 QObject，不能发信号——由
    // HardwareManager::PollTick 边沿检测后经 cameraStateChanged 广播给 UI
    CameraLinkState GetLinkState() const override { return linkState_.load(); }

    /// 主动探活（**降频**：内部缓存 500 ms，见 §4.2 L0）。
    /// 设备枚举计数 > 0 **且** `getDeviceInfo()` 不抛（僵尸设备探测，参考实现 :243-256）。
    /// 取帧前必须先调它——拔线瞬间直接取帧会崩。
    bool IsDevicePresent();

    /// 当前生效的彩色/深度 profile（诊断与日志用；未开流时为 0）
    void GetActiveProfiles(int& cw, int& ch, int& dw, int& dh) const;

private:
    void SetError(const std::string& msg);

    // ---- SDK 资源生命周期 ----
    bool CreateContext();
    bool BuildAndStartPipeline(const std::string& deviceId);
    bool ConfigureStreams();     // 动态枚举 profile + 组装 Config（E7/E8）
    void ConfigureAutoExposure();
    void WarmUp();               // 连接成功后 1.5 s + 丢若干帧（参考实现 :145）
    // 拔线后 stop() 会抛 Bad Magic 甚至段错误 → 吞异常 + 强制置空（参考实现 :294-333）
    void DestroyPipeline();
    void DestroyContext();       // 逼 SDK 重新扫描 USB 枚举（参考实现 :232-235）

    // ---- 分级恢复（§4.2）----
    void HandleGrabFailure();
    void ResetFailureCounters();
    bool probeDevices();         // 真正做枚举+探活（IsDevicePresent 的降频实现）
    // 可中断睡眠（TR-111）：`wantStreaming_` 变假（用户停流 / 关设备）时立即返回 false。
    // L3 要睡 6 s，若不可中断，用户在重连过程中停流会让 `CameraManager` 析构的
    // `cameraThread_->wait(2000)` 超时。
    bool InterruptibleSleep(std::chrono::milliseconds total);

    // ---- 帧归一化：任意彩色格式 → RGB888（FrameConverter 只认 RGB888）----
    bool NormalizeColor(const std::shared_ptr<ob::Frame>& src, CameraFrame& out);

    // 最新帧槽：抓取失败时回放 1 s 内的上一帧，避免 UI 因单帧丢失闪黑
    // （软对齐偶发失败是已知现象，§4.2 / 参考实现 :524-536）
    void StoreLastGood(const CameraFrame& f);
    CameraFrame TakeLastGood();

    CameraStreamConfig cfg_;
    std::string lastError_;
    std::string currentSn_;      // 本次打开用的 SN（恢复重建时复用）
    mutable std::mutex errMutex_;

    std::shared_ptr<ob::Context> context_;
    std::shared_ptr<ob::Device>  device_;
    std::shared_ptr<ob::Pipeline> pipeline_;
    std::shared_ptr<ob::Align>   align_;                  // SW 对齐滤波器（d2cAlign == "SW"）
    std::shared_ptr<ob::FormatConvertFilter> colorConv_;  // 非 RGB 输入的解码器
    int convSrcFormat_ = -1;     // colorConv_ 当前配置的源格式（OBFormat 的整数值）

    std::atomic<bool> opened_{false};
    std::atomic<bool> streaming_{false};
    // 「期望在流」意图位（TR-111，2026-09-23）。与 `streaming_`（**当前**是否真在取帧）
    // 必须分开：`DestroyPipeline()` 会把 `streaming_` 清零，但**恢复失败**与**用户主动停流**
    // 这两种情形下 `streaming_` 都是 false，而只有后者才不该重连。
    // `StartStream()` 置真；`StopStream()` / `Close()` 置假（主动停流）。
    std::atomic<bool> wantStreaming_{false};
    std::atomic<CameraLinkState> linkState_{CameraLinkState::Disconnected};

    mutable std::mutex frameMutex_;
    CameraFrame lastGoodFrame_;
    std::chrono::steady_clock::time_point lastGoodAt_{};

    int colorFormat_ = -1;       // 实际生效的彩色 OBFormat
    int colorW_ = 0, colorH_ = 0, depthW_ = 0, depthH_ = 0;

    CameraIntrinsics intrinsics_;

    // 分级恢复状态（§4.2）
    int consecutiveFailures_ = 0;
    int openFailures_ = 0;
    // 最近一次已发起的恢复档位（0 = 尚未发起，1/2/3 = L1/L2/L3）。
    // 用途：**每个冷却窗口最多升级一档**（TR-111）——冷却期内计数会继续累加，
    // 若直接按计数选档，L1 之后的 3 s 里计数已越过 L3 阈值，L2 永远不会执行。
    int lastAttemptLevel_ = 0;
    std::chrono::steady_clock::time_point lastReconnectAt_{};
    bool alignWarned_ = false;   // 软对齐偶发失败只提示一次，避免刷屏

    // 探活降频缓存（§4.2 L0：约 500 ms 一次）
    std::chrono::steady_clock::time_point lastProbeAt_{};
    bool lastProbeResult_ = false;
};
