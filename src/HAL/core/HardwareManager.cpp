#include "HardwareManager.h"

#include <QDateTime>

#include "HALFactory.h"
#include "AxisConverter.h"
#include "AxisConfigService.h"
#include "ConfigManager.h"
#include "CameraManager.h"
#include "CameraCaptureWorker.h"
#include "SimCard.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QThread>

#include <spdlog/spdlog.h>
#include <algorithm>
#include <vector>

namespace {

// config 中每个逻辑轴的 key 名（与 config/axes 对象 key 对应）
const char* kAxisConfigKeys[] = {
    "Axis_J1", "Axis_J2", "Axis_Z", "Axis_R", "Axis_Gripper", "Axis_Extruder"
};

// 确保 ConfigManager 已加载 config.json（与 ConfigPage 相同候选路径）
void EnsureConfigLoaded()
{
    auto& cfg = ConfigManager::instance();
    if (!cfg.filePath().isEmpty() && QFileInfo::exists(cfg.filePath()))
        return;

    QStringList candidates = {
#ifdef PROJECT_SOURCE_DIR
        QString::fromUtf8(PROJECT_SOURCE_DIR) + QStringLiteral("/config/config.json"),
#endif
        QCoreApplication::applicationDirPath() + QStringLiteral("/config.json"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../config/config.json"),
    };
    for (const auto& path : candidates) {
        if (QFileInfo::exists(path)) {
            cfg.load(path);
            return;
        }
    }
    SPDLOG_INFO("[HardwareManager] No config.json found, using defaults");
}

// 轴异常状态的可读描述（用于日志落盘；UI 侧各自组装 tooltip）
QString DescribeAxisStatus(const MotorStatus& s)
{
    QString parts;
    auto add = [&parts](const QString& t) {
        if (!parts.isEmpty()) parts += QStringLiteral("、");
        parts += t;
    };
    if (s.alarm)             add(QStringLiteral("驱动器报警"));
    if (s.followError)       add(QStringLiteral("跟随误差(失步)"));
    if (s.estop)             add(QStringLiteral("急停"));
    if (s.limitPositive)     add(QStringLiteral("正硬限位"));
    if (s.limitNegative)     add(QStringLiteral("负硬限位"));
    if (s.softLimitPositive) add(QStringLiteral("正软限位"));
    if (s.softLimitNegative) add(QStringLiteral("负软限位"));
    if (s.homeFail)          add(QStringLiteral("回零失败"));
    return parts.isEmpty() ? QStringLiteral("无") : parts;
}

} // namespace

HardwareManager& HardwareManager::instance()
{
    static HardwareManager inst;
    return inst;
}

HardwareManager::HardwareManager()
{
    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(50);
    connect(pollTimer_, &QTimer::timeout, this, &HardwareManager::PollTick);

    jogTimer_ = new QTimer(this);
    jogTimer_->setInterval(50);
    connect(jogTimer_, &QTimer::timeout, this, &HardwareManager::JogTick);

    cameraManager_ = std::make_unique<CameraManager>();
    connect(cameraManager_.get(), &CameraManager::frameReady, this, &HardwareManager::frameReady);
}

HardwareManager::~HardwareManager()
{
    StopCameraStream();
    cameraManager_.reset();   // 停采集线程并释放 worker
    pollTimer_->stop();
    if (motionCard_) motionCard_->Disconnect();
    if (servoJ2_) servoJ2_->Disconnect();
    if (servoJ3_) servoJ3_->Disconnect();
    if (gripper_) gripper_->Disconnect();
    if (camera_) camera_->Close();
}

bool HardwareManager::Initialize()
{
    if (initialized_) return true;

    SPDLOG_INFO("[HardwareManager] Initialize BEGIN");

    EnsureConfigLoaded();
    auto& cfg = ConfigManager::instance();

    // ---- 1. 创建运动控制卡 ----
    std::string cardType = cfg.getValue<std::string>("simulation.motionCardType", "SimCard");
    motionCard_ = MotionCardFactory::Instance().Create(cardType);
    if (motionCard_) {
        std::string ip = cfg.getValue<std::string>("communication.motionCard.ip", "192.168.0.1");
        // ConfigPage 将该端口绑定为文本(QLineEdit)，故按字符串读取再转换
        std::string portStr = cfg.getValue<std::string>("communication.motionCard.port", "60000");
        int port = 60000;
        try { port = std::stoi(portStr); } catch (...) {}
        // 网口卡需先注入本地(PC) IP，再建立连接
        std::string pcIp = cfg.getValue<std::string>("communication.motionCard.pcIp", "192.168.0.200");
        motionCard_->SetHost(pcIp, port);
        bool ok = motionCard_->Connect(ip, port);
        if (ok) {
            SPDLOG_INFO("[HardwareManager] MotionCard '{}' connected", cardType);
        } else {
            SPDLOG_WARN("[HardwareManager] MotionCard '{}' connect FAILED: {}",
                        cardType, motionCard_->GetLastError());
        }
    } else {
        SPDLOG_INFO("[HardwareManager] MotionCard type '{}' not registered", cardType);
    }

    // ---- 2. 创建舵机 (J2 / R) ----
    std::string servoType = cfg.getValue<std::string>("simulation.servoType", "SimServo");
    servoJ2_ = AxisServoFactory::Instance().Create(servoType);
    servoJ3_ = AxisServoFactory::Instance().Create(servoType);
    if (servoJ2_ || servoJ3_) {
        std::string port = cfg.getValue<std::string>("communication.servo.port", "COM3");
        // ConfigPage 将波特率绑定为文本(QComboBox)，故按字符串读取再转换
        std::string baudStr = cfg.getValue<std::string>("communication.servo.baudRate", "115200");
        int baud = 115200;
        try { baud = std::stoi(baudStr); } catch (...) {}
        // 舵机总线 ID 直接取电控与映射页的 portId（ConfigPage 改 portId 即生效，
        // 重启程序后重新 Connect 应用）。速度/限位仍从 communication.servos[] 读取。
        int idJ2 = cfg.getValue<int>("axes.Axis_J2.portId", 0);
        int idR  = cfg.getValue<int>("axes.Axis_R.portId", 1);
        double spdJ2 = cfg.getValue<double>("communication.servos[0].speed", 50.0);
        double spdR  = cfg.getValue<double>("communication.servos[1].speed", 50.0);
        if (servoJ2_) {
            bool ok = servoJ2_->Connect(port, baud);
            if (ok) {
                servoJ2_->SetServoId(static_cast<uint8_t>(idJ2));
                servoJ2_->SetSpeed(spdJ2);
            } else {
                SPDLOG_WARN("[HardwareManager] Servo J2 connect FAILED: {}",
                            servoJ2_->GetLastError());
            }
        }
        if (servoJ3_) {
            bool ok = servoJ3_->Connect(port, baud);
            if (ok) {
                servoJ3_->SetServoId(static_cast<uint8_t>(idR));
                servoJ3_->SetSpeed(spdR);
            } else {
                SPDLOG_WARN("[HardwareManager] Servo R connect FAILED: {}",
                            servoJ3_->GetLastError());
            }
        }
        SPDLOG_INFO("[HardwareManager] Servo '{}' created (J2 id={}, R id={})", servoType, idJ2, idR);
    } else {
        SPDLOG_INFO("[HardwareManager] Servo type '{}' not registered", servoType);
    }

    // ---- 3. 创建末端/相机/算法（可空，延后接入） ----
    std::string algoType = cfg.getValue<std::string>("simulation.algorithmType", "SimAlgo");
    algorithm_ = PuffAlgorithmFactory::Instance().Create(algoType);
    if (!algorithm_) {
        SPDLOG_WARN("[HardwareManager] Algorithm type '{}' not registered", algoType);
    } else {
        std::string jsonCfg = "{\"confidenceThreshold\":" +
            std::to_string(cfg.getValue<double>("vision.confidenceThreshold", 0.85)) + "}";
        algorithm_->LoadConfig(jsonCfg);
    }

    std::string camType = cfg.getValue<std::string>("simulation.cameraType", "SimCamera");
    camera_ = CameraFactory::Instance().Create(camType);
    if (!camera_) {
        SPDLOG_WARN("[HardwareManager] Camera type '{}' not registered", camType);
    } else {
        cameraManager_->SetCamera(camera_.get());
        int    w   = cfg.getValue<int>("vision.frameWidth", 640);
        int    h   = cfg.getValue<int>("vision.frameHeight", 480);
        double fps = cfg.getValue<double>("vision.frameFps", 30.0);
        CameraOpen(w, h, fps);
    }

    // ---- 4. 每轴换算参数喂给卡与 AxisConverter ----
    LoadAxisConfigsFromConfig();
    axisCfgSvc_ = std::make_unique<AxisConfigService>(axisConfigs_);

    // ---- 5. 启动 50ms 状态轮询 ----
    lastAlarm_.fill(false, static_cast<int>(LogicalAxis::Count));
    lastLimitPos_.fill(false, static_cast<int>(LogicalAxis::Count));
    lastLimitNeg_.fill(false, static_cast<int>(LogicalAxis::Count));
    lastSoftLimitHit_.fill(false, static_cast<int>(LogicalAxis::Count));
    lastPollPos_.fill(0.0, static_cast<int>(LogicalAxis::Count));
    lastAbnormalSig_.fill(0UL, static_cast<int>(LogicalAxis::Count));
    // 使能门禁：初始全轴未使能。使能必须手动触发（EnableAll），程序不自动使能。
    axisEnabled_.fill(false, static_cast<int>(LogicalAxis::Count));
    axisBusyUntilMs_.fill(0, static_cast<int>(LogicalAxis::Count));
    homeStartedMs_.fill(0, static_cast<int>(LogicalAxis::Count));
    axisBusyNotified_.fill(false, static_cast<int>(LogicalAxis::Count));
    homingActive_.fill(false, static_cast<int>(LogicalAxis::Count));
    // 用真实位置初始化软限位方向判断基线，避免首轮 delta 以 0 为基准误判（上电位置非 0 时）
    for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i) {
        auto b = AxisMap::Get(static_cast<LogicalAxis>(i));
        if (b.type == AxisBinding::Type::Card && i < lastPollPos_.size())
            lastPollPos_[i] = GetPosition(static_cast<LogicalAxis>(i));
    }
    pollTimer_->start();

