#include "HardwareManager.h"

#include <QDateTime>

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
        int    w   = cfg.getValue<int>("vision.frameWidth", 640);
        int    h   = cfg.getValue<int>("vision.frameHeight", 480);
        double fps = cfg.getValue<double>("vision.frameFps", 30.0);
        CameraOpen(w, h, fps);
    }

    // ---- 4. 每轴换算参数喂给卡与 AxisConverter ----
    LoadAxisConfigsFromConfig();

    // ---- 5. 启动 50ms 状态轮询 ----
    lastAlarm_.fill(false, static_cast<int>(LogicalAxis::Count));
    lastLimitPos_.fill(false, static_cast<int>(LogicalAxis::Count));
    lastLimitNeg_.fill(false, static_cast<int>(LogicalAxis::Count));
    lastSoftLimitHit_.fill(false, static_cast<int>(LogicalAxis::Count));
    // 使能门禁：初始全轴未使能。使能必须手动触发（EnableAll），程序不自动使能。
    axisEnabled_.fill(false, static_cast<int>(LogicalAxis::Count));
    axisBusyUntilMs_.fill(0, static_cast<int>(LogicalAxis::Count));
    axisBusyNotified_.fill(false, static_cast<int>(LogicalAxis::Count));
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
        // 轴类型：rotation(角度)/linear(直线 mm)，决定换算用 360° 还是 导程×减速比
        ac.axisType     = (cfg.getValue<std::string>(base + "axisType", "rotation") == "linear") ? 1 : 0;
        ac.pulsesPerRev = cfg.getValue<int>(base + "transmission.encoderResolution", 32000);
        ac.microSteps   = cfg.getValue<int>(base + "transmission.microSteps", 1);
        ac.lead         = cfg.getValue<double>(base + "transmission.lead", 360.0);
        ac.gearRatio    = cfg.getValue<double>(base + "transmission.gearRatio", 1.0);
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

    // 软限位校验：目标位置超出 [limitMin, limitMax] 直接拒绝
    if (!IsWithinSoftLimits(axis, mmOrDeg)) {
        SPDLOG_WARN("[HardwareManager] MoveAbs rejected: axis {} target {:.1f} out of soft limits [{:.1f}, {:.1f}]",
                    static_cast<int>(axis), mmOrDeg, GetLimitMin(axis), GetLimitMax(axis));
        return false;
    }

    auto binding = AxisMap::Get(axis);
    if (binding.type == AxisBinding::Type::Card && motionCard_) {
        long pulse = AxisConverter::Instance().ToPulse(static_cast<int>(axis), mmOrDeg);
        bool ok = motionCard_->MoveAbs(binding.index, pulse);
        if (ok) {
            // 卡轴按速度估算到位时间用于 Go 按钮门禁（卡本身 running 态也由轮询驱动）
            double dist = std::fabs(mmOrDeg - GetPosition(axis));
            double spd = GetJogSpeed(axis);
            int busyMs = (spd > 0.01) ? static_cast<int>(dist / spd * 1000.0) + 200 : 1000;
            MarkAxisBusy(axis, busyMs);
        }
        return ok;
    }
    if (binding.type == AxisBinding::Type::Servo) {
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) {
            bool ok = servo->MoveToAngle(mmOrDeg, 0);
            if (ok) MarkAxisBusy(axis, servo->GetLastMoveTimeMs());
            SPDLOG_INFO("[HardwareManager] MoveAbs axis={} target={:.1f} -> {}", (int)axis, mmOrDeg, ok);
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
        bool ok = motionCard_->MoveJog(binding.index, pulseSpeed, -1.0, direction);
        if (ok) MarkAxisBusy(axis, 3600 * 1000);   // 点动视为忙
        return ok;
    }
    if (binding.type == AxisBinding::Type::Servo) {
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) {
            // 记录点动起点与起始时刻；JogTick 按时间累积推进目标，保证
            // 目标速度 = jogSpeed_（不再每 tick 查询/递增，避免抖动）
            double start = servo->ReadAngle();
            jogAxis_       = axis;
            jogDir_        = direction;
            jogSpeed_      = mmOrDegPerSec;
            jogStartPos_   = start;
            jogStartMs_    = QDateTime::currentMSecsSinceEpoch();
            lastJogTarget_ = start;
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
    auto binding = AxisMap::Get(axis);
    if (binding.type == AxisBinding::Type::Card && motionCard_) {
        motionCard_->StopJog(binding.index);
    } else if (binding.type == AxisBinding::Type::Servo) {
        if (jogTimer_->isActive()) jogTimer_->stop();
        IAxisServo* servo = (static_cast<int>(axis) == static_cast<int>(LogicalAxis::J2)) ? servoJ2_.get() : servoJ3_.get();
        if (servo) servo->Stop();
        SPDLOG_INFO("[HardwareManager] StopJog axis={}", (int)axis);
    }
    // 停止点动 → 忙结束，恢复 Go 按钮
    int i = static_cast<int>(axis);
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

QString HardwareManager::AxisUnit(LogicalAxis axis) const
{
    int i = static_cast<int>(axis);
    if (i < 0 || i >= static_cast<int>(LogicalAxis::Count)) return QString();
    if (AxisMap::Get(axis).type == AxisBinding::Type::Servo)
        return QStringLiteral("\xC2\xB0");
    if (i < axisConfigs_.size() && axisConfigs_[i].axisType == 1)
        return QStringLiteral("mm");
    return QStringLiteral("\xC2\xB0");
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
        servo->MoveToAngle(target, 0);
        servo->Stop();
        jogTimer_->stop();
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
        bool ok = servo->MoveToAngle(target, intervalMs);
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
        // 串口事务在 UI 线程执行，降频到每 5 tick（250ms）查询一次
        if (++servoPollCounter_ % 5 == 0) {
            QVector<ServoTelemetry> servos;
            if (servoJ2_) servos.push_back(servoJ2_->ReadTelemetry());
            if (servoJ3_) servos.push_back(servoJ3_->ReadTelemetry());
            emit servoStateUpdated(servos);

            // 热重连：任一舵机持续离线达到阈值（约 1s）→ 重连共享串口
            bool anyOffline = (servoJ2_ && !servoJ2_->IsOnline()) || (servoJ3_ && !servoJ3_->IsOnline());
            if (anyOffline) {
                if (++servoOfflineTicks_ >= 4) {
                    servoOfflineTicks_ = 0;
                    SPDLOG_WARN("[HardwareManager] Servo offline for ~1s, attempting reconnect");
                    ReconnectServos();
                }
            } else {
                servoOfflineTicks_ = 0;
            }
        }
    }
}

void HardwareManager::ReconnectServos()
{
    // 两个舵机共享同一串口句柄，必须一起断开（引用计数归零才会真正关闭）
    if (jogTimer_->isActive()) jogTimer_->stop();
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

    if (servoJ2_) {
        if (servoJ2_->Connect(port, baud)) {
            servoJ2_->SetServoId(static_cast<uint8_t>(idJ2));
            servoJ2_->SetSpeed(spdJ2);
        }
    }
    if (servoJ3_) {
        if (servoJ3_->Connect(port, baud)) {
            servoJ3_->SetServoId(static_cast<uint8_t>(idR));
            servoJ3_->SetSpeed(spdR);
        }
    }
    SPDLOG_INFO("[HardwareManager] Servo reconnect done (J2 id={}, R id={})", idJ2, idR);
    emit enableStateChanged();
}
