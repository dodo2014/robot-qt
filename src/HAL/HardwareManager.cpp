#include "HardwareManager.h"

#include "HALFactory.h"
#include "AxisConverter.h"
#include "ConfigManager.h"
#include "SimCard.h"
#include "SimServo.h"
#ifdef USE_BOPAI
#include "BoPaiCard.h"
#endif
#ifdef USE_XRSERVO
#include "XRServo.h"
#endif

namespace {

// 强制引用各硬件实现中的非内联成员函数，确保其 .obj 被链接，
// 否则静态库中的文件级注册对象会被链接器丢弃（dead-strip），
// 导致 REGISTER_*_MAKER 的注册永远不执行。
void ForceLinkHALImpls()
{
    auto f1 = &SimCard::Step;
    auto f2 = &SimServo::Connect;
#ifdef USE_BOPAI
    auto f3 = &BoPaiCard::Connect;
#endif
#ifdef USE_XRSERVO
    auto f4 = &XRServo::Connect;
#endif
    (void)f1; (void)f2;
#ifdef USE_BOPAI
    (void)f3;
#endif
#ifdef USE_XRSERVO
    (void)f4;
#endif

    // 兜底：若链接器仍丢弃了注册对象，则显式注册，确保工厂一定能创建。
    // 注册相同类型会覆盖原条目，因此与静态注册共存时也无副作用。
    AxisServoFactory::Instance().Register("SimServo",
        []() -> std::unique_ptr<IAxisServo> { return std::make_unique<SimServo>(); });
#ifdef USE_XRSERVO
    AxisServoFactory::Instance().Register("XRServo",
        []() -> std::unique_ptr<IAxisServo> { return std::make_unique<XRServo>(); });
#endif
}

}

#include <QCoreApplication>
#include <QFileInfo>

#include <spdlog/spdlog.h>
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
}

HardwareManager::~HardwareManager()
{
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

    ForceLinkHALImpls();  // 确保各硬件实现的静态注册对象被链接
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
        int baud = cfg.getValue<int>("communication.servo.baudRate", 115200);
        // 舵机 ID / 限位 / 速度从 config 读取（communication.servos[]）
        int idJ2 = cfg.getValue<int>("communication.servos[0].id", 1);
        int idR  = cfg.getValue<int>("communication.servos[1].id", 2);
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

    std::string camType = cfg.getValue<std::string>("simulation.cameraType", "SimCamera");
    camera_ = CameraFactory::Instance().Create(camType);

    // ---- 4. 每轴换算参数喂给卡与 AxisConverter ----
    LoadAxisConfigsFromConfig();

    // ---- 5. 使能 + 回零 ----
    if (motionCard_) {
        for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i) {
            auto binding = AxisMap::Get(static_cast<LogicalAxis>(i));
            if (binding.type == AxisBinding::Type::Card)
                motionCard_->EnableAxis(binding.index);
        }
    }
    if (servoJ2_) { servoJ2_->TorqueOn(); }
    if (servoJ3_) { servoJ3_->TorqueOn(); }

    // ---- 6. 启动 50ms 状态轮询 ----
    lastAlarm_.fill(false, static_cast<int>(LogicalAxis::Count));
    lastLimitPos_.fill(false, static_cast<int>(LogicalAxis::Count));
    lastLimitNeg_.fill(false, static_cast<int>(LogicalAxis::Count));
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
        ac.inverted     = (cfg.getValue<int>(base + "direction", 0) != 0);
        ac.hardwareType = cfg.getValue<int>(base + "hardwareType", 0);
        ac.pulsesPerRev = cfg.getValue<int>(base + "transmission.encoderResolution", 131072);
        ac.microSteps   = cfg.getValue<int>(base + "transmission.microSteps", 512);
        ac.lead         = cfg.getValue<double>(base + "transmission.lead", 20.0);
        ac.gearRatio    = cfg.getValue<double>(base + "transmission.gearRatio", 1.0);
        ac.pulsesPerUnit = (ac.pulsesPerRev * ac.microSteps) / (ac.hardwareType == 0 ? 360.0 : ac.lead);

        axisConfigs_[i] = ac;

        AxisConverter::Instance().ConfigureAxis(i, ac);

        auto binding = AxisMap::Get(static_cast<LogicalAxis>(i));
        if (binding.type == AxisBinding::Type::Card && motionCard_) {
            ac.axisId = binding.index;
            motionCard_->SetAxisConfig(binding.index, ac);
        }
    }
    SPDLOG_INFO("[HardwareManager] Axis configs loaded ({} axes)", static_cast<int>(LogicalAxis::Count));
}

// ============================================================
// 物理单位门面
// ============================================================
bool HardwareManager::MoveAbs(LogicalAxis axis, double mmOrDeg)
{
    auto binding = AxisMap::Get(axis);
    if (binding.type == AxisBinding::Type::Card && motionCard_) {
        long pulse = AxisConverter::Instance().ToPulse(static_cast<int>(axis), mmOrDeg);
        return motionCard_->MoveAbs(binding.index, pulse);
    }
    if (binding.type == AxisBinding::Type::Servo) {
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) return servo->MoveToAngle(mmOrDeg, 0);
    }
    return false;
}