    initialized_ = true;
    emit connectionChanged();
    SPDLOG_INFO("[HardwareManager] Initialize complete");
    return true;
}

bool HardwareManager::IsInitialized() const
{
    return initialized_;
}

bool HardwareManager::IsMotionCardConnected() const
{
    return motionCard_ && motionCard_->IsConnected();
}

bool HardwareManager::IsServoConnected() const
{
    return (servoJ2_ && servoJ2_->IsConnected()) || (servoJ3_ && servoJ3_->IsConnected());
}

QString HardwareManager::ConnectionStatus() const
{
    QString cardState = IsMotionCardConnected() ? QStringLiteral("已连接") : QStringLiteral("未连接");
    QString servoState = IsServoConnected() ? QStringLiteral("已连接") : QStringLiteral("未连接");
    return QStringLiteral("运动卡: %1 | 舵机: %2").arg(cardState, servoState);
}

bool HardwareManager::CameraOpen(int width, int height, double fps)
{
    return cameraManager_ && cameraManager_->Open(width, height, fps);
}

void HardwareManager::CameraClose()
{
    if (cameraManager_) cameraManager_->Close();
}

bool HardwareManager::StartCameraStream(int fps)
{
    return cameraManager_ && cameraManager_->StartStream(fps);
}

void HardwareManager::StopCameraStream()
{
    if (cameraManager_) cameraManager_->StopStream();
}

bool HardwareManager::IsCameraStreaming() const
{
    return cameraManager_ && cameraManager_->IsStreaming();
}

void HardwareManager::LoadAxisConfigsFromConfig()
{
    auto& cfg = ConfigManager::instance();
    axisConfigs_.resize(static_cast<int>(LogicalAxis::Count));
    for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i)
    {
        const std::string key = kAxisConfigKeys[i];
        const std::string base = "axes." + key + ".";
        AxisConfig ac;
        ac.axisId       = i;
        ac.maxSpeed     = cfg.getValue<double>(base + "maxSpeed", 150.0);
        ac.maxAccel     = cfg.getValue<double>(base + "maxAccel", 500.0);
        // 智能 maxDecel：JSON 里配了 "maxDecel" 就用，没配则默认等于 Accel
        ac.maxDecel     = cfg.getValue<double>(base + "maxDecel", ac.maxAccel);
        ac.jogSpeed     = cfg.getValue<double>(base + "jogSpeed", ac.maxSpeed);
        ac.homePos      = cfg.getValue<double>(base + "homeOffset", 0.0);
        ac.limitMin     = cfg.getValue<double>(base + "limitMin", -180.0);
        ac.limitMax     = cfg.getValue<double>(base + "limitMax", 180.0);
        if (ac.limitMin >= ac.limitMax)
            SPDLOG_WARN("[HardwareManager] Axis {} soft limits invalid (min {:.1f} >= max {:.1f}), enforcement disabled",
                        key, ac.limitMin, ac.limitMax);
        ac.inverted     = (cfg.getValue<int>(base + "direction", 0) != 0);
        ac.hardwareType = cfg.getValue<int>(base + "hardwareType", 0);
        int portId      = cfg.getValue<int>(base + "portId", -1);
        // 轴类型：rotation(角度)/linear(直线 mm)，决定换算用 360° 还是 导程×减速比
        ac.axisType     = (cfg.getValue<std::string>(base + "axisType", "rotation") == "linear") ? 1 : 0;
        ac.pulsesPerRev = cfg.getValue<int>(base + "transmission.encoderResolution", 32000);
        ac.microSteps   = cfg.getValue<int>(base + "transmission.microSteps", 1);
        ac.lead         = cfg.getValue<double>(base + "transmission.lead", 360.0);
        ac.gearRatio    = cfg.getValue<double>(base + "transmission.gearRatio", 1.0);
        ac.homeDir      = (cfg.getValue<int>(base + "homeDir", 1) != 0) ? 1 : 0;
        ac.homeSns      = cfg.getValue<int>(base + "homeSns", -1);
        ac.homeRapidVel = cfg.getValue<double>(base + "homeRapidVel", 5.0);
        ac.homeLocatVel = cfg.getValue<double>(base + "homeLocatVel", 1.0);
        ac.homeBackDis  = static_cast<long>(cfg.getValue<double>(base + "homeBackDis", 0.0));
        ac.homeMaxDis   = static_cast<long>(cfg.getValue<double>(base + "homeMaxDis", 0.0));
        // 换算公式（参考 bopai\puff\MotionController.cpp）：
        //   rotation: pulsesPerUnit = 每圈脉冲 / 360          (脉冲/度)
        //   linear:   pulsesPerUnit = 每圈脉冲 / (lead×gearRatio) (脉冲/mm)
        const double stepsPerRev = ac.pulsesPerRev * ac.microSteps;
        if (ac.axisType == 1) {
            const double mmPerRev = ac.lead * ac.gearRatio;
            ac.pulsesPerUnit = (mmPerRev > 0) ? stepsPerRev / mmPerRev : 0.0;
        } else {
            ac.pulsesPerUnit = stepsPerRev / 360.0;
        }

        axisConfigs_[i] = ac;

        AxisConverter::Instance().ConfigureAxis(i, ac);

        // 以 config 的 hardwareType/portId 覆盖 AxisMap 绑定：
        //   卡轴 index = BoPai 卡 axis 号，舵机 index = 总线 ID。
        // 缺省 portId 时保留 AxisMap 默认兜底值。
        if (portId >= 0)
            AxisMap::SetBinding(static_cast<LogicalAxis>(i),
                                (ac.hardwareType == 1) ? AxisBinding::Type::Servo : AxisBinding::Type::Card,
                                portId);

        auto binding = AxisMap::Get(static_cast<LogicalAxis>(i));
        if (binding.type == AxisBinding::Type::Card && motionCard_) {
            ac.axisId = binding.index;
            // Home Offset 落地：卡端软限位按机械角度注入（SimCard 脉冲域夹紧基于机械位置）。
            // inverted 轴机械方向反转，min/max 对调；HardwareManager 自身软限位判断
            // 仍用 GetLimitMin/Max 读 config 的逻辑坐标，不受此处影响。
            double off = ac.homePos;
            double mechMin = ac.inverted ? -(ac.limitMax + off) : (ac.limitMin + off);
            double mechMax = ac.inverted ? -(ac.limitMin + off) : (ac.limitMax + off);
            ac.limitMin = mechMin;
            ac.limitMax = mechMax;
            motionCard_->SetAxisConfig(binding.index, ac);
        }
    }
    SPDLOG_INFO("[HardwareManager] Axis configs loaded ({} axes)", static_cast<int>(LogicalAxis::Count));
}

