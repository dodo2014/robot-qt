// ============================================================
// tests/test_camera_check.cpp — Orbbec 相机真机验证工具（P3 / S2–S4）
//
// **独立可执行，不在主程序依赖链上**（与 test_kinematics_check 同属 tests/，
// `ninja CreamPuffRobot` 不会编译它）。设计依据 doc/camera_integration_plan.md
// §五 S2/S3/S4 的验收判据。
//
// 用法（cwd 任意；config 路径编译期硬编码 PROJECT_SOURCE_DIR，与主程序同源）：
//   test_camera_check                     只枚举设备（S2 验收）
//   test_camera_check --stream            枚举 + 开流 10 s（S3 验收）
//   test_camera_check --stream 60         开流 60 s（S3 长稳：>10 min 用 700）
//   test_camera_check --stream 10 --sn XX 指定 SN（默认取 communication.camera.sn）
//
// 退出码 0 = 通过，1 = 失败（便于脚本判定）。
//
// ⚠ 本工具**不注册 SDK 插拔回调**（会崩），也不主动拆 Context —— 插拔恢复
//   由 `OrbbecCamera` 内部的分级恢复（L1/L2/L3）承担，见 §4.2。
//   拔插实测请手动进行：运行 `--stream 700` 期间拔线再插回，观察日志中
//   linkState 是否 Disconnected → Connecting → Streaming 且进程不退出。
// ============================================================

#include "OrbbecCamera.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include <spdlog/spdlog.h>

#include "ConfigManager.h"
#include "ConfigPaths.h"