bool HardwareManager::MoveJog(LogicalAxis axis, double mmOrDegPerSec, int direction)
{
    auto binding = AxisMap::Get(axis);
    if (binding.type == AxisBinding::Type::Card && motionCard_) {
        double pulseSpeed = AxisConverter::Instance().SpeedToPulse(static_cast<int>(axis), mmOrDegPerSec);
        return motionCard_->MoveJog(binding.index, pulseSpeed, -1.0, direction);
    }
    if (binding.type == AxisBinding::Type::Servo) {
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) {
            // 记录点动状态并启动 50ms 定时器，按住期间持续递增目标角 → 连续运动
            jogAxis_  = axis;
            jogDir_   = direction;
            jogStep_  = 1.0;
            jogSpeed_ = mmOrDegPerSec;
            if (!jogTimer_->isActive()) jogTimer_->start();
            return true;
        }
    }
    return false;
}

void HardwareManager::StopJog(LogicalAxis axis)
{
    auto binding = AxisMap::Get(axis);
    if (binding.type == AxisBinding::Type::Card && motionCard_) {
        motionCard_->StopJog(binding.index);
    } else if (binding.type == AxisBinding::Type::Servo) {
        if (jogTimer_->isActive()) jogTimer_->stop();
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) servo->Stop();
    }
}

bool HardwareManager::HomeAxis(LogicalAxis axis)
{
    auto binding = AxisMap::Get(axis);
    if (binding.type == AxisBinding::Type::Card && motionCard_)
        return motionCard_->HomeAxis(binding.index);
    if (binding.type == AxisBinding::Type::Servo) {
        if (jogTimer_->isActive() && axis == jogAxis_) jogTimer_->stop();
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) return servo->MoveToAngle(0.0, 0);
    }
    return false;
}

bool HardwareManager::StopAxis(LogicalAxis axis)
{
    auto binding = AxisMap::Get(axis);
    if (binding.type == AxisBinding::Type::Card && motionCard_)
        return motionCard_->StopAxis(binding.index);
    if (binding.type == AxisBinding::Type::Servo) {
        if (jogTimer_->isActive() && axis == jogAxis_) jogTimer_->stop();
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) return servo->Stop();
    }
    return false;
}

bool HardwareManager::HomeAll()
{
    bool ok = true;
    for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i)
        ok = HomeAxis(static_cast<LogicalAxis>(i)) && ok;
    return ok;
}

bool HardwareManager::EnableAll()
{
    bool ok = true;
    if (motionCard_) {
        for (int i = 0; i < static_cast<int>(LogicalAxis::Count); ++i) {
            auto binding = AxisMap::Get(static_cast<LogicalAxis>(i));
            if (binding.type == AxisBinding::Type::Card)
                ok = motionCard_->EnableAxis(binding.index) && ok;
        }
    }
    if (servoJ2_) ok = servoJ2_->TorqueOn() && ok;
    if (servoJ3_) ok = servoJ3_->TorqueOn() && ok;
    return ok;
}

bool HardwareManager::DisableAll()
{
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
    return ok;
}

bool HardwareManager::EmergencyStop()
{
    if (jogTimer_) jogTimer_->stop();
    if (motionCard_) motionCard_->EmergencyStop();
    if (servoJ2_) servoJ2_->TorqueOff();
    if (servoJ3_) servoJ3_->TorqueOff();
    return true;
}

double HardwareManager::GetPosition(LogicalAxis axis) const
{
    auto binding = AxisMap::Get(axis);
    if (binding.type == AxisBinding::Type::Card && motionCard_) {
        long pulse = static_cast<long>(motionCard_->GetPosition(binding.index));
        return AxisConverter::Instance().ToPhysical(static_cast<int>(axis), pulse);
    }
    if (binding.type == AxisBinding::Type::Servo) {
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) return servo->ReadAngle();
    }
    return 0.0;
}

// ============================================================
// 每轴速度/限位参数
// ============================================================
double HardwareManager::GetJogSpeed(LogicalAxis axis) const
{
    int i = static_cast<int>(axis);
    if (i >= 0 && i < axisConfigs_.size())
        return axisConfigs_[i].jogSpeed;
    return 100.0;
}

double HardwareManager::GetMaxSpeed(LogicalAxis axis) const
{
    // 直接读 config（axes.<key>.maxSpeed），保证与 ConfigPage「电控与映射」的编辑实时一致
    int i = static_cast<int>(axis);
    if (i < 0 || i >= static_cast<int>(LogicalAxis::Count)) return 150.0;
    return ConfigManager::instance().getValue<double>(
        std::string("axes.") + kAxisConfigKeys[i] + ".maxSpeed", 150.0);
}

bool HardwareManager::SetJogSpeed(LogicalAxis axis, double mmOrDegPerSec)
{
    int i = static_cast<int>(axis);
    if (i < 0 || i >= axisConfigs_.size()) return false;
    axisConfigs_[i].jogSpeed = mmOrDegPerSec;

    auto& cfg = ConfigManager::instance();
    cfg.set("axes." + std::string(kAxisConfigKeys[i]) + ".jogSpeed", mmOrDegPerSec);
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
    double current = servo->ReadAngle();
    servo->MoveAtSpeed(current + jogDir_ * jogStep_, jogSpeed_);
}

// ============================================================
// 50ms 状态轮询
// ============================================================
void HardwareManager::PollTick()
{
    if (!motionCard_ && !servoJ2_ && !servoJ3_) return;

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
                        st.position = AxisConverter::Instance().ToPhysical(i, static_cast<long>(s.position));
                        vec.push_back(st);
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
                break;
            }
        }
    }

    if (servoJ2_ || servoJ3_) {
        QVector<ServoTelemetry> servos;
        if (servoJ2_) servos.push_back(servoJ2_->ReadTelemetry());
        if (servoJ3_) servos.push_back(servoJ3_->ReadTelemetry());
        emit servoStateUpdated(servos);
    }
}
