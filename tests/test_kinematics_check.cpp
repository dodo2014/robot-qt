// tests/test_kinematics_check.cpp — 运动学 FK/IK 验证程序（2026-08-31 固化）
//
// 用法：
//   test_kinematics_check                              运行内置验证（真机摆位回归 + FK↔IK 往返自检）
//   test_kinematics_check fk <j1> <j2> <z> <r>         轴角(逻辑) → 正解坐标
//   test_kinematics_check ik <x> <y> <z> <r> [curJ2]   坐标 → 逆解轴角（双解就近，默认 curJ2=0）
//
// 运动学参数自动读取 config/config.json（kinematics.links + tcpCalibration + axes 软限位），
// config 更新后无需改代码。退出码：0=全部通过，1=存在失败（便于脚本/CI 调用）。
//
// 手动查询示例（对应真机摆位验证，2026-08-31）：
//   test_kinematics_check fk 0 0.5 0 179.9      → 期望 x=358.68 y=1.92 z=165
//   test_kinematics_check fk -90 -0.1 0 180.1   → 期望 x=-0.38 y=-358.69 z=165

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <utility>

#include "Kinematics.h"

namespace {

constexpr double kTolMm = 0.01;     // 真机用例容差：界面坐标显示精度 0.01mm
constexpr double kTolRound = 1e-6;  // FK↔IK 往返数学容差（双精度应达 1e-9 量级，取宽松值）

bool LoadKinematicsFromConfig(Kinematics& kin)
{
    const std::string path = std::string(PROJECT_SOURCE_DIR) + "/config/config.json";
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "[FAIL] cannot open %s\n", path.c_str());
        return false;
    }
    nlohmann::json cfg = nlohmann::json::parse(in, nullptr, false);
    if (cfg.is_discarded()) {
        std::fprintf(stderr, "[FAIL] config parse error: %s\n", path.c_str());
        return false;
    }
    const auto& links = cfg.at("kinematics").at("links");
    const auto& tcp = cfg.at("tcpCalibration");
    kin.SetParams(links.at("l1").get<double>(), links.at("l2").get<double>(),
                  links.at("z0").get<double>(), links.at("h1").get<double>());
    kin.SetTCP(tcp.at("toolOffsetX").get<double>(), tcp.at("toolOffsetY").get<double>(),
               tcp.at("toolOffsetZ").get<double>());

    const auto& axes = cfg.at("axes");
    const auto lim = [&axes](const char* key) {
        const auto& a = axes.at(key);
        return std::pair<double, double>{a.at("limitMin").get<double>(),
                                         a.at("limitMax").get<double>()};
    };
    const auto j1 = lim("Axis_J1");
    const auto j2 = lim("Axis_J2");
    const auto z = lim("Axis_Z");
    const auto r = lim("Axis_R");
    kin.SetJointLimits(j1.first, j1.second, j2.first, j2.second,
                       z.first, z.second, r.first, r.second);
    return true;
}

struct TruthCase
{
    const char* name;
    Joints j;
    Pose p;
};

// 真机摆位实测用例（2026-08-31 现场验证，误差 ≤0.01mm 见 TEST_RECORD.md）
const TruthCase kTruthCases[] = {
    {"真机摆位#1  J(0,0.5,0,179.9)",   {0.0, 0.5, 0.0, 179.9},    {358.68, 1.92, 165.0, 179.9}},
    {"真机摆位#2  J(-90,-0.1,0,180.1)", {-90.0, -0.1, 0.0, 180.1}, {-0.38, -358.69, 165.0, 180.1}},
};

