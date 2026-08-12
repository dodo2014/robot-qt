#include "AxisConfigService.h"

#include "ConfigManager.h"

namespace {

// config 中每个逻辑轴的 key 名（与 config/axes 对象 key 对应，与 HardwareManager 一致）
const char* kAxisConfigKeys[] = {
    "Axis_J1", "Axis_J2", "Axis_Z", "Axis_R", "Axis_Gripper", "Axis_Extruder"
};

std::string AxisKey(LogicalAxis axis)
{
    int i = static_cast<int>(axis);
    return (i >= 0 && i < static_cast<int>(LogicalAxis::Count)) ? kAxisConfigKeys[i] : "";
}

std::string AxisCfgPath(LogicalAxis axis)
{
    std::string key = AxisKey(axis);
    return key.empty() ? std::string() : ("axes." + key);
}

} // namespace

AxisConfigService::AxisConfigService(QVector<AxisConfig>& configs)
    : configs_(configs)
{
}

double AxisConfigService::GetJogSpeed(LogicalAxis axis) const
{
    int i = static_cast<int>(axis);
    if (i >= 0 && i < configs_.size())
        return configs_[i].jogSpeed;
    return 100.0;
}

double AxisConfigService::GetMaxSpeed(LogicalAxis axis) const
{
    // 直接读 config（axes.<key>.maxSpeed），保证与 ConfigPage「电控与映射」的编辑实时一致
    int i = static_cast<int>(axis);
    if (i < 0 || i >= static_cast<int>(LogicalAxis::Count)) return 150.0;
    std::string path = AxisCfgPath(axis);
    if (path.empty()) return 150.0;
    return ConfigManager::instance().getValue<double>(path + ".maxSpeed", 150.0);
}

bool AxisConfigService::SetJogSpeed(LogicalAxis axis, double mmOrDegPerSec)
{
    int i = static_cast<int>(axis);
    if (i < 0 || i >= configs_.size()) return false;
    configs_[i].jogSpeed = mmOrDegPerSec;

    std::string path = AxisCfgPath(axis);
    if (!path.empty())
        ConfigManager::instance().set(path + ".jogSpeed", mmOrDegPerSec);
    return true;
}

QString AxisConfigService::AxisUnit(LogicalAxis axis) const
{
    int i = static_cast<int>(axis);
    if (i < 0 || i >= static_cast<int>(LogicalAxis::Count)) return QString();
    if (AxisMap::Get(axis).type == AxisBinding::Type::Servo)
        return QStringLiteral("\xC2\xB0");
    if (i < configs_.size() && configs_[i].axisType == 1)
        return QStringLiteral("mm");
    return QStringLiteral("\xC2\xB0");
}

double AxisConfigService::GetLimitMin(LogicalAxis axis) const
{
    int i = static_cast<int>(axis);
    if (i < 0 || i >= static_cast<int>(LogicalAxis::Count)) return -180.0;
    std::string path = AxisCfgPath(axis);
    if (path.empty()) return -180.0;
    return ConfigManager::instance().getValue<double>(path + ".limitMin", -180.0);
}

double AxisConfigService::GetLimitMax(LogicalAxis axis) const
{
    int i = static_cast<int>(axis);
    if (i < 0 || i >= static_cast<int>(LogicalAxis::Count)) return 180.0;
    std::string path = AxisCfgPath(axis);
    if (path.empty()) return 180.0;
    return ConfigManager::instance().getValue<double>(path + ".limitMax", 180.0);
}

bool AxisConfigService::IsWithinSoftLimits(LogicalAxis axis, double pos) const
{
    double lo = GetLimitMin(axis);
    double hi = GetLimitMax(axis);
    if (lo >= hi) return true; // 配置错误（min>=max）：视为不限制
    return pos >= lo - 1e-6 && pos <= hi + 1e-6;
}