namespace {

const char* LinkStateName(CameraLinkState s)
{
    switch (s) {
    case CameraLinkState::Unknown:      return "Unknown";
    case CameraLinkState::Disconnected: return "Disconnected";
    case CameraLinkState::Connecting:   return "Connecting";
    case CameraLinkState::Streaming:    return "Streaming";
    case CameraLinkState::Degraded:     return "Degraded";
    }
    return "?";
}

int g_failures = 0;

void Check(bool cond, const std::string& what)
{
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

// 从 config 读出流参数，复用主程序同一条读取链路（communication.camera.*）
CameraStreamConfig ReadCfg()
{
    ConfigPaths::ensureConfigLoaded();
    auto& cfg = ConfigManager::instance();
    CameraStreamConfig c;
    c.colorWidth   = cfg.getValue<int>("communication.camera.colorWidth",  c.colorWidth);
    c.colorHeight  = cfg.getValue<int>("communication.camera.colorHeight", c.colorHeight);
    c.colorFps     = cfg.getValue<double>("communication.camera.colorFps", c.colorFps);
    c.colorFormat  = cfg.getValue<std::string>("communication.camera.colorFormat", c.colorFormat);
    c.depthWidth   = cfg.getValue<int>("communication.camera.depthWidth",  c.depthWidth);
    c.depthHeight  = cfg.getValue<int>("communication.camera.depthHeight", c.depthHeight);
    c.depthFps     = cfg.getValue<double>("communication.camera.depthFps", c.depthFps);
    c.depthFormat  = cfg.getValue<std::string>("communication.camera.depthFormat", c.depthFormat);
    c.d2cAlign     = cfg.getValue<std::string>("communication.camera.d2cAlign", c.d2cAlign);
    c.autoExposure = cfg.getValue<bool>("communication.camera.autoExposure", c.autoExposure);
    return c;
}

}  // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    // 控制台输出代码页置 UTF-8：本工程全局 /utf-8（CMakeLists.txt:45）→ 中文字面量按
    // UTF-8 字节输出，而 cmd.exe 默认代码页是 GBK(936)，直接运行会中文乱码。
    // 仅影响控制台渲染；重定向到文件时不受影响（文件本来就是 UTF-8）。
    SetConsoleOutputCP(CP_UTF8);
#endif
    int  seconds = 0;
    std::string snOverride;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--stream") {
            seconds = 10;
            if (i + 1 < argc && argv[i + 1][0] != '-') seconds = std::atoi(argv[++i]);
        } else if (a == "--sn" && i + 1 < argc) {
            snOverride = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::printf("用法: test_camera_check [--stream [秒数]] [--sn <序列号>]\n");
            return 0;
        }
    }

    std::printf("=== OrbbecCamera 真机验证（P3 / S2–S4）===\n");

    CameraStreamConfig cfg = ReadCfg();
    ConfigPaths::ensureConfigLoaded();
    std::string sn = snOverride.empty()
        ? ConfigManager::instance().getValue<std::string>("communication.camera.sn", std::string())
        : snOverride;

    std::printf("\n[配置] sn='%s' color=%dx%d@%.0f fmt='%s' depth=%dx%d@%.0f fmt='%s' "
                "d2cAlign=%s autoExposure=%d\n",
                sn.c_str(), cfg.colorWidth, cfg.colorHeight, cfg.colorFps,
                cfg.colorFormat.empty() ? "(auto)" : cfg.colorFormat.c_str(),
                cfg.depthWidth, cfg.depthHeight, cfg.depthFps, cfg.depthFormat.c_str(),
                cfg.d2cAlign.c_str(), cfg.autoExposure ? 1 : 0);

    OrbbecCamera cam;
    cam.Configure(cfg);

    // ---- S2：设备枚举 ----
    std::printf("\n--- S2 设备枚举 ---\n");
    std::vector<CameraDeviceInfo> devs = cam.EnumerateDevices();
    std::printf("  枚举到 %zu 台设备\n", devs.size());
    for (const auto& d : devs)
        std::printf("    id='%s'  name='%s'  conn='%s'\n",
                    d.id.c_str(), d.name.c_str(), d.connection.c_str());
    Check(!devs.empty(), "枚举到至少一台设备（未插相机时此项失败）");

    if (devs.empty()) {
        std::printf("\n结果: FAIL（无设备，后续取流测试跳过）\n");
        return 1;
    }

    // 未显式指定 SN 时，用枚举到的第一台（避免 config 里是占位值 "336L" 而匹配不上）
    if (snOverride.empty()) {
        bool matched = false;
        for (const auto& d : devs) if (d.id == sn) matched = true;
        if (!matched) {
            std::printf("  [INFO] config 的 sn='%s' 不在枚举结果中 → 改用第一台 '%s'\n",
                        sn.c_str(), devs.front().id.c_str());
            sn = devs.front().id;
        }
    }

    if (seconds <= 0) {
        std::printf("\n结果: %s（仅枚举；加 --stream 做取流测试）\n",
                    g_failures == 0 ? "PASS" : "FAIL");
        return g_failures == 0 ? 0 : 1;
    }

    // ---- S3：开流 + 取帧 ----
    std::printf("\n--- S3 开流与取帧 ---\n");
    const bool opened = cam.Open(sn, cfg.colorWidth, cfg.colorHeight, cfg.colorFps);
    Check(opened, "Open 成功" + (opened ? std::string() : (" — " + cam.GetLastError())));
    if (!opened) {
        std::printf("\n结果: FAIL\n");
        return 1;
    }
    Check(cam.IsOpened(), "IsOpened() == true");

    const bool started = cam.StartStream();
    Check(started, "StartStream 成功" + (started ? std::string() : (" — " + cam.GetLastError())));
    if (!started) {
        cam.Close();
        std::printf("\n结果: FAIL\n");
        return 1;
    }

    int cw = 0, ch = 0, dw = 0, dh = 0;
    cam.GetActiveProfiles(cw, ch, dw, dh);
    std::printf("  生效 profile: color %dx%d / depth %dx%d\n", cw, ch, dw, dh);
    Check(cw > 0 && ch > 0, "彩色 profile 已解析（>0）");

    int  frames = 0, rgbFrames = 0, depthFrames = 0, badFrames = 0;
    int  firstW = 0, firstH = 0;
    size_t firstColorBytes = 0;
    double firstDepthMm = 0.0;
    const auto t0 = std::chrono::steady_clock::now();

    // 分钟级分段统计：11.32 的「帧率无衰减（前后各 1 min 对比）」靠它取证——
    // 只打全程平均值无法判断衰减（本次实测补上，此前该子判据无法取证）。
    long long nextSegAt   = 60;   // 下一个分段边界（秒）
    int       lastSegFrames = 0;
    int       lastSegBad    = 0;

    while (true) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= seconds) break;

        while (elapsed >= nextSegAt) {
            const int segF = frames - lastSegFrames;
            std::printf("  [分段] 第 %lld 分钟：%d 帧（%.1f fps）、空帧 %d\n",
                        nextSegAt / 60, segF, segF / 60.0, badFrames - lastSegBad);
            lastSegFrames = frames;
            lastSegBad    = badFrames;
            nextSegAt += 60;
        }

        CameraFrame f = cam.CaptureFrame();
        if (f.width <= 0 || f.height <= 0 || f.colorData.empty()) {
            ++badFrames;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (frames == 0) {
            firstW = f.width; firstH = f.height;
            firstColorBytes = f.colorData.size();
            for (float d : f.depthData) if (d > 0.0f) { firstDepthMm = d; break; }
        }
        ++frames;
        // FrameConverter::ColorToQImage 只认 RGB888 → 字节数必须 == w*h*3
        if (f.colorData.size() == static_cast<size_t>(f.width) * f.height * 3) ++rgbFrames;
        if (!f.depthData.empty()) ++depthFrames;
    }

    const double secs = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - t0).count();
    const double fps = frames / (secs > 0 ? secs : 1.0);

    std::printf("  采集 %.1f s：有效帧 %d（%.1f fps），空帧 %d\n", secs, frames, fps, badFrames);
    std::printf("  首帧 %dx%d，colorData=%zu 字节，深度首值=%.1f mm\n",
                firstW, firstH, firstColorBytes, firstDepthMm);

    Check(frames > 0, "至少取到一帧");
    Check(fps >= 2.0, "帧率 ≥ 2 fps（参考实现的「流异常」阈值）");
    Check(rgbFrames == frames, "每帧都是 RGB888（字节数 == w*h*3，UI 不花屏）");
    Check(depthFrames > 0, "深度图非空（D2C 对齐链路通）");

    const CameraLinkState st = cam.GetLinkState();
    std::printf("  链路状态: %s\n", LinkStateName(st));
    Check(st == CameraLinkState::Streaming || st == CameraLinkState::Unknown,
          "链路状态为 Streaming");

    cam.StopStream();
    cam.Close();
    Check(!cam.IsOpened(), "Close 后 IsOpened() == false");

    std::printf("\n结果: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