int RunBuiltin(const Kinematics& kin)
{
    int failed = 0;
    int total = 0;

    std::printf("=== 真机摆位回归用例（容差 %.2fmm）===\n", kTolMm);
    for (const auto& c : kTruthCases) {
        ++total;
        const Pose out = kin.Forward(c.j);
        const double dx = out.x - c.p.x, dy = out.y - c.p.y;
        const double dz = out.z - c.p.z, dr = out.r - c.p.r;
        const bool ok = std::fabs(dx) <= kTolMm && std::fabs(dy) <= kTolMm &&
                        std::fabs(dz) <= kTolMm && std::fabs(dr) <= kTolMm;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", c.name);
        std::printf("       calc x=%+.4f y=%+.4f z=%+.4f r=%+.4f\n", out.x, out.y, out.z, out.r);
        std::printf("       real x=%+.4f y=%+.4f z=%+.4f r=%+.4f\n", c.p.x, c.p.y, c.p.z, c.p.r);
        if (!ok) {
            ++failed;
            std::printf("       delta x=%+.4f y=%+.4f z=%+.4f r=%+.4f  (超出容差)\n", dx, dy, dz, dr);
        }
    }

    std::printf("\n=== FK<->IK 往返自检（限位内随机 20 组，容差 %.0e）===\n", kTolRound);

    // 工作区边界回归：2026-08-31 真机点_001 (358.69, 1.15) 极径 358.6918 微超理论最大
    // 半径 358.69（显示舍入所致），IK 必须成功（0.1mm 边界容差），曾以 0.001mm 容差误拒。
    {
        ++total;
        Pose p{};
        p.x = 358.69; p.y = 1.15; p.z = 165.0; p.r = 179.8;
        Joints sol{};
        const bool ok = kin.InverseSmart(p, sol, 0.0);
        if (ok) {
            const Pose back = kin.Forward(sol);
            const double e = std::hypot(back.x - p.x, back.y - p.y);
            std::printf("[PASS] 边界点 IK 成功: J(%.3f,%.3f) 还原误差 %.2e mm\n",
                        sol.j1, sol.j2, e);
        } else {
            ++failed;
            std::printf("[FAIL] 边界点 (358.69, 1.15) IK 被拒（边界容差回归！）\n");
        }
    }

    std::mt19937 rng(20260831);
    std::uniform_real_distribution<double> dJ1(-100.0, 5.0);
    std::uniform_real_distribution<double> dJ2(-25.0, 150.0);
    for (int i = 0; i < 20; ++i) {
        ++total;
        Joints src{};
        src.j1 = dJ1(rng);
        src.j2 = dJ2(rng);
        src.z = 0.0;
        src.r = 0.0;
        const Pose p = kin.Forward(src);
        Joints back{};
        if (!kin.InverseSmart(p, back, src.j2)) {
            ++failed;
            std::printf("[FAIL] 往返#%d IK 求解失败: J(%.3f,%.3f) -> P(%.3f,%.3f)\n",
                        i + 1, src.j1, src.j2, p.x, p.y);
            continue;
        }
        const double e1 = std::fabs(back.j1 - src.j1);
        const double e2 = std::fabs(back.j2 - src.j2);
        const bool ok = e1 <= kTolRound && e2 <= kTolRound;
        if (!ok) {
            ++failed;
            std::printf("[FAIL] 往返#%d J(%.4f,%.4f) -> IK J(%.4f,%.4f) 误差 e1=%.2e e2=%.2e\n",
                        i + 1, src.j1, src.j2, back.j1, back.j2, e1, e2);
        }
    }
    if (failed == 0)
        std::printf("[PASS] 20/20 往返还原\n");

    std::printf("\n%s（%d/%d 通过）\n", failed == 0 ? "ALL PASS" : "FAILED", total - failed, total);
    return failed == 0 ? 0 : 1;
}

int RunFk(const Kinematics& kin, int argc, char** argv)
{
    if (argc != 6) {
        std::fprintf(stderr, "用法: test_kinematics_check fk <j1> <j2> <z> <r>\n");
        return 1;
    }
    Joints j{};
    j.j1 = std::atof(argv[2]);
    j.j2 = std::atof(argv[3]);
    j.z = std::atof(argv[4]);
    j.r = std::atof(argv[5]);
    const Pose p = kin.Forward(j);
    std::printf("FK J(j1=%g, j2=%g, z=%g, r=%g)\n", j.j1, j.j2, j.z, j.r);
    std::printf("  -> x=%.4f  y=%.4f  z=%.4f  r=%.4f\n", p.x, p.y, p.z, p.r);
    return 0;
}

int RunIk(const Kinematics& kin, int argc, char** argv)
{
    if (argc != 6 && argc != 7) {
        std::fprintf(stderr, "用法: test_kinematics_check ik <x> <y> <z> <r> [curJ2]\n");
        return 1;
    }
    Pose p{};
    p.x = std::atof(argv[2]);
    p.y = std::atof(argv[3]);
    p.z = std::atof(argv[4]);
    p.r = std::atof(argv[5]);
    const double curJ2 = (argc == 7) ? std::atof(argv[6]) : 0.0;
    Joints j{};
    if (!kin.InverseSmart(p, j, curJ2)) {
        std::printf("IK P(x=%g, y=%g, z=%g, r=%g) -> 无合法解（工作区/限位拒绝）\n", p.x, p.y, p.z, p.r);
        return 1;
    }
    std::printf("IK P(x=%g, y=%g, z=%g, r=%g) [curJ2=%g]\n", p.x, p.y, p.z, p.r, curJ2);
    std::printf("  -> j1=%.4f  j2=%.4f  z=%.4f  r=%.4f\n", j.j1, j.j2, j.z, j.r);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::off);  // 静音 Kinematics 构造/IK 的内部日志，输出由本程序控制

    Kinematics kin;
    if (!LoadKinematicsFromConfig(kin)) {
        return 1;
    }

    if (argc >= 2 && std::string(argv[1]) == "fk")
        return RunFk(kin, argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "ik")
        return RunIk(kin, argc, argv);
    if (argc >= 2) {
        std::fprintf(stderr,
                     "用法:\n"
                     "  test_kinematics_check                          内置验证（真机回归+往返自检）\n"
                     "  test_kinematics_check fk <j1> <j2> <z> <r>     正解查询\n"
                     "  test_kinematics_check ik <x> <y> <z> <r> [curJ2] 逆解查询\n");
        return 1;
    }
    return RunBuiltin(kin);
}
