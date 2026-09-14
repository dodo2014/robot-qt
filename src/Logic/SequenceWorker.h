#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <memory>
#include <functional>

#include "ProcessManager.h"
#include "HAL/core/AxisMap.h"

class Kinematics;
class CoordTransform;
class QEventLoop;

// ============================================================
// 顺序执行引擎（SequenceWorker）
// 大脑核心：按工艺方案（SchemeData）逐动作执行，通过 HardwareManager 门面
// 驱动硬件（物理单位 mm/度），Motion 动作经 Kinematics 逆解后逐点移动。
//
// 线程模型：QObject，可 moveToThread 到独立 QThread；执行循环在 worker 线程，
// 通过信号与 UI 通信（actionStarted/actionFinished/schemeFinished/...）。
// 严禁在 worker 内直接操作 UI 控件（跨线程崩溃 0xc0000005）。
//
// 中断模型：Stop()/EmergencyStop() 置 cancel 标志，当前等待循环（到位/延时/单步）
// 立即退出，动作安全停止后跳出执行循环。
//
// 使能门禁：RunSequence 入口检查 HardwareManager::IsGlobalEnabled()，
// 未使能拒绝执行并发出 errorOccurred。
// ============================================================
class SequenceWorker : public QObject
{
    Q_OBJECT

public:
    // 显式状态机（替代原先 running / stepMode 等布尔量的隐式组合）
    // Q_ENUM 必需：stateChanged 跨线程 Queued 传递需元类型注册，
    // 否则运行时报 "Cannot queue arguments of type 'WorkerState'"。
    enum class WorkerState { Idle, Running, Paused, Fault };
    Q_ENUM(WorkerState)

    // 挂起原因：与 WorkerState::Paused 正交，用于区分「用户暂停」与「单步等待放行」。
    // 单步等待时 running 本就为 true（历史语义），故不设独立 StepPaused 状态。
    enum class PauseReason { None, User, Step };
    Q_ENUM(PauseReason)

    explicit SequenceWorker(QObject* parent = nullptr);
    ~SequenceWorker() override;

    // 循环生产配置（值语义，2026-09-14 新增）。
    // 主线程组装 → 随 RunSequence 进 worker 线程；worker 线程只读副本，
    // 绝不跨线程读 ConfigManager（json 读写非线程安全）。
    // 默认 Single/1 = 与历史行为逐字节一致（既有调用点零改动）。
    struct LoopConfig
    {
        enum class Mode { Single, Count, Infinite };
        Mode mode            = Mode::Single;   // 单轮 / 指定 N 次 / 无限循环
        int  count           = 1;              // 仅 Mode::Count 生效，>= 1
        bool returnToSafePos = true;           // 进入下一轮前先回安全位（防跨轮横扫）
        int  retryCount      = 0;              // 0 = 不重试（默认/今天行为）；N = 动作级重试 N 次
        int  retryDelayMs    = 500;            // 重试前等待（等物料稳定）
    };

    // 从 config 加载运动学/TCP/手眼参数到内部 Kinematics/CoordTransform。
    // 应在 RunSequence 之前调用（每次运行前刷新，保证与 ConfigPage 编辑一致）。
    void ReloadFromConfig();

    // 启动方案执行。若已在执行返回 false。线程安全（内部排队到 worker 线程执行）。
    // loop 为默认值时等价于历史单轮语义（AutoRunPage/ProcessPage/RunSafePos 零改动）。
    bool RunSequence(const SchemeData& scheme, const LoopConfig& loop = LoopConfig{});

    // 回安全位（2026-09-07）：读 config 的 kinematics.safePos（关节角 j1/j2/z/r），
    // 构造临时方案（1 个 Move 动作、2 个 hasJoints 点：点1=原地抬 Z、点2=水平走安全位）
    // 走既有 RunSequence → ExecuteMove——复用暂停/停止/门禁，并与自动运行天然互斥。
    // 未启用（safePos.enabled=false，默认）返回 false。
    bool RunSafePos();
    bool IsSafePosSession() const;   // 安全位会话进行中（UI 据此屏蔽临时方案的联动）

    // 单独执行方案中某一条动作（含其全部点位），与 RunSequence 共用 running 门禁互斥。
    // 未使能/越界/运行中拒绝返回 false（未使能会发 errorOccurred）。线程安全。
    bool RunSingleAction(const SchemeData& scheme, int actionIndex);

    // 停止（就近）：仅置 cancel，当前动作执行完才退出。线程安全。
    // 单步调试与程序退出（ShutdownWorker）用这条——保持历史语义（TR-063 实测）。
    void Stop();

    // 立即停止（自动运行页【停止】）：cancel + 全轴减速停（MC_Stop 斜坡，保持使能），
    // 然后废弃上下文退回就绪态。与 Stop() 的区别是**下发硬件减速停**，不等当前动作跑完。
    // 注：绝不能"断脉冲/断使能"式急停——开环步进会因惯性滑行丢步，坐标系错乱。
    void StopImmediate();

    // 急停：置 cancel + HardwareManager::EmergencyStop（断使能）。线程安全。
    void EmergencyStop();

