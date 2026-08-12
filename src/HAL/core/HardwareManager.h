#pragma once

#include <QObject>
#include <QTimer>
#include <QVector>

#include <memory>
#include <string>

#include "IMotionCard.h"
#include "IAxisServo.h"
#include "IEndEffector.h"
#include "ICamera.h"
#include "IPuffAlgorithm.h"
#include "AxisMap.h"

class QThread;
class CameraManager;
class AxisConfigService;

// ============================================================
// 硬件组装管理 (HardwareManager)
// 单例。职责：
//   1. 根据 config 的 simulation.*Type 经 HALFactory 创建各硬件实例
//   2. 将每轴换算参数通过 IMotionCard::SetAxisConfig 喂给底层卡
//      （HAL 底层卡代码不主动读配置）
//   3. 对外提供物理单位(mm/度)的调用门面，内部经 AxisConverter
//      换算成脉冲后调用底层卡
//   4. 50ms QTimer 高频轮询底层状态，反向换算后经 Qt 信号广播，
//      供 ManualControlPage 等 UI 刷新位置与状态灯
// ============================================================
class HardwareManager : public QObject
{
    Q_OBJECT

public:
    static HardwareManager& instance();

    // 读取 config 并创建/连接/使能所有硬件。可重复调用（幂等）。
    bool Initialize();
    bool IsInitialized() const;

    // ---- 物理单位门面 (mm / 度) ----
    bool MoveAbs(LogicalAxis axis, double mmOrDeg, double speed = 0.0);
    bool MoveJog(LogicalAxis axis, double mmOrDegPerSec, int direction = 1);
    void StopJog(LogicalAxis axis);
    bool HomeAxis(LogicalAxis axis);
    bool StopAxis(LogicalAxis axis);
    bool HomeAll();
    bool EnableAll();
    bool DisableAll();
    bool EmergencyStop();
    // 进程退出清理（线程安全）：仅硬件层断使能，不触碰 Qt 对象（QTimer 等非线程安全）。
    // 供 aboutToQuit 与 Windows 控制台信号处理器（ConsoleCtrlHandler 独立线程）调用。
    void ShutdownHalt();

    // ---- 使能状态门禁（唯一事实源） ----
    // 点动/移动/回零 必须先使能（手动触发），未使能拒绝执行；断使能/急停后需重新使能。
    // 注意：不允许程序自动使能（Initialize 不再自动使能），热重连/硬件断开会使状态复位。
    bool IsAxisEnabled(LogicalAxis axis) const;
    // 所有已配置的卡轴 + 舵机轴均已使能（供"一键回零"等全局操作判断）
    bool IsGlobalEnabled() const;

    // 轴是否"运动中/忙"（Go 发出去到估计到位之间）。UI 据此置灰 Go 按钮，
    // 防止多次点击导致重复打断与指令覆盖（曾引发舵机突然加速）。
    bool IsAxisBusy(LogicalAxis axis) const;
    int  GetAxisBusyMs(LogicalAxis axis) const;

    double GetPosition(LogicalAxis axis) const;

    // ---- 每轴速度/限位参数（加载自 config，供 UI 读取与修改） ----
    double GetJogSpeed(LogicalAxis axis) const;
    double GetMaxSpeed(LogicalAxis axis) const;
    bool   SetJogSpeed(LogicalAxis axis, double mmOrDegPerSec);

    // ---- 轴显示单位（rotation→"°"，linear→"mm"，舵机→"°"） ----
    QString AxisUnit(LogicalAxis axis) const;

    // ---- 软限位（实时读 config，与「电控与映射」编辑保持一致） ----
    double GetLimitMin(LogicalAxis axis) const;
    double GetLimitMax(LogicalAxis axis) const;
    bool   IsWithinSoftLimits(LogicalAxis axis, double pos) const;

    // ---- 连接状态 ----
    bool IsMotionCardConnected() const;
    bool IsServoConnected() const;
    QString ConnectionStatus() const;

    // ---- 硬件访问器 ----
    IMotionCard*   motionCard()  const { return motionCard_.get(); }
    IAxisServo*    servoJ2()     const { return servoJ2_.get(); }
    IAxisServo*    servoJ3()     const { return servoJ3_.get(); }
    IEndEffector*  gripper()     const { return gripper_.get(); }
    ICamera*       camera()      const { return camera_.get(); }
    IPuffAlgorithm* algorithm()  const { return algorithm_.get(); }

