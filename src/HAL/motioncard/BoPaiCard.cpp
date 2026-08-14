#include "BoPaiCard.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <cstring>
#include <thread>
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
constexpr unsigned long kStatusHomeSwitch   = AXIS_STATUS_HOME_SWITCH;   // 0x00004000
constexpr unsigned long kStatusHomeFail     = AXIS_STATUS_HOME_FAIL;     // 0x00400000
constexpr unsigned long kStatusPosHardLimit = AXIS_STATUS_POS_HARD_LIMIT;// 0x00000020
constexpr unsigned long kStatusNegHardLimit = AXIS_STATUS_NEG_HARD_LIMIT;// 0x00000040
constexpr unsigned long kStatusEstop        = AXIS_STATUS_ESTOP;         // 0x00000001
constexpr unsigned long kStatusPosSoftLimit = AXIS_STATUS_POS_SOFT_LIMIT;// 0x00000004
constexpr unsigned long kStatusNegSoftLimit = AXIS_STATUS_NEG_SOFT_LIMIT;// 0x00000008
constexpr unsigned long kStatusFollowErr    = AXIS_STATUS_FOLLOW_ERR;    // 0x00000010
constexpr unsigned long kStatusArrive       = AXIS_STATUS_ARRIVE;        // 0x00000800

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
    double denom;
    if (c.axisType == 1) {
        double mmPerRev = c.lead * c.gearRatio;
        denom = (mmPerRev > 0) ? mmPerRev : 1.0;
    } else {
        // 旋转轴：减速比补偿（gearRatio = 输出端转数/电机转数，1:100 谐波 → 0.01）。
        // 必须与 AxisConverter::PhysicalPerPulse 保持一致（steps /= gearRatio），
        // 否则 PulsePerUnit 少算 1/gearRatio 倍 → AccelToPulse 加速度低估 → 点动/Go 几乎不动。
        double gear = (c.gearRatio > 0) ? c.gearRatio : 1.0;
        steps /= gear;
        denom = 360.0;
    }
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

