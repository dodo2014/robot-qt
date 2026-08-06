#include "HardwareManager.h"

#include "HALFactory.h"
#include "AxisConverter.h"
#include "ConfigManager.h"
#include "SimCard.h"
#include "SimServo.h"
#include "SimCamera.h"
#include "SimAlgo.h"
#include "CameraCaptureWorker.h"
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
    auto f5 = &SimCamera::CaptureFrame;
    auto f6 = &SimAlgo::Detect;
#ifdef USE_BOPAI
    auto f3 = &BoPaiCard::Connect;
#endif
#ifdef USE_XRSERVO
    auto f4 = &XRServo::Connect;
#endif
    (void)f1; (void)f2; (void)f5; (void)f6;
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
    CameraFactory::Instance().Register("SimCamera",
        []() -> std::unique_ptr<ICamera> { return std::make_unique<SimCamera>(); });
    PuffAlgorithmFactory::Instance().Register("SimAlgo",
        []() -> std::unique_ptr<IPuffAlgorithm> { return std::make_unique<SimAlgo>(); });
#ifdef USE_XRSERVO
    AxisServoFactory::Instance().Register("XRServo",
        []() -> std::unique_ptr<IAxisServo> { return std::make_unique<XRServo>(); });
#endif
}

}

#include <QCoreApplication>
#include <QFileInfo>
#include <QThread>

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

    cameraThread_ = new QThread(this);
    cameraWorker_ = new CameraCaptureWorker();
    cameraWorker_->moveToThread(cameraThread_);
    connect(cameraThread_, &QThread::finished, cameraWorker_, &QObject::deleteLater);
    connect(cameraWorker_, &CameraCaptureWorker::frameReady, this, &HardwareManager::frameReady);
}

HardwareManager::~HardwareManager()
{
    StopCameraStream();
    if (cameraThread_ && cameraThread_->isRunning()) {
        cameraThread_->quit();
        cameraThread_->wait(2000);
    }
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
        int    w   = cfg.getValue<int>("vision.frameWidth", 640);
        int    h   = cfg.getValue<int>("vision.frameHeight", 480);
        double fps = cfg.getValue<double>("vision.frameFps", 30.0);
        CameraOpen(w, h, fps);
    }

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
    lastSoftLimitHit_.fill(false, static_cast<int>(LogicalAxis::Count));
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
    if (!camera_) {
        SPDLOG_WARN("[HardwareManager] CameraOpen failed: no camera instance");
        return false;
    }
    std::string deviceId = ConfigManager::instance().getValue<std::string>(
        "simulation.cameraDeviceId", "CAM-SIM-001");
    bool ok = camera_->Open(deviceId, width, height, fps);
    if (!ok)
        SPDLOG_WARN("[HardwareManager] CameraOpen FAILED: {}", camera_->GetLastError());
    else
        SPDLOG_INFO("[HardwareManager] Camera opened");
    return ok;
}

void HardwareManager::CameraClose()
{
    StopCameraStream();
    if (camera_) camera_->Close();
}

bool HardwareManager::StartCameraStream(int fps)
{
    if (!camera_) {
        SPDLOG_WARN("[HardwareManager] StartCameraStream failed: no camera instance");
        return false;
    }
    if (!camera_->IsOpened())
        CameraOpen(0, 0, 0);
    if (!camera_->StartStream()) {
        SPDLOG_WARN("[HardwareManager] StartStream FAILED: {}", camera_->GetLastError());
        return false;
    }
    if (!cameraThread_->isRunning()) {
        cameraWorker_->SetCamera(camera_.get());
        cameraThread_->start();
    }
    QMetaObject::invokeMethod(cameraWorker_, "Start", Qt::QueuedConnection,
                              Q_ARG(int, fps));
    cameraStreaming_ = true;
    return true;
}

void HardwareManager::StopCameraStream()
{
    cameraStreaming_ = false;
    if (camera_) camera_->StopStream();
    if (cameraWorker_)
        QMetaObject::invokeMethod(cameraWorker_, "Stop", Qt::QueuedConnection);
}