    // ---- 相机流（采集独立线程，帧经 frameReady 信号广播） ----
    bool CameraOpen(int width, int height, double fps);
    void CameraClose();
    bool StartCameraStream(int fps = 30);
    void StopCameraStream();
    bool IsCameraStreaming() const;

signals:
    // 高频状态广播 (50ms)，position 已换算为物理单位 (mm/度)
    void stateUpdated(const QVector<MotorStatus>& axes);
    void servoStateUpdated(const QVector<ServoTelemetry>& servos);
    void axisAlarm(int logicalAxis, bool alarm);
    void limitTriggered(int logicalAxis, bool positive, bool negative);
    // 轴已到达软限位边界（点动撞限被自动停止，或点动启动方向已在边界）。
    // positive=true 表示撞到 Max，false 表示撞到 Min。
    void softLimitTriggered(int logicalAxis, bool positive);
    // 连接状态变化（Initialize 之后或重连后发出）
    void connectionChanged();
    // 使能状态变化（EnableAll/DisableAll/EmergencyStop/热重连后发出），UI 据此刷新
    void enableStateChanged();
    // 轴运动结束（Go 到位 / 停止 / 急停），UI 据此恢复 Go 按钮
    void axisMoveFinished(int logicalAxis);
    // 相机采集线程产出的最新帧（值类型，跨线程自动深拷贝）
    void frameReady(const CameraFrame& frame);

private:
    HardwareManager();
    ~HardwareManager() override;
    HardwareManager(const HardwareManager&) = delete;
    HardwareManager& operator=(const HardwareManager&) = delete;

    void PollTick();
    void LoadAxisConfigsFromConfig();
    void JogTick();
    // 舵机串口断开后的热重连：两个实例共享同一句柄，须一起断开重连
    void ReconnectServos();

    QTimer* pollTimer_ = nullptr;
    QTimer* jogTimer_  = nullptr;
    bool initialized_ = false;

    // 舵机遥测轮询计数：每 5 个 poll tick（250ms）查询一次，避免
    // 阻塞串口事务占满主线程（真机每事务约几 ms，逐 tick 查询会卡 UI）
    int servoPollCounter_ = 0;
    // 舵机连续离线次数（每 250ms 遥测刷新一次），达到阈值触发热重连
    int servoOfflineTicks_ = 0;

    // 舵机连续点动状态（jogTimer_ 驱动）
    // 目标按时间累积推进（速度 = jogSpeed_），发送节流 ≥2°（越过 FashionStar
    // 约 1.5° 控制死区）。曾用「每 tick current+3°」导致推进 60°/s 远超设定速度、
    // 舵机追不上而一顿一顿。
    LogicalAxis jogAxis_      = LogicalAxis::J1;
    int         jogDir_       = 1;
    double      jogSpeed_     = 50.0;
    double      jogStartPos_  = 0.0;
    qint64      jogStartMs_   = 0;
    double      lastJogTarget_ = 0.0;
    static constexpr double kServoJogSendThreshold = 2.0; // 目标增量 ≥ 此值才下发

    std::unique_ptr<IMotionCard>    motionCard_;
    std::unique_ptr<IAxisServo>     servoJ2_;
    std::unique_ptr<IAxisServo>     servoJ3_;
    std::unique_ptr<IEndEffector>   gripper_;
    std::unique_ptr<ICamera>        camera_;
    std::unique_ptr<IPuffAlgorithm> algorithm_;

    // 相机生命周期（采集线程/worker/流状态）委托给 CameraManager
    std::unique_ptr<CameraManager>  cameraManager_;
    // 每轴速度/单位/软限位查询（委托 AxisConfigService）
    std::unique_ptr<AxisConfigService> axisCfgSvc_;

    // 每个逻辑轴从 config 加载的配置快照（LoadAxisConfigsFromConfig 填充）
    QVector<AxisConfig> axisConfigs_;

    // 每个逻辑轴的报警/限位上次状态（用于边沿触发）
    QVector<bool> lastAlarm_;
    QVector<bool> lastLimitPos_;
    QVector<bool> lastLimitNeg_;
    // 每个逻辑轴的软限位撞限边沿（去重，避免轮询重复发信号）
    QVector<bool> lastSoftLimitHit_;

    // 每个逻辑轴的使能状态（先使能再运动的安全门禁唯一事实源）。
    // 仅 EnableAll() 手动置 true；DisableAll()/EmergencyStop()/热重连置 false。
    QVector<bool> axisEnabled_;

    // 每个逻辑轴的"忙"截止时间戳(ms，0=空闲)。Go/点动发出时更新为 now+估计到位时间，
    // PollTick 里到达后复位并发 axisMoveFinished。用于 UI 置灰 Go 按钮防连点。
    QVector<qint64> axisBusyUntilMs_;
    // 每个逻辑轴"忙"是否已广播（边沿去重）
    QVector<bool>   axisBusyNotified_;

    // 每个逻辑轴是否正在回零（HomeAxis 置位；回零完成/停止/急停/断使能复位）。
    // MoveAbs/MoveJog 据此拒绝回零中的运动请求（安全门禁，与 busy 分离避免挡点动 autoRepeat）。
    QVector<bool>   homingActive_;

    void MarkAxisBusy(LogicalAxis axis, int busyMs);
    void CheckAxisBusy();
};
