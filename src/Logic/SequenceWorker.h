#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QElapsedTimer>
#include <memory>
#include <functional>
#include <vector>

#include "ProcessManager.h"
#include "HAL/core/AxisMap.h"
#include "HAL/interfaces/IPuffAlgorithm.h"   // PuffResult（检测结果通道，S6 决策 6）

class Kinematics;
class CoordTransform;
class QEventLoop;

// ============================================================
// 顺序执行引擎（SequenceWorker）
// 大脑核心：按工艺方案（SchemeData）逐节点执行，通过 HardwareManager 门面
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
//
// 节点模型（P6，doc/action_tree_concurrency_plan.md）：方案的一级列表是**两级树**——
// 节点分「动作」与「分组」（分组不能嵌套）。分组的子节点**并发执行**：调度器在 worker
// 线程内单线程轮询推进各子状态机，全部完成即汇合（见 §五 单线程协作式调度）。
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

    // 检测结果载荷（P3 S6 决策 6，2026-09-23 落地）。
    //
    // **存在的理由**：`AutoRunPage` 原先订阅全局 `frameReady`（原始视频流）来刷预览，
    // 但它只在「方案运行中」登记相机持有（`AcquireCamera("AutoRun")`）——**未运行时只消费不持有**，
    // 于是视觉页点「停止采集」把流停掉后，自动运行页的画面会**定格**（用例 11.26 实测）。
    // 改订阅本通道后，自动运行页只在「识别动作真的跑过」时拿到图，**不再拉原始流**，
    // 该不对称从结构上消失（`doc/camera_integration_plan.md` 决策 6）。
    //
    // **载荷取「帧 + 结果」而非预渲染位图**：渲染责任留给消费者，从而保住 P3-4 的
    // 「本页不可见时不产出位图」优化——若在 worker 线程预渲染，页面切走后仍会白做
    // 6.2 MB 转换 + 全分辨率叠加绘制。
    struct DetectionResult
    {
        CameraFramePtr      frame;       // 本次检测所用的帧（共享只读，消费者自行渲染）
        QVector<PuffResult> results;     // 图像空间结果（框 / 置信度），可能为空
        double              threshold = 0.0;   // 本次检测的置信度阈值（渲染叠加图用）
    };

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
        int  retryCount      = 0;              // 0 = 不重试（默认/今天行为）；N = 节点级重试 N 次
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
    // 走既有 RunSequence → ExecuteAction——复用暂停/停止/门禁，并与自动运行天然互斥。
    // 未启用（safePos.enabled=false，默认）返回 false。
    bool RunSafePos();
    bool IsSafePosSession() const;   // 安全位会话进行中（UI 据此屏蔽临时方案的联动）

    // 单独执行方案中某一个节点（动作含其全部点位；分组则并发执行其全部子节点）。
    // 与 RunSequence 共用 running 门禁互斥。**按节点编号定位**（P6：不再是扁平行号）。
    // 未使能/编号不存在/运行中拒绝返回 false（未使能会发 errorOccurred）。线程安全。
    bool RunSingleAction(const SchemeData& scheme, int nodeId);

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

    // 单步模式：每执行完一个**一级节点**后挂起，等待 NextStep() 继续。
    // P6：分组的「一步」= 整个分组（组内一律不调用 WaitForStep，否则 N 个子状态机
    // 会争抢同一个 stepGo 原子量，放行顺序不确定）。
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
    // 一级节点开始/结束。**`nodeId` 是持久化节点编号**（P6 起，不再是扁平行号）——
    // 分组与动作共用本信号，由接收方按编号在树里定位。
    void actionStarted(int nodeId, const QString& name);
    void actionFinished(int nodeId, const QString& name);
    void singleActionFinished(int nodeId);
    // 分组并发调度（P6）：与 actionStarted/actionFinished 的区别是**粒度**——后者对每个
    // 一级节点（含分组）都发一次，前者只在「组内并发调度」这一层发，供 UI 展示组内进度。
    void groupStarted(int nodeId, const QString& name);
    void groupFinished(int nodeId, const QString& name);
    void schemeFinished();
    // 循环进度：每轮开始时发一次。index 从 1 开始；total < 0 表示无限循环。
    // schemeFinished 仍只在「全部循环完成」时发一次（见方案 §八）。
    void cycleChanged(int index, int total);
    void interrupted(const QString& reason);
    void errorOccurred(const QString& message);
    void logMessage(const QString& message);
    // 识别结果（P5）：识别分支末尾发出，携带**该识别动作的编号**与基座坐标（mm）。
    // 用途只有一个——`ProcessPage` 的视觉引导编辑区据此**只读预览**两组路径点（§5.5 决策 1）。
    // 坐标**不进 `process.json`**（决策 5）：这里是内存广播，会话结束即失效。
    // 跨线程 Queued 传递，参数只用 POD，无需注册元类型。
    void visionResultReady(int actionId, double x, double y, double z);
    // 检测结果通道（P3 S6 决策 6）：每次「识别」动作跑完即发一次，携带**该次用的帧 + 图像空间结果**。
    // 消费方 = `AutoRunPage`（画「识别结果叠加图」）；**它不再订阅 `frameReady`**。
    // 跨线程 Queued 传递，故需 `qRegisterMetaType`（见 `SequenceWorker.cpp` 文件级静态初始化）。
    void detectionReady(const SequenceWorker::DetectionResult& result);
    // 状态迁移（带挂起原因快照）。Queued 连接下带参比"查询当前值"更准，不会错位。
    void stateChanged(SequenceWorker::WorkerState state, SequenceWorker::PauseReason reason);