double BoPaiCard::AccelToPulse(int axisId, double mmOrDegPerSec2) const
{
    // 物理加速度 (°/s² 或 mm/s²) -> 卡端加速度单位 Pulse/ms²
    // 每秒速度变化(mm或°/s) × 每物理单位脉冲数 = 脉冲/s²；再 /1e6 得脉冲/ms²
    if (mmOrDegPerSec2 <= 0.0) return 0.0;
    return mmOrDegPerSec2 * PulsePerUnit(axisId) / 1000000.0;
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
        s.statusWord    = st;   // 保留原始 32 位状态字（诊断）
        s.alarm         = (st & kStatusAlarm) != 0;
        s.enabled       = (st & kStatusEnable) != 0;
        s.running       = (st & kStatusRunning) != 0;
        s.homeDone      = (st & kStatusHomeSuccess) != 0;
        s.limitPositive = (st & kStatusPosHardLimit) != 0;
        s.limitNegative = (st & kStatusNegHardLimit) != 0;
        s.homeSwitch    = (st & kStatusHomeSwitch) != 0;
        s.homeFail      = (st & kStatusHomeFail) != 0;
        s.estop         = (st & kStatusEstop) != 0;
        s.softLimitPositive = (st & kStatusPosSoftLimit) != 0;
        s.softLimitNegative = (st & kStatusNegSoftLimit) != 0;
        s.followError   = (st & kStatusFollowErr) != 0;
        s.arrive        = (st & kStatusArrive) != 0;

        // 状态回读：直接回读卡内位置寄存器（脉冲），与 IMotionCard「脉冲单位」契约一致；
        // 物理换算统一由 HardwareManager/AxisConverter 完成。
        s.position = static_cast<double>(impl_->sysStatus.lAxisPrfPos[axisId]);
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
        int homeDir = Cfg(axisId)->homeDir;   // config: axes.<key>.homeDir, 1=正方向 0=反方向
        int homeSns = Cfg(axisId)->homeSns;   // config: axes.<key>.homeSns, -1=不改 0/1=有效电平
        double homeRapidVel = Cfg(axisId)->homeRapidVel;   // Pulse/ms
        double homeLocatVel = Cfg(axisId)->homeLocatVel;   // Pulse/ms
        long   homeBackDis  = Cfg(axisId)->homeBackDis;    // Pulse，碰信号后反向退出距离
        long   homeMaxDis   = Cfg(axisId)->homeMaxDis;     // Pulse，最大搜索距离（0=不限制）
        TAxisHomePrm prm{};
        prm.nHomeMode = 1;
        prm.nHomeDir = homeDir;
        prm.lOffset = 0;
        prm.dHomeRapidVel = (homeRapidVel > 0) ? homeRapidVel : 5.0;
        prm.dHomeLocatVel = (homeLocatVel > 0) ? homeLocatVel : 1.0;
        prm.dHomeIndexVel = 1.0;
        prm.dHomeAcc = 1.0;
        prm.ulHomeIndexDis = 0;
        prm.ulHomeBackDis = (homeBackDis > 0) ? homeBackDis : 0;
        prm.nDelayTimeBeforeZero = 1000;
        prm.ulHomeMaxDis = static_cast<unsigned long>(homeMaxDis);
        short ch = static_cast<short>(axisId + 1);
        int ecSns = 0;
        if (homeSns >= 0) {
            // MC_HomeSns 参数是全局位掩码（每 bit 对应一轴），homeSns=1→置位(高有效)、0→清零(低有效)
            unsigned long curSense = 0;
            impl_->card.MC_GetHomeSns(&curSense);
            if (homeSns != 0)
                curSense |= (1UL << axisId);
            else
                curSense &= ~(1UL << axisId);
            ecSns = impl_->card.MC_HomeSns(curSense);
            SPDLOG_INFO("[BoPaiCard] Home axis {} set HOME sense={} senseReg=0x{:x} ret={}",
                        axisId, homeSns, curSense, ecSns);
        }
        // 前置清理：HomeStart 需要轴就绪（使能 + 静止），未使能先重新使能
        RefreshStatus();
        MotorStatus st0 = lastStatus_.count(axisId) ? lastStatus_[axisId] : MotorStatus{};
        SPDLOG_INFO("[BoPaiCard] Home axis {} pre-start: pos={:.0f}p running={} homeDone={} homeSwitch={} homeFail={} enabled={}",
                    axisId, st0.position, st0.running, st0.homeDone, st0.homeSwitch, st0.homeFail, st0.enabled);
        if (lastStatus_.count(axisId) && !lastStatus_[axisId].enabled) {
            int ecOn = impl_->card.MC_AxisOn(ch);
            SPDLOG_INFO("[BoPaiCard] Home axis {} re-enable before home ret={}", axisId, ecOn);
        }
        impl_->card.MC_Stop(0x0001 << axisId, 0);
        int ecStop0 = impl_->card.MC_HomeStop(ch);
        int ec0 = impl_->card.MC_HomeSetPrm(ch, &prm);

        // 启动回零：HomeStop 刚发出后 HomeStart 可能被卡以"轴忙/未就绪"拒绝（返回 1），
        // 短暂间隔重试最多 3 次
        int ec2 = -1;
        for (int attempt = 0; attempt < 3; ++attempt) {
            ec2 = impl_->card.MC_HomeStart(ch);
            if (ec2 == 0) break;
            SPDLOG_WARN("[BoPaiCard] Home axis {} start attempt {} failed: {}", axisId, attempt + 1, ec2);
            impl_->card.MC_HomeStop(ch);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        SPDLOG_INFO("[BoPaiCard] Home axis {} started mode={} dir={} offset={} rapid={} locat={} "
                    "backDis={} maxDis={} | sns={} set={} stop={} start={}",
                    axisId, prm.nHomeMode, prm.nHomeDir, prm.lOffset,
                    prm.dHomeRapidVel, prm.dHomeLocatVel, prm.ulHomeBackDis, prm.ulHomeMaxDis,
                    ecSns, ec0, ecStop0, ec2);
        if (ec0 != 0 || ec2 != 0) {
            SPDLOG_WARN("[BoPaiCard] Home axis {} failed: set={} start={}", axisId, ec0, ec2);
            return false;
        }
        // 启动后验证（持锁内直接读 lastStatus_，避免 GetAxisStatus 二次加锁死锁）：
        // running=false + homeDone=false 说明回零立即结束且未成功：常见于 HOME 信号当前已有效
        // （homeSwitch=true → 卡认为已在原点），或回零方向/极性配置问题
        RefreshStatus();
        MotorStatus st = lastStatus_.count(axisId) ? lastStatus_[axisId] : MotorStatus{};
        SPDLOG_INFO("[BoPaiCard] Home axis {} post-start: running={} homeDone={} homeSwitch={} homeFail={} alarm={} enabled={}",
                    axisId, st.running, st.homeDone, st.homeSwitch, st.homeFail, st.alarm, st.enabled);
        if (!st.running && !st.homeDone)
            SPDLOG_WARN("[BoPaiCard] Home axis {} finished immediately WITHOUT success: homeSwitch={} homeFail={} "
                        "(HOME 信号当前有效? 检查机械挡块/接线极性, 或手动点动离开信号区后重试)",
                        axisId, st.homeSwitch, st.homeFail);
    }
    return true;
}

