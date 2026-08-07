#include "BoPaiCard.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <vector>

// BoPai SDK 头文件 — 注意：该头文件 GBK 编码 + #pragma pack(2)，
// 仅在 cpp 内包含，避免影响公共头文件的对齐
#include "MultiCardCPP.h"

// 在全局作用域自动注册到工厂
REGISTER_MOTION_CARD("Bopai", BoPaiCard)

namespace {

// 状态位定义（与 MultiCardCPP.h 一致）
constexpr unsigned long kStatusAlarm        = AXIS_STATUS_SV_ALARM;      // 0x00000002
constexpr unsigned long kStatusEnable       = AXIS_STATUS_ENABLE;        // 0x00000200
constexpr unsigned long kStatusRunning      = AXIS_STATUS_RUNNING;       // 0x00000400
constexpr unsigned long kStatusHomeSuccess  = AXIS_STATUS_HOME_SUCESS;   // 0x00002000
constexpr unsigned long kStatusPosHardLimit = AXIS_STATUS_POS_HARD_LIMIT;// 0x00000020
constexpr unsigned long kStatusNegHardLimit = AXIS_STATUS_NEG_HARD_LIMIT;// 0x00000040

} // namespace

// ============================================================
// PIMPL — 持有 MultiCard 实例与状态快照
// ============================================================
class BoPaiCard::Impl
{
public:
    MultiCard card;
    TAllSysStatusDataSX sysStatus{};
    bool useHardware = false;
    bool connected = false;
    short cardNum = 1;
    std::string lastError;
    std::string pcIp = "192.168.0.200";
    int pcPort = 60000;
};

// ============================================================
BoPaiCard::BoPaiCard()
    : impl_(std::make_unique<Impl>())
{
}

BoPaiCard::~BoPaiCard()
{
    Disconnect();
}

bool BoPaiCard::Connect(const std::string& ip, int port)
{
    std::lock_guard<std::mutex> lock(mutex_);

    char pcIp[64] = { 0 };
    char cardIp[64] = { 0 };
    strncpy_s(pcIp, impl_->pcIp.c_str(), sizeof(pcIp) - 1);
    strncpy_s(cardIp, ip.c_str(), sizeof(cardIp) - 1);
    unsigned short p = static_cast<unsigned short>(port > 0 ? port : 60000);
    unsigned short pcP = static_cast<unsigned short>(impl_->pcPort > 0 ? impl_->pcPort : 60000);

    int ret = impl_->card.MC_Open(impl_->cardNum, pcIp, pcP, cardIp, p);
    if (ret != 0) {
        impl_->lastError = "MC_Open failed, code=" + std::to_string(ret);
        impl_->useHardware = false;
        impl_->connected = false;
        SPDLOG_WARN("[BoPaiCard] MC_Open failed: {} (fallback disabled)", ret);
        return false;
    }

    impl_->card.MC_Reset();
    impl_->useHardware = true;
    impl_->connected = true;
    SPDLOG_INFO("[BoPaiCard] Connected to Bopai card at {}:{} (pc {})", ip, port, impl_->pcIp);
    RefreshStatus();
    return true;
}

bool BoPaiCard::SetHost(const std::string& pcIp, int pcPort)
{
    impl_->pcIp = pcIp;
    impl_->pcPort = (pcPort > 0) ? pcPort : 60000;
    SPDLOG_INFO("[BoPaiCard] Host IP set to {}:{}", impl_->pcIp, impl_->pcPort);
    return true;
}

void BoPaiCard::Disconnect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (impl_->useHardware) {
        impl_->card.MC_Stop(0xFFFFFFFF, 0xFFFFFFFF);
        impl_->card.MC_Close();
        impl_->useHardware = false;
        impl_->connected = false;
        SPDLOG_INFO("[BoPaiCard] Disconnected");
    }
}

bool BoPaiCard::IsConnected() const
{
    return impl_->connected && impl_->useHardware;
}

// ============================================================
// 每脉冲对应的物理量 — 由 SetAxisConfig 喂入的参数计算
// ============================================================
double BoPaiCard::PulsePerUnit(int axisId) const
{
    auto it = configs_.find(axisId);
    if (it == configs_.end())
        return 1.0;
    const AxisConfig& c = it->second;
    double pulsesPerRev = (c.pulsesPerRev > 0) ? c.pulsesPerRev : 1.0;
    double steps = pulsesPerRev * (c.microSteps > 1 ? c.microSteps : 1);
    double denom = (c.hardwareType == 0) ? 360.0 : (c.lead > 0 ? c.lead : 1.0);
    return steps / denom; // 脉冲/物理单位
}

AxisConfig* BoPaiCard::Cfg(int axisId)
{
    auto it = configs_.find(axisId);
    return (it != configs_.end()) ? &it->second : nullptr;
}

const AxisConfig* BoPaiCard::Cfg(int axisId) const
{
    auto it = configs_.find(axisId);
    return (it != configs_.end()) ? &it->second : nullptr;
}