// ============================================================
// 物理单位门面
// ============================================================
bool HardwareManager::MoveAbs(LogicalAxis axis, double mmOrDeg, double speed)
{
    // 使能门禁：未使能禁止运动（安全门禁，先使能再运动）
    if (!IsAxisEnabled(axis)) {
        SPDLOG_WARN("[HardwareManager] MoveAbs rejected: axis {} not enabled", (int)axis);
        return false;
    }
    // 报警门禁：轴报警状态禁止运动（停止/急停除外）
    int ai = static_cast<int>(axis);
    if (ai >= 0 && ai < lastAlarm_.size() && lastAlarm_[ai]) {
        SPDLOG_WARN("[HardwareManager] MoveAbs rejected: axis {} in alarm", (int)axis);
        return false;
    }
    // 回零门禁：回零进行中禁止运动，需先停止回零
    if (ai >= 0 && ai < homingActive_.size() && homingActive_[ai]) {
        SPDLOG_WARN("[HardwareManager] MoveAbs rejected: axis {} homing in progress", ai);
        return false;
    }

    // 方向反转（config direction=反向）+ Home Offset（机械零点→逻辑零点）：
    // 逻辑角度 = 机械角度 - homeOffset，故下发目标机械角度 = 逻辑 + homeOffset；
    // inverted 轴在机械方向坐标上再取反。界面/软限位恒用逻辑坐标。
    bool inv = (ai >= 0 && ai < static_cast<int>(axisConfigs_.size())) && axisConfigs_[ai].inverted;
    double off = (ai >= 0 && ai < static_cast<int>(axisConfigs_.size())) ? axisConfigs_[ai].homePos : 0.0;
    double targetPhys = inv ? -(mmOrDeg + off) : (mmOrDeg + off);

    // 软限位校验（逻辑坐标，与界面显示一致）：目标位置超出 [limitMin, limitMax] 直接拒绝
    if (!IsWithinSoftLimits(axis, mmOrDeg)) {
        SPDLOG_WARN("[HardwareManager] MoveAbs rejected: axis {} target {:.1f} out of soft limits [{:.1f}, {:.1f}]",
                    ai, mmOrDeg, GetLimitMin(axis), GetLimitMax(axis));
        return false;
    }

    auto binding = AxisMap::Get(axis);
    if (binding.type == AxisBinding::Type::Card && motionCard_) {
        long pulse = AxisConverter::Instance().ToPulse(ai, targetPhys);
        // speed>0：物理速度 → 脉冲速度下发（Go 使用界面速度框）；speed<=0：沿用卡 maxSpeed
        double speedPulse = (speed > 0)
            ? AxisConverter::Instance().SpeedToPulse(ai, speed)
            : -1.0;
        // 加速度实时读 config（与「电控与映射」编辑一致），运动前刷新卡内快照
        motionCard_->SetAccel(binding.index, GetMaxAccel(axis));
        bool ok = motionCard_->MoveAbs(binding.index, pulse, speedPulse);
        if (ok) {
            jogInProgress_ = false;   // Go 接管：终止任何残留点动状态
            // 卡轴按速度估算到位时间用于 Go 按钮门禁（卡本身 running 态也由轮询驱动）
            double dist = std::fabs(mmOrDeg - GetPosition(axis));
            double spd = (speed > 0) ? speed : GetJogSpeed(axis);
            int busyMs = (spd > 0.01) ? static_cast<int>(dist / spd * 1000.0) + 200 : 1000;
            MarkAxisBusy(axis, busyMs);
        }
        return ok;
    }
    if (binding.type == AxisBinding::Type::Servo) {
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) {
            bool ok = servo->MoveToAngle(targetPhys, 0);
            if (ok) {
                jogInProgress_ = false;   // Go 接管：终止任何残留点动状态
                MarkAxisBusy(axis, servo->GetLastMoveTimeMs());
            }
            SPDLOG_INFO("[HardwareManager] MoveAbs axis={} target={:.1f} -> {}", ai, mmOrDeg, ok);
            return ok;
        }
    }
    return false;
}