private slots:
    void StartExecution();          // worker 线程入口（QueuedConnection 调用）
    void StartSingleExecution(int nodeId);   // 单节点执行入口（RunSingleAction 排队调用）

private:
    bool ExecuteActions();          // 【循环外壳】按 LoopConfig 重复 ExecuteOnce
    bool ExecuteOnce();             // 单轮：逐一级节点执行
    // 节点级重试包装（内部转调 ExecuteNode）。retryCount=0 时**直通**，零额外行为。
    // 分组按 §Q3 **重试整个分组**（部分重试会破坏组内资源独占：一个子动作已走完、
    // 另一个重试 → 状态错配）。本函数**不 emit errorOccurred**——失败原因留在
    // Impl::lastError，由会话级单点发出（D17 方案 A）。
    bool ExecuteNodeWithRetry(const ActionData& node, int index);
    // 节点分派（P6 §九 第 1 项）：按 `nodeType` 分派到 ExecuteAction（动作）或 ExecuteGroup（分组）。
    bool ExecuteNode(const ActionData& node, int index);
    // 动作节点 = 单元素调度器（原 ExecuteAction 分派 + 阻塞式动作体，P6 改为状态机驱动）。
    // **本函数及其子树一律不 emit errorOccurred**（D17 方案 A）：失败原因写入 Impl::lastError。
    bool ExecuteAction(const ActionData& action, int index);
    // 分组节点（P6 §九 第 3 项）：为每个子节点建一个子状态机，单线程协作式调度至全部完成。
    // 失败归因 = **首个失败者**（其余仅记日志），原因留在 lastError（§九 第 4 项）。
    bool ExecuteGroup(const ActionData& group, int index);

    // ---- P6 第三步：单线程协作式调度的子状态机 ----
    // 一个 RunnerState = 一个「可步进」的节点执行器：原阻塞式动作体拆成
    // Begin（解析目标）→ Dispatch（下发）→ Wait（等待到位）→ 下一段，StepRunner 推进一次。
    // 收益来源是**等待重叠**（下发互不阻塞，等待统一汇合），不是计算并行（§5.1）。
    struct RunnerState {
        enum Phase { PhaseBegin = 0, PhaseDispatch = 1, PhaseWait = 2, PhaseDone = 3 };

        ActionData action;                 // 值拷贝：会话内不变（避免与主线程编辑竞争）
        int  segIdx = 0;                   // 当前段号（Move=点序号 / Extrude=0挤出,1回抽 / 其余=0）
        int  phase  = PhaseBegin;
        bool failed = false;
        QString failReason;

        // 当前段解析出的下发参数
        double j1 = 0.0, j2 = 0.0, jz = 0.0, jr = 0.0;   // Move 段：关节目标
        double speedScale = 1.0;
        double segTarget  = 0.0;           // 单轴动作（挤出/夹爪）的绝对目标
        double segSpeed   = 0.0;           // 单轴动作的速度
        QVector<LogicalAxis> axes;         // 当前段等待的轴
        int    timeoutMs = 0;
        QElapsedTimer waitTimer;

        // Delay（含"无相机视觉"的模拟延时）：暂停时长不计入延时预算。
        // 调度器在每次恢复时把本次挂起时长累加进 pauseAccumMs（对**每个**子状态机都加，
        // 故不必区分"谁在暂停期间还没开始计时"）。
        QElapsedTimer delayTimer;
        bool delayStarted = false;
        long long pauseAccumMs = 0;

        // VisionGuide：基座坐标只在首个 Begin 查一次会话结果表
        bool   baseResolved = false;
        double baseX = 0.0, baseY = 0.0, baseZ = 0.0;
        bool   baseOk = false;

        bool done() const { return phase == PhaseDone; }
    };

    // 推进一个子状态机一次。返回 false = 该子状态机失败（原因已写入 r.failReason）。
    // **本函数不阻塞、不处理暂停/停止**——那些由调度器统一处理（整组减速停）。
    bool StepRunner(RunnerState& r);

    // 段级三小步（StepRunner 的组成部分，拆开只为可读）：
    //   Prepare  = Begin：解析当前段目标（IK / 软限位 / 日志），失败即写 failReason
    //   Dispatch = 下发当前段（非阻塞：MoveAbs 立即返回，到位靠 IsAxisBusy 轮询）
    //   Advance  = 段完成 → segIdx+1；返回 true = 还有下一段
    bool PrepareRunnerSegment(RunnerState& r);
    bool DispatchRunnerSegment(RunnerState& r);
    bool AdvanceRunnerSegment(RunnerState& r);
    // 恢复续走：重发**当前段**的绝对目标（不是重跑整节点——多点点位重发已完成点会跳分支，
    // 挤出重跑会把轴反向补回满量程再回抽，等于多挤一份料）。**须在 InMainThread 内调用。**
    void ReissueRunnerSegment(RunnerState& r);
    // 把点位解析为关节目标（示教 joints + FK 一致性校验，否则 IK）。逻辑与改造前
    // MoveToPoint 的前半段逐行一致，只是结果落到 RunnerState 而非局部变量。
    bool ResolveRunnerJoints(RunnerState& r, const PointData& pt);

    // 单线程协作式调度器（§5.2）：持 N 个子状态机，轮询推进，全部 done 即汇合。
    // 暂停 / 停止 / 急停在此统一处理：组内任一子状态机所在轴参与运动 →
    // **整组减速停** → 汇合后统一挂起（§九 第 6 项）；cancel 则等待所有子动作
    // 安全停止后再返回（§Q2，禁止立即返回）。
    bool RunScheduler(std::vector<RunnerState>& runners);

    // 重试等待：与 Delay 同构（for(;;) + PauseGate），暂停/停止/急停均生效，
    // 暂停时长不计入间隔预算。**不可用 Impl::WaitForCancelOrTime**（只处理 cancel，不处理 paused）。
    bool WaitRetryDelay(int ms);

    // 构造「回安全位」动作：点1 = 原地抬 Z（J1/J2/R 保持当前值），点2 = 水平走安全位。
    // j1/j2/z/r = **安全位**关节角（由调用方给定：RunSafePos 传 config 现值，
    // 循环间隔传启动快照）——worker 线程绝不读 config。当前位在调用时实时读（主线程直通）。
    // 返回 false = 安全位未就绪（未启用/未捕获）→ 调用方判为故障，不静默跳过。
    bool BuildSafePosActionFrom(ActionData& act, double j1, double j2, double z, double r);

    // 循环间隔内联回安全位：三不——不置 safeSession、不发 actionStarted/actionFinished、
    // 不重入门禁（复用动作执行链 → 白得 cancel 检查 / PauseGate / reissue 续走 / 逐点日志）。
    bool ReturnToSafePosInline();

    // 挂起门闩：返回 false = 被 cancel 终止（调用方应中断动作）。
    // axes 非空时先减速停这些轴；reissue 非空时在 Resume 后重发绝对目标。
    // P6 起只用于**节点边界**（轮边界、重试间隔）与单元素调度器——组内并发由
    // RunScheduler 统一处理（整组减速停），不再逐子动作调用。
    bool PauseGate(const QVector<LogicalAxis>& axes = {},
                   std::function<bool()> reissue = nullptr);

    // 状态迁移唯一出口（收敛原先 3 处重复的"清 running + emit 空闲"）
    void SetState(WorkerState s, PauseReason r = PauseReason::None);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

// `detectionReady` 是跨线程 Queued 连接，实参需元类型注册——
// `DetectionResult` 含 `QVector<PuffResult>`（非 Qt 内建），不注册会在运行时报
// "Cannot queue arguments of type 'SequenceWorker::DetectionResult'"。
// 注册动作在 `SequenceWorker.cpp` 的文件级静态初始化里（与 `CameraCaptureWorker.cpp` 同款做法）。
Q_DECLARE_METATYPE(SequenceWorker::DetectionResult)
