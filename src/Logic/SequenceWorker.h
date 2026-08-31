#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <memory>

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
    explicit SequenceWorker(QObject* parent = nullptr);
    ~SequenceWorker() override;

    // 从 config 加载运动学/TCP/手眼参数到内部 Kinematics/CoordTransform。
    // 应在 RunSequence 之前调用（每次运行前刷新，保证与 ConfigPage 编辑一致）。
    void ReloadFromConfig();

    // 启动方案执行。若已在执行返回 false。线程安全（内部排队到 worker 线程执行）。
    bool RunSequence(const SchemeData& scheme);

    // 单独执行方案中某一条动作（含其全部点位），与 RunSequence 共用 running 门禁互斥。
    // 未使能/越界/运行中拒绝返回 false（未使能会发 errorOccurred）。线程安全。
    bool RunSingleAction(const SchemeData& scheme, int actionIndex);

    // 停止：取消当前动作并中断执行（安全停止，保持使能）。线程安全。
    void Stop();

    // 急停：置 cancel + HardwareManager::EmergencyStop（断使能）。线程安全。
    void EmergencyStop();

    // 单步模式：每执行完一个动作后挂起，等待 NextStep() 继续。
    void SetStepMode(bool enabled);
    bool NextStep();

    bool IsRunning() const;
    bool IsPaused() const;
    bool IsStepMode() const;

signals:
    void actionStarted(int index, const QString& name);
    void actionFinished(int index, const QString& name);
    void singleActionFinished(int index);
    void schemeFinished();
    void interrupted(const QString& reason);
    void errorOccurred(const QString& message);
    void logMessage(const QString& message);
    void stateChanged(const QString& state);

private slots:
    void StartExecution();          // worker 线程入口（QueuedConnection 调用）
    void StartSingleExecution(int index);   // 单动作执行入口（RunSingleAction 排队调用）

private:
    bool ExecuteActions();          // 逐动作执行主循环
    bool ExecuteAction(const ActionData& action, int index);
    bool ExecuteMove(const ActionData& action);
    bool ExecuteVision(const ActionData& action);
    bool ExecuteExtrude(const ActionData& action);
    bool ExecuteDelay(const ActionData& action);
    bool ExecuteGripper(const ActionData& action);

    bool MoveToPoint(const PointData& pt, double speedScale);
    bool WaitForAxes(const QVector<LogicalAxis>& axes, int timeoutMs);
    bool CancelSleep(int ms);
    bool WaitForStepGate();

    class Impl;
    std::unique_ptr<Impl> impl_;
};