    // 暂停：Running→Paused。协作式：在最近挂起点（≤20ms）减速停当前轴后挂起，
    // 保持上下文（scheme / currentIndex 不变），Resume() 后从半路续走当前绝对目标。
    // 单步等待放行（stepGatePending）时返回 false——此时应点「继续/单步」而非暂停。
    bool Pause();

    // 继续：Paused→Running。单步等待放行时等价于放行下一步（转调 stepGo）。
    bool Resume();

    // 清除故障锁存：Fault→Idle。仅清引擎锁存，**不动硬件使能**——
    // 卡轴清报警实际靠手动页「断使能 + 重新使能」，而"程序不自动使能"是既有安全原则。
    void ClearFault();

    // 单步模式：每执行完一个动作后挂起，等待 NextStep() 继续。
    void SetStepMode(bool enabled);
    bool NextStep();

    // IsRunning 语义保持「会话持有中」= state != Idle：单步暂停时亦为 true，
    // 与历史行为一致（MainWindow::IsExecutionActive / ProcessPage 零回归）。
    bool IsRunning() const;
    bool IsPaused() const;
    bool IsStepMode() const;

    WorkerState GetState() const;
    PauseReason GetPauseReason() const;
    bool IsSingleActionSession() const;   // 单动作会话（原「执行选中动作」状态）
    bool IsStepGatePending() const;       // 单步等待放行中（UI 判定能否点「下一步」）

signals:
    void actionStarted(int index, const QString& name);
    void actionFinished(int index, const QString& name);
    void singleActionFinished(int index);
    void schemeFinished();
    // 循环进度：每轮开始时发一次。index 从 1 开始；total < 0 表示无限循环。
    // schemeFinished 仍只在「全部循环完成」时发一次（见方案 §八）。
    void cycleChanged(int index, int total);
    void interrupted(const QString& reason);
    void errorOccurred(const QString& message);
    void logMessage(const QString& message);
    // 状态迁移（带挂起原因快照）。Queued 连接下带参比"查询当前值"更准，不会错位。
    void stateChanged(SequenceWorker::WorkerState state, SequenceWorker::PauseReason reason);

private slots:
    void StartExecution();          // worker 线程入口（QueuedConnection 调用）
    void StartSingleExecution(int index);   // 单动作执行入口（RunSingleAction 排队调用）

private:
    bool ExecuteActions();          // 【循环外壳】按 LoopConfig 重复 ExecuteOnce
    bool ExecuteOnce();             // 单轮：逐动作执行（原 ExecuteActions 主体，原样搬迁）
    bool ExecuteAction(const ActionData& action, int index);
    // 动作级重试包装（内部转调 ExecuteAction）。retryCount=0 时**直通**，零额外行为。
    // 本函数**不 emit errorOccurred**——失败原因留在 Impl::lastError，由会话级单点发出（D17 方案 A）。
    bool ExecuteActionWithRetry(const ActionData& action, int index);
    bool ExecuteMove(const ActionData& action);
    bool ExecuteVision(const ActionData& action);
    bool ExecuteExtrude(const ActionData& action);
    bool ExecuteDelay(const ActionData& action);
    bool ExecuteGripper(const ActionData& action);

    // 重试等待：与 ExecuteDelay 同构（for(;;) + PauseGate），暂停/停止/急停均生效，
    // 暂停时长不计入间隔预算。**不可用 Impl::WaitForCancelOrTime**（只处理 cancel，不处理 paused）。
    bool WaitRetryDelay(int ms);

    // 构造「回安全位」动作：点1 = 原地抬 Z（J1/J2/R 保持当前值），点2 = 水平走安全位。
    // j1/j2/z/r = **安全位**关节角（由调用方给定：RunSafePos 传 config 现值，
    // 循环间隔传启动快照）——worker 线程绝不读 config。当前位在调用时实时读（主线程直通）。
    // 返回 false = 安全位未就绪（未启用/未捕获）→ 调用方判为故障，不静默跳过。
    bool BuildSafePosActionFrom(ActionData& act, double j1, double j2, double z, double r);

    // 循环间隔内联回安全位：三不——不置 safeSession、不发 actionStarted/actionFinished、
    // 不重入门禁（复用 ExecuteMove → 白得 cancel 检查 / PauseGate / reissue 续走 / 逐点日志）。
    bool ReturnToSafePosInline();

    bool MoveToPoint(const PointData& pt, double speedScale);

    // 等待轴到位；期间若请求暂停 → PauseGate(axes, reissue)：减速停 → 挂起 →
    // Resume 后调用 reissue 重发**当前那条**绝对目标 MoveAbs 从半路续走。
    // reissue 为空表示不支持续走（仅挂起，如动作边界）。
    bool WaitForAxes(const QVector<LogicalAxis>& axes, int timeoutMs,
                     std::function<bool()> reissue = nullptr);

    // 挂起门闩：返回 false = 被 cancel 终止（调用方应中断动作）。
    // axes 非空时先减速停这些轴；reissue 非空时在 Resume 后重发绝对目标。
    bool PauseGate(const QVector<LogicalAxis>& axes = {},
                   std::function<bool()> reissue = nullptr);

    // 状态迁移唯一出口（收敛原先 3 处重复的"清 running + emit 空闲"）
    void SetState(WorkerState s, PauseReason r = PauseReason::None);

    class Impl;
    std::unique_ptr<Impl> impl_;
};