bool HardwareManager::IsCameraStreaming() const
{
    return cameraStreaming_;
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
    // 软限位校验：目标位置超出 [limitMin, limitMax] 直接拒绝
    if (!IsWithinSoftLimits(axis, mmOrDeg)) {
        SPDLOG_WARN("[HardwareManager] MoveAbs rejected: axis {} target {:.1f} out of soft limits [{:.1f}, {:.1f}]",
                    static_cast<int>(axis), mmOrDeg, GetLimitMin(axis), GetLimitMax(axis));
        return false;
    }

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
    // 软限位校验：启动方向已在边界则拒绝
    double lo = GetLimitMin(axis);
    double hi = GetLimitMax(axis);
    if (lo < hi) {
        double current = GetPosition(axis);
        if (direction > 0 && current >= hi - 1e-6) {
            emit softLimitTriggered(static_cast<int>(axis), true);
            SPDLOG_WARN("[HardwareManager] MoveJog rejected: axis {} at max soft limit {:.1f}", static_cast<int>(axis), hi);
            return false;
        }
        if (direction < 0 && current <= lo + 1e-6) {
            emit softLimitTriggered(static_cast<int>(axis), false);
            SPDLOG_WARN("[HardwareManager] MoveJog rejected: axis {} at min soft limit {:.1f}", static_cast<int>(axis), lo);
            return false;
        }
    }

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

double HardwareManager::GetLimitMin(LogicalAxis axis) const
{
    int i = static_cast<int>(axis);
    if (i < 0 || i >= static_cast<int>(LogicalAxis::Count)) return -180.0;
    return ConfigManager::instance().getValue<double>(
        std::string("axes.") + kAxisConfigKeys[i] + ".limitMin", -180.0);
}

double HardwareManager::GetLimitMax(LogicalAxis axis) const
{
    int i = static_cast<int>(axis);
    if (i < 0 || i >= static_cast<int>(LogicalAxis::Count)) return 180.0;
    return ConfigManager::instance().getValue<double>(
        std::string("axes.") + kAxisConfigKeys[i] + ".limitMax", 180.0);
}

bool HardwareManager::IsWithinSoftLimits(LogicalAxis axis, double pos) const
{
    double lo = GetLimitMin(axis);
    double hi = GetLimitMax(axis);
    if (lo >= hi) return true; // 配置错误（min>=max）：视为不限制
    return pos >= lo - 1e-6 && pos <= hi + 1e-6;
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

    // 软限位：下一步越过边界 → 夹紧到边界并停止
    double lo = GetLimitMin(jogAxis_);
    double hi = GetLimitMax(jogAxis_);
    if (lo < hi) {
        double next = current + jogDir_ * jogStep_;
        bool hit = (jogDir_ > 0 && next >= hi - 1e-6) || (jogDir_ < 0 && next <= lo + 1e-6);
        if (hit) {
            next = (jogDir_ > 0) ? hi : lo;
            servo->MoveToAngle(next, 0);
            servo->Stop();
            jogTimer_->stop();
            int idx = static_cast<int>(jogAxis_);
            if (idx >= 0 && idx < lastSoftLimitHit_.size() && !lastSoftLimitHit_[idx]) {
                lastSoftLimitHit_[idx] = true;
                emit softLimitTriggered(idx, jogDir_ > 0);
            }
            return;
        }
    }

    // 移出边界后清除软限位标记，允许再次撞限时重新触发
    int idx = static_cast<int>(jogAxis_);
    if (idx >= 0 && idx < lastSoftLimitHit_.size() && lastSoftLimitHit_[idx])
        lastSoftLimitHit_[idx] = false;

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

                        // 软限位：点动过程中到达边界 → 自动停止（只处理越界/到边界的轴）
                        // 仅在“正在点动撞入边界”时标记并通知，静止停在边界（如 Z/夹爪/挤出的
                        // 初始最小位置 0）不触发，避免启动即误报。
                        double lo = GetLimitMin(static_cast<LogicalAxis>(i));
                        double hi = GetLimitMax(static_cast<LogicalAxis>(i));
                        if (lo < hi && (st.position >= hi - 1e-6 || st.position <= lo + 1e-6)) {
                            bool positive = st.position >= hi;
                            if (st.running) {
                                motionCard_->StopJog(binding.index);
                                if (i < lastSoftLimitHit_.size() && !lastSoftLimitHit_[i]) {
                                    lastSoftLimitHit_[i] = true;
                                    emit softLimitTriggered(i, positive);
                                }
                            }
                        } else if (i < lastSoftLimitHit_.size() && lastSoftLimitHit_[i]) {
                            lastSoftLimitHit_[i] = false;
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