// ============================================================
// 状态回读 — MC_GetAllSysStatusSX
// ============================================================
void BoPaiCard::RefreshStatus()
{
    if (!impl_->useHardware) return;

    impl_->card.MC_GetAllSysStatusSX(&impl_->sysStatus);

    for (auto& [axisId, cfg] : configs_)
    {
        unsigned long st = impl_->sysStatus.lAxisStatus[axisId];
        MotorStatus s;
        s.axisId        = axisId;
        s.enabled       = (st & kStatusEnable) != 0;
        s.alarm         = (st & kStatusAlarm) != 0;
        s.running       = (st & kStatusRunning) != 0;
        s.homeDone      = (st & kStatusHomeSuccess) != 0;
        s.limitPositive = (st & kStatusPosHardLimit) != 0;
        s.limitNegative = (st & kStatusNegHardLimit) != 0;

        double ppu = PulsePerUnit(axisId);
        s.position = (ppu > 0) ? impl_->sysStatus.lAxisPrfPos[axisId] / ppu : 0.0;
        s.velocity = 0.0;
        s.current  = 0.0;
        lastStatus_[axisId] = s;
    }
}

// ============================================================
bool BoPaiCard::EnableAxis(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (impl_->useHardware) {
        impl_->card.MC_ClrSts(static_cast<short>(axisId + 1));
        int ret = impl_->card.MC_AxisOn(static_cast<short>(axisId + 1));
        if (ret != 0) {
            impl_->lastError = "MC_AxisOn failed, code=" + std::to_string(ret);
            SPDLOG_WARN("[BoPaiCard] MC_AxisOn({}) failed: {}", axisId, ret);
            return false;
        }
    }
    RefreshStatus();
    return true;
}

bool BoPaiCard::DisableAxis(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (impl_->useHardware)
        impl_->card.MC_AxisOff(static_cast<short>(axisId + 1));
    RefreshStatus();
    return true;
}

bool BoPaiCard::HomeAxis(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (impl_->useHardware) {
        TAxisHomePrm prm{};
        prm.nHomeMode = 1;
        prm.nHomeDir = 1;
        prm.lOffset = 0;
        prm.dHomeRapidVel = 5.0;
        prm.dHomeLocatVel = 1.0;
        prm.dHomeIndexVel = 1.0;
        prm.dHomeAcc = 1.0;
        prm.ulHomeIndexDis = 0;
        prm.ulHomeBackDis = 0;
        prm.nDelayTimeBeforeZero = 1000;
        prm.ulHomeMaxDis = 0;
        impl_->card.MC_HomeSetPrm(static_cast<short>(axisId + 1), &prm);
        impl_->card.MC_HomeStop(static_cast<short>(axisId + 1));
        impl_->card.MC_HomeStart(static_cast<short>(axisId + 1));
        SPDLOG_INFO("[BoPaiCard] Home axis {} started", axisId);
    }
    return true;
}

bool BoPaiCard::StopAxis(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (impl_->useHardware)
        impl_->card.MC_Stop(0x0001 << axisId, 0);
    RefreshStatus();
    return true;
}

bool BoPaiCard::StopAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (impl_->useHardware)
        impl_->card.MC_Stop(0xFFFFFFFF, 0xFFFFFFFF);
    return true;
}

bool BoPaiCard::EmergencyStop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (impl_->useHardware)
        impl_->card.MC_Stop(0xFFFFFFFF, 0xFFFFFFFF);
    SPDLOG_WARN("[BoPaiCard] EMERGENCY STOP");
    return true;
}

bool BoPaiCard::MoveAbs(int axisId, double position, double speed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!impl_->useHardware) return false;

    const AxisConfig* cfg = Cfg(axisId);
    double ppu = PulsePerUnit(axisId);
    long posPulse = static_cast<long>(position * ppu);

    TTrapPrm trapPrm{};
    trapPrm.acc = (cfg && cfg->maxAccel > 0) ? cfg->maxAccel : 200.0;
    trapPrm.dec = (cfg && cfg->maxDecel > 0) ? cfg->maxDecel : trapPrm.acc;
    trapPrm.velStart = 0;
    trapPrm.smoothTime = 0;

    double vel = (speed > 0) ? speed : (cfg ? cfg->maxSpeed * PulsePerUnit(axisId) : 100.0);
    vel = vel / 1000.0; // Pulse/ms

    impl_->card.MC_PrfTrap(static_cast<short>(axisId + 1));
    impl_->card.MC_SetTrapPrm(static_cast<short>(axisId + 1), &trapPrm);
    impl_->card.MC_SetPos(static_cast<short>(axisId + 1), posPulse);
    impl_->card.MC_SetVel(static_cast<short>(axisId + 1), vel);
    impl_->card.MC_Update(0x0001 << axisId);

    SPDLOG_INFO("[BoPaiCard] Axis {} MoveAbs: {:.1f} pulses, vel={:.2f} p/ms", axisId, position, vel);
    RefreshStatus();
    return true;
}

