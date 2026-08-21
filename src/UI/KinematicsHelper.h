#pragma once

#include "Core/Kinematics.h"
#include "Config/ConfigManager.h"

// ============================================================
// 运动学实例工厂（UI 层辅助，消除多页面重复的 config→Kinematics 组装）
// 从 ConfigManager 读取 kinematics.links.* 与 tcpCalibration.toolOffsetX/Y/Z，
// 组装并返回一个已配置的 Kinematics。AutoRunPage 坐标面板与 ProcessPage
// 示教读取共用，保证三处（含 SequenceWorker::ReloadFromConfig）读取一致。
// ============================================================
namespace KinematicsHelper {

inline Kinematics FromConfig()
{
    auto& cfg = ConfigManager::instance();
    Kinematics kin;
    kin.SetParams(cfg.getValue("kinematics.links.l1", 138.83),
                  cfg.getValue("kinematics.links.l2", 166.86),
                  cfg.getValue("kinematics.links.z0", 0.0),
                  cfg.getValue("kinematics.links.h1", 0.0));
    kin.SetTCP(cfg.getValue("tcpCalibration.toolOffsetX", 0.0),
               cfg.getValue("tcpCalibration.toolOffsetY", 0.0),
               cfg.getValue("tcpCalibration.toolOffsetZ", 0.0));
    return kin;
}

// 读当前 config 的运动学参数快照（供"参数是否变化"判断用）
inline void ReadConfigParams(double& l1, double& l2, double& z0, double& h1,
                             double& tcpX, double& tcpY, double& tcpZ)
{
    auto& cfg = ConfigManager::instance();
    l1 = cfg.getValue("kinematics.links.l1", 138.83);
    l2 = cfg.getValue("kinematics.links.l2", 166.86);
    z0 = cfg.getValue("kinematics.links.z0", 0.0);
    h1 = cfg.getValue("kinematics.links.h1", 0.0);
    tcpX = cfg.getValue("tcpCalibration.toolOffsetX", 0.0);
    tcpY = cfg.getValue("tcpCalibration.toolOffsetY", 0.0);
    tcpZ = cfg.getValue("tcpCalibration.toolOffsetZ", 0.0);
}

} // namespace KinematicsHelper