bool HardwareManager::MoveJog(LogicalAxis axis, double mmOrDegPerSec, int direction)
{
    // 使能门禁：未使能禁止点动（安全门禁，先使能再运动）
    if (!IsAxisEnabled(axis)) {
        SPDLOG_WARN("[HardwareManager] MoveJog rejected: axis {} not enabled", (int)axis);
        return false;
    }
    // 报警门禁：轴报警状态禁止点动（停止/急停除外）
    int ai = static_cast<int>(axis);
    if (ai >= 0 && ai < lastAlarm_.size() && lastAlarm_[ai]) {
        SPDLOG_WARN("[HardwareManager] MoveJog rejected: axis {} in alarm", (int)axis);
        return false;
    }
    // 回零门禁：回零进行中禁止点动，需先停止回零
    if (ai >= 0 && ai < homingActive_.size() && homingActive_[ai]) {
        SPDLOG_WARN("[HardwareManager] MoveJog rejected: axis {} homing in progress", ai);
        return false;
    }

    // 方向反转（config direction=反向）：物理运动方向取反，边界判断/回读均基于物理位置
    bool inv = (ai >= 0 && ai < static_cast<int>(axisConfigs_.size())) && axisConfigs_[ai].inverted;
    int effDir = inv ? -direction : direction;

    // 软限位校验：启动方向已在边界则拒绝（用「逻辑方向 direction」配「逻辑位置 current/lo/hi」。
    // 不能用物理方向 effDir：inverted 轴上两者相反，会导致上下界逻辑颠倒——越界方向放行、回程方向误拦）
    double lo = GetLimitMin(axis);
    double hi = GetLimitMax(axis);
    if (lo < hi) {
        double current = GetPosition(axis);
        // 容差 0.01：覆盖撞界自动停止后位置略低于边界的浮点夹紧舍入（实测 7.99996875），
        // 否则边界上重复点动会被放行（1e-6 太紧）
        if (direction > 0 && current >= hi - 0.01) {
            emit softLimitTriggered(static_cast<int>(axis), true);
            SPDLOG_WARN("[HardwareManager] MoveJog rejected: axis {} at max soft limit {:.1f}", static_cast<int>(axis), hi);
            return false;
        }
        if (direction < 0 && current <= lo + 0.01) {
            emit softLimitTriggered(static_cast<int>(axis), false);
            SPDLOG_WARN("[HardwareManager] MoveJog rejected: axis {} at min soft limit {:.1f}", static_cast<int>(axis), lo);
            return false;
        }
    }

    auto binding = AxisMap::Get(axis);
    if (binding.type == AxisBinding::Type::Card && motionCard_) {
        double pulseSpeed = AxisConverter::Instance().SpeedToPulse(static_cast<int>(axis), mmOrDegPerSec);
        // 加速度实时读 config（与「电控与映射」编辑一致），运动前刷新卡内快照
        motionCard_->SetAccel(binding.index, GetMaxAccel(axis));
        bool ok = motionCard_->MoveJog(binding.index, pulseSpeed, -1.0, effDir);
        if (ok) {
            jogAxis_ = axis;
            jogInProgress_ = true;
            MarkAxisBusy(axis, 3600 * 1000);   // 点动视为忙
        }
        return ok;
    }
    if (binding.type == AxisBinding::Type::Servo) {
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) {
            // 记录点动起点与起始时刻；JogTick 按时间累积推进目标，保证
            // 目标速度 = jogSpeed_（不再每 tick 查询/递增，避免抖动）
            // ReadAngle 为机械角度，点动起点/目标用逻辑坐标（= 机械 - homeOffset）
            double off = (ai >= 0 && ai < static_cast<int>(axisConfigs_.size())) ? axisConfigs_[ai].homePos : 0.0;
            double start = servo->ReadAngle() - off;
            jogAxis_       = axis;
            jogDir_        = effDir;
            jogSpeed_      = mmOrDegPerSec;
            jogStartPos_   = start;
            jogStartMs_    = QDateTime::currentMSecsSinceEpoch();
            lastJogTarget_ = start;
            jogInProgress_ = true;
            if (!jogTimer_->isActive()) jogTimer_->start();
            // 点动持续进行中，视为忙（UI 置灰 Go 按钮，防连点打断）
            MarkAxisBusy(axis, 3600 * 1000);
            SPDLOG_INFO("[HardwareManager] MoveJog axis={} dir={} speed={:.1f} start={:.1f}",
                        (int)axis, direction, mmOrDegPerSec, start);
            return true;
        }
    }
    return false;
}

void HardwareManager::StopJog(LogicalAxis axis)
{
    // 门禁：仅停止"当前正在点动的轴"。回零中松点动键（MoveJog 被回零门禁拒绝但
    // UI OnJogStop 仍会触发）曾无条件 StopJog → 误中断回零/其他轴运动；现非点动轴直接 return
    if (!jogInProgress_ || jogAxis_ != axis) {
        SPDLOG_INFO("[HardwareManager] StopJog skipped axis={} (jogInProgress={} jogAxis={})",
                    static_cast<int>(axis), jogInProgress_, static_cast<int>(jogAxis_));
        return;
    }
    auto binding = AxisMap::Get(axis);
    if (binding.type == AxisBinding::Type::Card && motionCard_) {
        motionCard_->StopJog(binding.index);
    } else if (binding.type == AxisBinding::Type::Servo) {
        if (jogTimer_->isActive()) jogTimer_->stop();
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) servo->Stop();
        SPDLOG_INFO("[HardwareManager] StopJog axis={}", (int)axis);
    }
    jogInProgress_ = false;
    // 停止回零/点动 → 解除回零门禁 + 忙结束，恢复 Go 按钮
    int i = static_cast<int>(axis);
    if (i >= 0 && i < homingActive_.size()) homingActive_[i] = false;
    if (i >= 0 && i < axisBusyUntilMs_.size()) {
        axisBusyUntilMs_[i] = 0;
        if (i < axisBusyNotified_.size() && !axisBusyNotified_[i]) {
            axisBusyNotified_[i] = true;
            emit axisMoveFinished(i);
        }
    }
}

bool HardwareManager::HomeAxis(LogicalAxis axis)
{
    // 使能门禁：未使能禁止回零（安全门禁，先使能再运动）
    if (!IsAxisEnabled(axis)) {
        SPDLOG_WARN("[HardwareManager] HomeAxis rejected: axis {} not enabled", (int)axis);
        return false;
    }
    // 报警门禁：轴报警状态禁止回零（停止/急停除外）
    int ai = static_cast<int>(axis);
    if (ai >= 0 && ai < lastAlarm_.size() && lastAlarm_[ai]) {
        SPDLOG_WARN("[HardwareManager] HomeAxis rejected: axis {} in alarm", (int)axis);
        return false;
    }
    // 重入防护：回零进行中拒绝重复触发（防止多次点击导致 SDK 状态机异常/崩溃）
    if (IsAxisBusy(axis)) {
        SPDLOG_WARN("[HardwareManager] HomeAxis rejected: axis {} busy (already moving/homing)", ai);
        return false;
    }

    auto binding = AxisMap::Get(axis);
    if (binding.type == AxisBinding::Type::Card && motionCard_) {
        bool ok = motionCard_->HomeAxis(binding.index);
        if (ok) {
            jogInProgress_ = false;   // 回零接管：点动状态终止
            if (ai >= 0 && ai < homingActive_.size()) homingActive_[ai] = true;   // 回零门禁
            if (ai >= 0 && ai < homeStartedMs_.size())
                homeStartedMs_[ai] = QDateTime::currentMSecsSinceEpoch();   // 完成检测保护期基准
            MarkAxisBusy(axis, 30000);   // 回零最长 30s，超时后 CheckAxisBusy 自动释放忙态
        }
        return ok;
    }
    if (binding.type == AxisBinding::Type::Servo) {
        if (jogTimer_->isActive() && axis == jogAxis_) jogTimer_->stop();
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) {
            bool ok = servo->MoveToAngle(0.0, 0);
            if (ok) {
                jogInProgress_ = false;   // 回零接管：点动状态终止
                if (ai >= 0 && ai < homingActive_.size()) homingActive_[ai] = true;
                MarkAxisBusy(axis, 5000);   // 舵机回零按 5s 上限
            }
            return ok;
        }
    }
    return false;
}