bool BoPaiCard::MoveRel(int axisId, double distance, double speed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!impl_->useHardware) return false;

    const AxisConfig* cfg = Cfg(axisId);
    double ppu = PulsePerUnit(axisId);
    long distPulse = static_cast<long>(distance * ppu);
    long curPos = impl_->sysStatus.lAxisPrfPos[axisId];
    long targetPulse = curPos + distPulse;

    TTrapPrm trapPrm{};
    trapPrm.acc = (cfg && cfg->maxAccel > 0) ? cfg->maxAccel : 200.0;
    trapPrm.dec = (cfg && cfg->maxDecel > 0) ? cfg->maxDecel : trapPrm.acc;
    trapPrm.velStart = 0;
    trapPrm.smoothTime = 0;

    double vel = (speed > 0) ? speed : (cfg ? cfg->maxSpeed * PulsePerUnit(axisId) : 100.0);
    vel = vel / 1000.0;

    impl_->card.MC_PrfTrap(static_cast<short>(axisId + 1));
    impl_->card.MC_SetTrapPrm(static_cast<short>(axisId + 1), &trapPrm);
    impl_->card.MC_SetPos(static_cast<short>(axisId + 1), targetPulse);
    impl_->card.MC_SetVel(static_cast<short>(axisId + 1), vel);
    impl_->card.MC_Update(0x0001 << axisId);
    return true;
}

bool BoPaiCard::MoveLinear(const std::vector<double>& targetPositions, double speed)
{
    bool ok = true;
    for (size_t i = 0; i < targetPositions.size(); ++i)
        ok = MoveAbs(static_cast<int>(i), targetPositions[i], speed) && ok;
    return ok;
}

bool BoPaiCard::MoveJog(int axisId, double speedPulsesPerSec, double accel, int direction)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!impl_->useHardware) return false;
    (void)accel;

    const AxisConfig* cfg = Cfg(axisId);
    double vel = (speedPulsesPerSec > 0) ? speedPulsesPerSec : (cfg ? cfg->maxSpeed * PulsePerUnit(axisId) : 1000.0);
    double velDir = (direction >= 0) ? vel : -vel;
    velDir = velDir / 1000.0; // Pulse/ms

    TJogPrm jogPrm{};
    jogPrm.dAcc = (cfg && cfg->maxAccel > 0) ? cfg->maxAccel : 200.0;
    jogPrm.dDec = (cfg && cfg->maxDecel > 0) ? cfg->maxDecel : jogPrm.dAcc;
    jogPrm.dSmooth = 100;

    impl_->card.MC_PrfJog(static_cast<short>(axisId + 1));
    impl_->card.MC_SetJogPrm(static_cast<short>(axisId + 1), &jogPrm);
    impl_->card.MC_SetVel(static_cast<short>(axisId + 1), velDir);
    impl_->card.MC_Update(0x0001 << axisId);

    SPDLOG_INFO("[BoPaiCard] Axis {} MoveJog: vel={:.2f} p/ms", axisId, velDir);
    return true;
}

bool BoPaiCard::StopJog(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (impl_->useHardware)
        impl_->card.MC_Stop(0x0001 << axisId, 0);
    return true;
}

bool BoPaiCard::SetSpeed(int axisId, double speed)
{
    if (auto* cfg = Cfg(axisId)) {
        cfg->maxSpeed = speed;
        return true;
    }
    return false;
}

bool BoPaiCard::SetAccel(int axisId, double accel, double decel)
{
    if (auto* cfg = Cfg(axisId)) {
        cfg->maxAccel = accel;
        cfg->maxDecel = (decel > 0) ? decel : accel;
        return true;
    }
    return false;
}

bool BoPaiCard::SetAxisConfig(int axisId, const AxisConfig& cfg)
{
    configs_[axisId] = cfg;
    return true;
}

double BoPaiCard::GetPosition(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lastStatus_.find(axisId);
    return (it != lastStatus_.end()) ? it->second.position : 0.0;
}

MotorStatus BoPaiCard::GetAxisStatus(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    RefreshStatus();
    auto it = lastStatus_.find(axisId);
    return (it != lastStatus_.end()) ? it->second : MotorStatus{};
}

std::vector<MotorStatus> BoPaiCard::GetAllStatus()
{
    std::lock_guard<std::mutex> lock(mutex_);
    RefreshStatus();
    std::vector<MotorStatus> out;
    for (const auto& [id, s] : lastStatus_)
        out.push_back(s);
    return out;
}

bool BoPaiCard::SetDO(int channel, bool state)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (impl_->useHardware)
        impl_->card.MC_SetDoBit(1, static_cast<short>(channel), state ? 1 : 0);
    return true;
}

bool BoPaiCard::GetDI(int channel)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!impl_->useHardware) return false;
    long value = 0;
    impl_->card.MC_GetDi(1, &value);
    return (value & (1L << channel)) != 0;
}

std::string BoPaiCard::GetLastError() const
{
    return impl_->lastError;
}