bool BoPaiCard::StopAxis(int axisId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (impl_->useHardware) {
        // 先终止可能进行中的回零，再停止运动：仅 MC_Stop 可能无法退出卡端 HOME 状态机，
        // 导致后续 Jog/Trap 被拒绝
        impl_->card.MC_HomeStop(static_cast<short>(axisId + 1));
        impl_->card.MC_Stop(0x0001 << axisId, 0);
    }
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
    if (impl_->useHardware) {
        // 先退出所有轴的 HOME 状态机，再全轴停止：仅 MC_Stop 可能无法退出卡端 HOME 状态机，
        // 导致急停后该轴 Jog/Trap（点动/Go）被卡拒绝（与 StopAxis 同语义）
        for (const auto& [axisId, cfg] : configs_)
            impl_->card.MC_HomeStop(static_cast<short>(axisId + 1));
        impl_->card.MC_Stop(0xFFFFFFFF, 0xFFFFFFFF);
    }
    SPDLOG_WARN("[BoPaiCard] EMERGENCY STOP");
    return true;
}

bool BoPaiCard::MoveAbs(int axisId, double position, double speed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!impl_->useHardware) return false;

    const AxisConfig* cfg = Cfg(axisId);
    // position 已是脉冲（IMotionCard 契约，HardwareManager 已换算），不再 ×ppu
    long posPulse = static_cast<long>(position);

    TTrapPrm trapPrm{};
    trapPrm.acc = (cfg && cfg->maxAccel > 0) ? AccelToPulse(axisId, cfg->maxAccel) : 200.0;
    trapPrm.dec = (cfg && cfg->maxDecel > 0) ? AccelToPulse(axisId, cfg->maxDecel) : trapPrm.acc;
    trapPrm.velStart = 0;
    trapPrm.smoothTime = 0;

    double vel = (speed > 0) ? speed : (cfg ? cfg->maxSpeed * PulsePerUnit(axisId) : 100.0);
    vel = vel / 1000.0; // Pulse/ms

    impl_->card.MC_PrfTrap(static_cast<short>(axisId + 1));
    impl_->card.MC_SetTrapPrm(static_cast<short>(axisId + 1), &trapPrm);
    impl_->card.MC_SetPos(static_cast<short>(axisId + 1), posPulse);
    impl_->card.MC_SetVel(static_cast<short>(axisId + 1), vel);
    impl_->card.MC_Update(0x0001 << axisId);

    SPDLOG_INFO("[BoPaiCard] Axis {} MoveAbs: {} pulses, vel={:.2f} p/ms, acc={:.6f} p/ms2", axisId, posPulse, vel, trapPrm.acc);
    RefreshStatus();
    return true;
}

bool BoPaiCard::MoveRel(int axisId, double distance, double speed)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!impl_->useHardware) return false;

    const AxisConfig* cfg = Cfg(axisId);
    // distance 已是脉冲增量（IMotionCard 契约），不再 ×ppu
    long distPulse = static_cast<long>(distance);
    long curPos = impl_->sysStatus.lAxisPrfPos[axisId];
    long targetPulse = curPos + distPulse;

    TTrapPrm trapPrm{};
    trapPrm.acc = (cfg && cfg->maxAccel > 0) ? AccelToPulse(axisId, cfg->maxAccel) : 200.0;
    trapPrm.dec = (cfg && cfg->maxDecel > 0) ? AccelToPulse(axisId, cfg->maxDecel) : trapPrm.acc;
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
    jogPrm.dAcc = (cfg && cfg->maxAccel > 0) ? AccelToPulse(axisId, cfg->maxAccel) : 200.0;
    jogPrm.dDec = (cfg && cfg->maxDecel > 0) ? AccelToPulse(axisId, cfg->maxDecel) : jogPrm.dAcc;
    jogPrm.dSmooth = 100;

    short ch = static_cast<short>(axisId + 1);
    int e0 = impl_->card.MC_PrfJog(ch);
    int e1 = impl_->card.MC_SetJogPrm(ch, &jogPrm);
    int e2 = impl_->card.MC_SetVel(ch, velDir);
    int e3 = impl_->card.MC_Update(0x0001 << axisId);
    SPDLOG_INFO("[BoPaiCard] Axis {} MoveJog: vel={:.2f} p/ms (prf={} prm={} vel={} upd={})",
                axisId, velDir, e0, e1, e2, e3);
    if (e0 != 0 || e1 != 0 || e2 != 0 || e3 != 0) {
        SPDLOG_WARN("[BoPaiCard] Axis {} MoveJog rejected by card: prf={} prm={} vel={} upd={}",
                    axisId, e0, e1, e2, e3);
        return false;
    }
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