bool HardwareManager::StopAxis(LogicalAxis axis)
{
    auto binding = AxisMap::Get(axis);
    bool ok = false;
    if (binding.type == AxisBinding::Type::Card && motionCard_)
        ok = motionCard_->StopAxis(binding.index);
    else if (binding.type == AxisBinding::Type::Servo) {
        if (jogTimer_->isActive() && axis == jogAxis_) jogTimer_->stop();
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) ok = servo->Stop();
    }
    // 停止当前运动（点动/Go/回零）→ 解除回零门禁 + 忙结束，恢复 Go 按钮
    jogInProgress_ = false;
    int i = static_cast<int>(axis);
    if (i >= 0 && i < homingActive_.size()) homingActive_[i] = false;
    if (i >= 0 && i < axisBusyUntilMs_.size()) {
        axisBusyUntilMs_[i] = 0;
        if (i < axisBusyNotified_.size() && !axisBusyNotified_[i]) {
            axisBusyNotified_[i] = true;
            emit axisMoveFinished(i);
        }
    }
    return ok;
}

void HardwareManager::MarkAxisBusy(LogicalAxis axis, int busyMs)
{
    int i = static_cast<int>(axis);
    if (i < 0 || i >= axisBusyUntilMs_.size()) return;
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    axisBusyUntilMs_[i] = now + busyMs;
    if (i < axisBusyNotified_.size()) axisBusyNotified_[i] = false;
}

void HardwareManager::CheckAxisBusy()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < axisBusyUntilMs_.size(); ++i) {
        if (axisBusyUntilMs_[i] == 0) continue;
        if (now >= axisBusyUntilMs_[i]) {
            axisBusyUntilMs_[i] = 0;
            jogInProgress_ = false;   // 忙超时兜底：运动已终止（防御性）
            if (i < homingActive_.size()) homingActive_[i] = false;  // 回零超时兜底，舵机/无开关卡轴
            if (i < axisBusyNotified_.size() && !axisBusyNotified_[i]) {
                axisBusyNotified_[i] = true;
                emit axisMoveFinished(i);
            }
        }
    }
}

bool HardwareManager::IsAxisBusy(LogicalAxis axis) const
{
    int i = static_cast<int>(axis);
    if (i < 0 || i >= axisBusyUntilMs_.size()) return false;
    if (axisBusyUntilMs_[i] == 0) return false;
    return QDateTime::currentMSecsSinceEpoch() < axisBusyUntilMs_[i];
}

int HardwareManager::GetAxisBusyMs(LogicalAxis axis) const
{
    int i = static_cast<int>(axis);
    if (i < 0 || i >= axisBusyUntilMs_.size() || axisBusyUntilMs_[i] == 0) return 0;
    qint64 left = axisBusyUntilMs_[i] - QDateTime::currentMSecsSinceEpoch();
    return left > 0 ? static_cast<int>(left) : 0;
}

bool HardwareManager::HomeAll()
{
    // 需求3：全局轴使能未执行 / 全局断使能执行后，不允许一键回零
    if (!IsGlobalEnabled()) {
        SPDLOG_WARN("[HardwareManager] HomeAll rejected: axes not globally enabled");
        return false;
    }
    bool ok = true;
    for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i)
        ok = HomeAxis(static_cast<LogicalAxis>(i)) && ok;
    return ok;
}

bool HardwareManager::EnableAll()
{
    // 连接门禁：运动卡/舵机 COM 任一未连接，全局使能视为无效操作
    // （直接拒绝，axisEnabled_ 保持全 false，状态灯显示未使能）
    if (!IsMotionCardConnected() || !IsServoConnected()) {
        SPDLOG_WARN("[HardwareManager] EnableAll rejected: motionCard={} servo={}",
                    IsMotionCardConnected(), IsServoConnected());
        return false;
    }
    bool ok = true;
    // 逐轴记录使能结果，用于更新门禁状态（任一轴失败则该轴保持未使能）
    QVector<bool> axisOk(static_cast<int>(LogicalAxis::Count), false);
    if (motionCard_) {
        for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i) {
            auto binding = AxisMap::Get(static_cast<LogicalAxis>(i));
            if (binding.type == AxisBinding::Type::Card) {
                bool r = motionCard_->EnableAxis(binding.index);
                axisOk[i] = r;
                ok = r && ok;
            }
        }
    }
    if (servoJ2_) {
        bool r = servoJ2_->TorqueOn();
        axisOk[static_cast<int>(LogicalAxis::J2)] = r;
        ok = r && ok;
    }
    if (servoJ3_) {
        bool r = servoJ3_->TorqueOn();
        axisOk[static_cast<int>(LogicalAxis::R)] = r;
        ok = r && ok;
    }

    // 更新使能门禁状态
    for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i) {
        if (i < axisEnabled_.size()) axisEnabled_[i] = axisOk[i];
    }
    emit enableStateChanged();
    return ok;
}

bool HardwareManager::DisableAll()
{
    // 先停止一切进行中的运动/点动，再断使能，避免"点动中断使能后仍运动"
    if (jogTimer_->isActive()) jogTimer_->stop();
    if (motionCard_) motionCard_->StopAll();

    // 断使能视为回零中止
    jogInProgress_ = false;
    homingActive_.fill(false, static_cast<int>(LogicalAxis::Count));
    // 清除忙态时间戳，避免急停/断使能后 30s 内再回零/Go 被 IsAxisBusy 拒绝
    for (int i = 0; i < axisBusyUntilMs_.size(); ++i) {
        axisBusyUntilMs_[i] = 0;
        if (i < axisBusyNotified_.size() && !axisBusyNotified_[i]) {
            axisBusyNotified_[i] = true;
            emit axisMoveFinished(i);
        }
    }

    bool ok = true;
    if (motionCard_) {
        for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i) {
            auto binding = AxisMap::Get(static_cast<LogicalAxis>(i));
            if (binding.type == AxisBinding::Type::Card)
                ok = motionCard_->DisableAxis(binding.index) && ok;
        }
    }
    if (servoJ2_) ok = servoJ2_->TorqueOff() && ok;
    if (servoJ3_) ok = servoJ3_->TorqueOff() && ok;

    axisEnabled_.fill(false, static_cast<int>(LogicalAxis::Count));
    emit enableStateChanged();
    return ok;
}

bool HardwareManager::EmergencyStop()
{
    if (jogTimer_) jogTimer_->stop();
    if (motionCard_) motionCard_->EmergencyStop();
    if (servoJ2_) servoJ2_->TorqueOff();
    if (servoJ3_) servoJ3_->TorqueOff();
    // 急停后需重新手动使能才能继续点动/移动/回零
    axisEnabled_.fill(false, static_cast<int>(LogicalAxis::Count));
    jogInProgress_ = false;
    homingActive_.fill(false, static_cast<int>(LogicalAxis::Count));
    // 清除忙态时间戳，避免急停后 30s 内再回零/Go 被 IsAxisBusy 拒绝
    for (int i = 0; i < axisBusyUntilMs_.size(); ++i) {
        axisBusyUntilMs_[i] = 0;
        if (i < axisBusyNotified_.size() && !axisBusyNotified_[i]) {
            axisBusyNotified_[i] = true;
            emit axisMoveFinished(i);
        }
    }
    emit enableStateChanged();
    return true;
}

void HardwareManager::ShutdownHalt()
{
    // 退出清理：只做硬件层断使能。可被 aboutToQuit（UI 线程）或
    // ConsoleCtrlHandler（控制台信号线程）调用，故不触碰 Qt 对象。
    if (motionCard_) {
        for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i) {
            auto binding = AxisMap::Get(static_cast<LogicalAxis>(i));
            if (binding.type == AxisBinding::Type::Card)
                motionCard_->DisableAxis(binding.index);
        }
    }
    if (servoJ2_) servoJ2_->TorqueOff();
    if (servoJ3_) servoJ3_->TorqueOff();
}

double HardwareManager::GetPosition(LogicalAxis axis) const
{
    int i = static_cast<int>(axis);
    bool inv = (i >= 0 && i < static_cast<int>(axisConfigs_.size())) && axisConfigs_[i].inverted;
    double off = (i >= 0 && i < static_cast<int>(axisConfigs_.size())) ? axisConfigs_[i].homePos : 0.0;
    auto binding = AxisMap::Get(axis);
    if (binding.type == AxisBinding::Type::Card && motionCard_) {
        long pulse = static_cast<long>(motionCard_->GetPosition(binding.index));
        double phys = AxisConverter::Instance().ToPhysical(i, pulse);
        return (inv ? -phys : phys) - off;   // 逻辑角度 = 机械角度 - homeOffset
    }
    if (binding.type == AxisBinding::Type::Servo) {
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) {
            double ang = servo->ReadAngle();
            return (inv ? -ang : ang) - off;  // 逻辑角度 = 机械角度 - homeOffset
        }
    }
    return 0.0;
}

// ============================================================
// 每轴速度/限位参数
// ============================================================
double HardwareManager::GetJogSpeed(LogicalAxis axis) const
{
    return axisCfgSvc_ ? axisCfgSvc_->GetJogSpeed(axis) : 100.0;
}

double HardwareManager::GetMaxSpeed(LogicalAxis axis) const
{
    return axisCfgSvc_ ? axisCfgSvc_->GetMaxSpeed(axis) : 150.0;
}

double HardwareManager::GetMaxAccel(LogicalAxis axis) const
{
    return axisCfgSvc_ ? axisCfgSvc_->GetMaxAccel(axis) : 500.0;
}

bool HardwareManager::SetJogSpeed(LogicalAxis axis, double mmOrDegPerSec)
{
    return axisCfgSvc_ && axisCfgSvc_->SetJogSpeed(axis, mmOrDegPerSec);
}

QString HardwareManager::AxisUnit(LogicalAxis axis) const
{
    return axisCfgSvc_ ? axisCfgSvc_->AxisUnit(axis) : QString();
}

double HardwareManager::GetLimitMin(LogicalAxis axis) const
{
    return axisCfgSvc_ ? axisCfgSvc_->GetLimitMin(axis) : -180.0;
}

double HardwareManager::GetLimitMax(LogicalAxis axis) const
{
    return axisCfgSvc_ ? axisCfgSvc_->GetLimitMax(axis) : 180.0;
}

bool HardwareManager::IsWithinSoftLimits(LogicalAxis axis, double pos) const
{
    return axisCfgSvc_ ? axisCfgSvc_->IsWithinSoftLimits(axis, pos)
                       : (pos >= -1e6 && pos <= 1e6);
}

bool HardwareManager::IsAxisEnabled(LogicalAxis axis) const
{
    int i = static_cast<int>(axis);
    return i >= 0 && i < axisEnabled_.size() && axisEnabled_[i];
}

bool HardwareManager::IsGlobalEnabled() const
{
    for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i) {
        // 仅统计已绑定硬件（卡轴 / 舵机轴）的逻辑轴；空轴不参与判定
        auto binding = AxisMap::Get(static_cast<LogicalAxis>(i));
        bool bound = binding.type == AxisBinding::Type::Card ||
                     binding.type == AxisBinding::Type::Servo;
        if (bound && !IsAxisEnabled(static_cast<LogicalAxis>(i)))
            return false;
    }
    return true;
}

// ============================================================
// 舵机连续点动（50ms 定时器驱动）
// ============================================================
void HardwareManager::JogTick()
{
    auto binding = AxisMap::Get(jogAxis_);
    if (binding.type != AxisBinding::Type::Servo) return;
    IAxisServo* servo = (static_cast<int>(jogAxis_) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
    if (!servo) return;

    // JogTick 的目标是逻辑坐标（起点/速度/软限位均为逻辑），下发舵机须转回机械角度（+homeOffset）
    int jai = static_cast<int>(jogAxis_);
    double off = (jai >= 0 && jai < static_cast<int>(axisConfigs_.size())) ? axisConfigs_[jai].homePos : 0.0;

    // 目标 = 起点 + 方向 × 速度 × 已用时（时间累积模型，目标推进速度即设定速度）
    double elapsed = (QDateTime::currentMSecsSinceEpoch() - jogStartMs_) / 1000.0;
    double target = jogStartPos_ + jogDir_ * jogSpeed_ * elapsed;

    // 软限位：目标越过边界 → 夹紧到边界并停止
    double lo = GetLimitMin(jogAxis_);
    double hi = GetLimitMax(jogAxis_);
    bool hit = (lo < hi) &&
        ((jogDir_ > 0 && target >= hi - 1e-6) || (jogDir_ < 0 && target <= lo + 1e-6));
    if (hit) {
        target = (jogDir_ > 0) ? hi : lo;
        servo->MoveToAngle(target + off, 0);
        servo->Stop();
        jogTimer_->stop();
        jogInProgress_ = false;
        int idx = static_cast<int>(jogAxis_);
        if (idx >= 0 && idx < axisBusyUntilMs_.size()) {
            axisBusyUntilMs_[idx] = 0;
            if (idx < axisBusyNotified_.size() && !axisBusyNotified_[idx]) {
                axisBusyNotified_[idx] = true;
                emit axisMoveFinished(idx);
            }
        }
        if (idx >= 0 && idx < lastSoftLimitHit_.size() && !lastSoftLimitHit_[idx]) {
            lastSoftLimitHit_[idx] = true;
            emit softLimitTriggered(idx, jogDir_ > 0);
        }
        SPDLOG_WARN("[HardwareManager] JogTick axis={} hit soft limit -> stop at {:.1f}", (int)jogAxis_, target);
        return;
    }

    // 移出边界后清除软限位标记，允许再次撞限时重新触发
    int idx = static_cast<int>(jogAxis_);
    if (idx >= 0 && idx < lastSoftLimitHit_.size() && lastSoftLimitHit_[idx])
        lastSoftLimitHit_[idx] = false;

    // 发送节流：目标增量 ≥ 阈值（越过舵机死区）才下发一条 SET_ANGLE。
    // interval 必须按「相对上次已发送目标」的增量计算，而不能用 servo 内部缓存
    // （impl_->angle 会被遥测 PollTick 刷新成真实位置，真实位置滞后目标 5-8°，
    //  导致 interval 在 185ms/500ms 之间交替、舵机每次被打断重启 → 一顿一顿）。
    if (std::fabs(target - lastJogTarget_) >= kServoJogSendThreshold) {
        double step = target - lastJogTarget_;
        int intervalMs = static_cast<int>((std::fabs(step) / jogSpeed_) * 1000.0);
        if (intervalMs < 50) intervalMs = 50;
        if (intervalMs > 30000) intervalMs = 30000;
        bool ok = servo->MoveToAngle(target + off, intervalMs);
        lastJogTarget_ = target;
        SPDLOG_INFO("[HardwareManager] JogTick axis={} target={:.1f} step={:.2f} interval={}ms elapsed={:.2f}s -> {}",
                    (int)jogAxis_, target, step, intervalMs, elapsed, ok);
    }
}

// ============================================================
// 50ms 状态轮询
// ============================================================
void HardwareManager::PollTick()
{
    if (!motionCard_ && !servoJ2_ && !servoJ3_) return;

    CheckAxisBusy();   // 到位后发 axisMoveFinished（UI 恢复 Go 按钮）

    if (motionCard_) {
        // 仿真卡：先积分点动运动
        if (auto* sim = dynamic_cast<SimCard*>(motionCard_.get()))
            sim->Step(0.05);

        auto all = motionCard_->GetAllStatus();
        QVector<MotorStatus> vec;
        for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i) {
            auto binding = AxisMap::Get(static_cast<LogicalAxis>(i));
            if (binding.type == AxisBinding::Type::Card) {
                for (const auto& s : all) {
                    if (s.axisId == binding.index) {
                        MotorStatus st = s;
                        st.axisId   = i;  // 映射为逻辑轴索引
                        double phys = AxisConverter::Instance().ToPhysical(i, static_cast<long>(s.position));
                        double off  = (i >= 0 && i < static_cast<int>(axisConfigs_.size()))
                                          ? axisConfigs_[i].homePos : 0.0;
                        st.position = (axisConfigs_[i].inverted ? -phys : phys) - off;  // 界面按逻辑坐标显示
                        vec.push_back(st);

                        // 软限位：点动过程中到达边界 → 自动停止（只处理越界/到边界的轴）
                        // 仅在"正在点动撞入边界"时标记并通知，静止停在边界不触发。
                        // 回零中的轴不拦截：回零由卡 HomeStart 自行控制终点，PollTick 停止会打断回零搜索
                        double lo = GetLimitMin(static_cast<LogicalAxis>(i));
                        double hi = GetLimitMax(static_cast<LogicalAxis>(i));
                        if (lo < hi && (st.position >= hi - 1e-6 || st.position <= lo + 1e-6)) {
                            bool positive = st.position >= hi;
                            bool isHoming = (i < homingActive_.size() && homingActive_[i]);
                            if (st.running && !isHoming) {
                                // 按运动方向判断：仅拦截"仍朝越界方向运动"的轴（如惯性冲过边界后
                                // 继续向外冲）；若已开始朝边界内运动（反向离开越界区，delta 反向），
                                // 必须放行，否则出现"数值超界后持续反向点动无反应、只能一点点按"。
                                // 曾只按 position 越界 + running 即 StopJog，导致夹爪惯性冲到 0.2 后
                                // 按住"松开"每 50ms 被 StopJog 打断、Go 回界内也被立即停（与 J1 同源）。
                                double prev = (i < lastPollPos_.size()) ? lastPollPos_[i] : st.position;
                                double delta = st.position - prev;
                                bool movingOut = positive ? (delta >= 0.0) : (delta <= 0.0);
                                if (movingOut) {
                                    motionCard_->StopJog(binding.index);
                                    jogInProgress_ = false;
                                    if (i < lastSoftLimitHit_.size() && !lastSoftLimitHit_[i]) {
                                        lastSoftLimitHit_[i] = true;
                                        emit softLimitTriggered(i, positive);
                                    }
                                }
                            }
                        } else if (i < lastSoftLimitHit_.size() && lastSoftLimitHit_[i]) {
                            lastSoftLimitHit_[i] = false;
                        }
                        // 记录本轮逻辑位置，供下一轮 delta 方向判断
                        if (i < lastPollPos_.size()) lastPollPos_[i] = st.position;

                        // 回零完成检测：HomeAxis 置位后，卡 running 复位 → 回零结束，释放门禁与忙态。
                        // 必须已过最短保护期（发起后 1s）：MC_HomeStart 到卡端 running 置位存在启动间隙
                        // （含 Start 返回 1 的 HomeStop+重试窗口），间隙内单拍判定会误清 homingActive_，
                        // 随后真正的回零搜索被软限位 StopJog 误杀——J1 未回零压界 -102==limitMin 且
                        // inverted 搜索方向恰朝越界侧，必然触发（曾报"轴1到达软限位"且未碰原点开关）
                        if (i < homingActive_.size() && homingActive_[i] && !st.running
                            && i < homeStartedMs_.size()
                            && QDateTime::currentMSecsSinceEpoch() - homeStartedMs_[i] > 1000) {
                            homingActive_[i] = false;
                            if (i < axisBusyUntilMs_.size()) {
                                axisBusyUntilMs_[i] = 0;
                                if (i < axisBusyNotified_.size() && !axisBusyNotified_[i]) {
                                    axisBusyNotified_[i] = true;
                                    emit axisMoveFinished(i);
                                }
                            }
                            SPDLOG_INFO("[HardwareManager] axis {} home done (running cleared)", i);
                        }
                        break;
                    }
                }
            }
        }
        emit stateUpdated(vec);

        // 报警 / 限位边沿检测
        for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i) {
            auto binding = AxisMap::Get(static_cast<LogicalAxis>(i));
            if (binding.type != AxisBinding::Type::Card) continue;
            for (const auto& s : all) {
                if (s.axisId != binding.index) continue;
                if (s.alarm != lastAlarm_[i]) {
                    lastAlarm_[i] = s.alarm;
                    emit axisAlarm(i, s.alarm);
                }
                if (s.limitPositive != lastLimitPos_[i]) {
                    lastLimitPos_[i] = s.limitPositive;
                    emit limitTriggered(i, s.limitPositive, lastLimitNeg_[i]);
                }
                if (s.limitNegative != lastLimitNeg_[i]) {
                    lastLimitNeg_[i] = s.limitNegative;
                    emit limitTriggered(i, lastLimitPos_[i], s.limitNegative);
                }

                // 异常签名（报警/跟随误差/急停/硬软限位组合）边沿变化 → 落盘完整状态字日志
                // 仅边沿打印，正常运行时静默，避免 50ms 轮询刷爆日志
                unsigned long sig = 0;
                if (s.alarm)             sig |= 1UL << 0;
                if (s.followError)       sig |= 1UL << 1;
                if (s.estop)             sig |= 1UL << 2;
                if (s.limitPositive)     sig |= 1UL << 3;
                if (s.limitNegative)     sig |= 1UL << 4;
                if (s.softLimitPositive) sig |= 1UL << 5;
                if (s.softLimitNegative) sig |= 1UL << 6;
                if (i < lastAbnormalSig_.size() && sig != lastAbnormalSig_[i]) {
                    lastAbnormalSig_[i] = sig;
                    QString desc = DescribeAxisStatus(s);
                    if (sig != 0)
                        SPDLOG_WARN("[HardwareManager] axis {} abnormal -> {} | statusWord=0x{:08x} (pos={:.2f})",
                                    i, desc.toStdString(), s.statusWord, s.position);
                    else
                        SPDLOG_INFO("[HardwareManager] axis {} abnormal cleared -> {} | statusWord=0x{:08x}",
                                    i, desc.toStdString(), s.statusWord);
                }
                break;
            }
        }
    }

    if (servoJ2_ || servoJ3_) {
        // 离线时遥测降频到 20 tick（1s）：离线时每舵机查询最多阻塞 ~120ms（60ms 超时），
        // 250ms 轮询会把 UI 线程占满 → 急停/全局使能按钮事件排队秒级无响应（真机实测"卡死"）
        bool anyOffline = (servoJ2_ && !servoJ2_->IsOnline()) || (servoJ3_ && !servoJ3_->IsOnline());
        const int pollDiv = anyOffline ? 20 : 5;
        if (++servoPollCounter_ % pollDiv == 0) {
            QVector<ServoTelemetry> servos;
            auto toLogicalAngle = [this](LogicalAxis axis, ServoTelemetry t) {
                int idx = static_cast<int>(axis);
                double off = (idx >= 0 && idx < static_cast<int>(axisConfigs_.size()))
                                 ? axisConfigs_[idx].homePos : 0.0;
                t.angleDeg -= off;   // 界面统一显示逻辑角度 = 机械角度 - homeOffset
                return t;
            };
            if (servoJ2_) servos.push_back(toLogicalAngle(LogicalAxis::J2, servoJ2_->ReadTelemetry()));
            if (servoJ3_) servos.push_back(toLogicalAngle(LogicalAxis::R,  servoJ3_->ReadTelemetry()));
            emit servoStateUpdated(servos);

            // 热重连：任一舵机离线 → 先 Ping 确认再重连共享串口。
            // 失败指数退避（2s→4s→…→30s cap）；成功也冷却 30s——设备"半死"时
            // 成功归零曾形成 1-2s 一次的断→连→断循环（重连的 CloseHandle→CreateFileA→
            // DTR 翻转反而扰动总线，越连越抖）。在线后遥测查询成功会自动恢复 online，
            // 不依赖重连；仅串口句柄失效（拔 USB）需要重连，冷却最多延迟恢复 30s。
            if (anyOffline) {
                qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now >= servoNextReconnectMs_) {
                    bool j2Alive = !servoJ2_ || servoJ2_->Ping();
                    bool rAlive  = !servoJ3_ || servoJ3_->Ping();
                    if (j2Alive && rAlive) {
                        // Ping 通=句柄有效、设备应答，仅遥测瞬时失败：退避后再查，不重连
                        ++servoPingSkipCount_;
                        qint64 skipWait = std::min<qint64>(2000 << std::min(servoPingSkipCount_ - 1, 3), 15000);
                        SPDLOG_WARN("[HardwareManager] Servo offline but Ping ok, skip reconnect (wait {}ms)", skipWait);
                        servoNextReconnectMs_ = now + skipWait;
                    } else {
                        servoPingSkipCount_ = 0;
                        SPDLOG_WARN("[HardwareManager] Servo offline (ping J2={} R={}), reconnect backoff={}ms",
                                    j2Alive, rAlive, servoReconnectBackoffMs_);
                        ReconnectServos();
                        bool j2Online = servoJ2_ && servoJ2_->IsOnline();
                        bool rOnline  = servoJ3_ && servoJ3_->IsOnline();
                        if (j2Online && rOnline) {
                            servoReconnectBackoffMs_ = 0;
                            servoNextReconnectMs_ = now + 30000;   // 成功冷却 30s，打断抖动循环
                        } else {
                            // 重连失败：指数退避，上限 30s
                            servoReconnectBackoffMs_ = servoReconnectBackoffMs_ > 0
                                                           ? std::min<qint64>(servoReconnectBackoffMs_ * 2, 30000)
                                                           : 2000;
                            servoNextReconnectMs_ = now + servoReconnectBackoffMs_;
                        }
                    }
                }
            } else {
                servoReconnectBackoffMs_ = 0;
                servoPingSkipCount_ = 0;
            }
        }
    }
}

void HardwareManager::ReconnectServos()
{
    // 两个舵机共享同一串口句柄，必须一起断开（引用计数归零才会真正关闭）
    if (jogTimer_->isActive()) jogTimer_->stop();
    jogInProgress_ = false;
    // 重连=运动中被打断，同步解除回零门禁 + 忙态
    homingActive_.fill(false, static_cast<int>(LogicalAxis::Count));
    for (int i = 0; i < axisBusyUntilMs_.size(); ++i) {
        axisBusyUntilMs_[i] = 0;
        if (i < axisBusyNotified_.size() && !axisBusyNotified_[i]) {
            axisBusyNotified_[i] = true;
            emit axisMoveFinished(i);
        }
    }
    // 重连后舵机扭矩归零（断电/拔线），使能状态复位，需重新手动使能
    for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i) {
        auto binding = AxisMap::Get(static_cast<LogicalAxis>(i));
        if (binding.type == AxisBinding::Type::Servo && i < axisEnabled_.size())
            axisEnabled_[i] = false;
    }
    if (servoJ2_) servoJ2_->Disconnect();
    if (servoJ3_) servoJ3_->Disconnect();

    auto& cfg = ConfigManager::instance();
    std::string port = cfg.getValue<std::string>("communication.servo.port", "COM3");
    std::string baudStr = cfg.getValue<std::string>("communication.servo.baudRate", "115200");
    int baud = 115200;
    try { baud = std::stoi(baudStr); } catch (...) {}
    int idJ2 = cfg.getValue<int>("axes.Axis_J2.portId", 0);
    int idR  = cfg.getValue<int>("axes.Axis_R.portId", 1);
    double spdJ2 = cfg.getValue<double>("communication.servos[0].speed", 50.0);
    double spdR  = cfg.getValue<double>("communication.servos[1].speed", 50.0);

    bool j2Ok = servoJ2_ && servoJ2_->Connect(port, baud);
    if (j2Ok) {
        servoJ2_->SetServoId(static_cast<uint8_t>(idJ2));
        servoJ2_->SetSpeed(spdJ2);
    }
    bool rOk = servoJ3_ && servoJ3_->Connect(port, baud);
    if (rOk) {
        servoJ3_->SetServoId(static_cast<uint8_t>(idR));
        servoJ3_->SetSpeed(spdR);
    }
    if (!j2Ok || !rOk) {
        SPDLOG_WARN("[HardwareManager] Servo reconnect partial/failed: J2={} R={} (err J2={} R={})",
                    j2Ok, rOk,
                    (servoJ2_ ? servoJ2_->GetLastError() : "none"),
                    (servoJ3_ ? servoJ3_->GetLastError() : "none"));
    } else {
        SPDLOG_INFO("[HardwareManager] Servo reconnect done (J2 id={}, R id={})", idJ2, idR);
    }
    emit enableStateChanged();
}
