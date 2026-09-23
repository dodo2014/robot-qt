#pragma once

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QTextEdit>
#include <QVector>
#include <QPointer>
#include "HAL/interfaces/ICamera.h"
#include "HAL/interfaces/IMotionCard.h"
#include "HAL/interfaces/IAxisServo.h"
#include "Core/Kinematics.h"
#include "KinematicsHelper.h"
#include "Logic/SequenceWorker.h"   // 槽签名用 WorkerState/PauseReason（Q_ENUM 元类型）

class SequenceWorker;

class AutoRunPage : public QWidget
{
    Q_OBJECT

public:
    explicit AutoRunPage(QWidget* parent = nullptr);

    void SetSequenceWorker(SequenceWorker* worker);

    // 模式互锁入口（下一步 TR-073 接入；本次恒 true，行为不变）
    void SetAutoModeActive(bool active);

    // 预览源开关（P3 S6 决策 6 + 2026-09-23 用户裁定）。
    //
    // **默认关** = 本页显示「检测结果」：由 `SequenceWorker::detectionReady` 驱动，
    // **不订阅原始帧流** —— 从结构上消除用例 11.26 的「消费 ≠ 持有」不对称
    // （原先本页构造期就订阅 `frameReady`，却只在方案运行中登记持有相机，
    //  于是视觉页一停流，本页画面就定格）。
    //
    // **开** = 回到「原始视频流 + 可见性持有」：本页可见期间登记持有相机（消费 ⟺ 持有），
    // 隐藏时释放。这条路径**保留**（用户明确要求不删），现场排障看实时画面时可用。
    //
    // **调用方（TR-113，2026-09-23）**：配置页「视觉与工艺参数」页签的开关 →
    // `ConfigPage::autoRunPreviewSourceChanged`；`MainWindow` 启动时按
    // `ui.autoRunStreamPreview` 再应用一次。**不要新增调用方**——本函数是预览源的唯一
    // 切换入口，且只在值真变化时才动作（相等即早退，故启动期不产生提示噪音）。
    void SetStreamPreviewEnabled(bool enabled);
    bool IsStreamPreviewEnabled() const { return m_streamPreviewOn; }

signals:
    // 全局消息横幅（2026-09-15）：本页不再自持底部提示控件（10 寸触控屏竖向空间紧张），
    // SetHint 统一转发至 MainWindow 顶栏横幅（Elide 截断 + 点击弹窗看全文）。
    // 手动控制页的提示保留原地不动——横幅只承载本页提示，单一信号源，不引入 TR-087 竞争。
    void requestGlobalHint(const QString& text, const QString& color);

private slots:
    // 5 按钮（2026-09-07 语义重构：暂停=减速停保持上下文；停止=立即减速停废弃上下文；
    // 复位→清除报警。原「复位」实为一键回零，已归手动控制页）
    void OnStartOrResumeClicked();
    void OnPauseClicked();
    void OnStopClicked();
    void OnAlarmClearClicked();
    void OnInitClicked();
    void OnEmergencyTriggered();   // 响应全局急停信号（硬件/worker 中断在 MainWindow 触发点完成）
    void OnWorkerStateChanged(SequenceWorker::WorkerState state,
                              SequenceWorker::PauseReason reason);

    // 检测结果通道（S6 决策 6）：每次「识别」动作跑完来一次（**含未检出目标的空结果**）。
    void OnDetectionReady(const SequenceWorker::DetectionResult& result);
    // 原始帧流。**只有 `SetStreamPreviewEnabled(true)` 时才订阅**（见 SetupStreamPreview）。
    void OnFrameReady(const CameraFramePtr& frame);
    void OnLogMessage(const QString& msg);
    void OnStateUpdated(const QVector<MotorStatus>& axes);
    void OnServoStateUpdated(const QVector<ServoTelemetry>& servos);
    void OnActionStarted(int index, const QString& name);
    void OnSchemeFinished();
    void OnInterrupted(const QString& reason);
    void OnError(const QString& message);
    // 循环进度（TR-091）：每轮开始发一次；index 从 1 起，total < 0 = 无限循环
    void OnCycleChanged(int index, int total);

private:
    void SetupUI();
    void RefreshCoordPanel();
    void RefreshSchemeCombo();
    // 相机两格的位图刷新（P3-4，2026-09-22）。抽成函数是因为两个入口共用同一套：
    // ① 每帧（**仅本页可见时**）；② `showEvent` 切回本页时补绘。
    // ⚠ 2026-09-23 起本函数属于**「视频流」预览路径**（仅 `m_streamPreviewOn == true` 时被调用）；
    // 默认路径是 `RenderDetectionTiles()`（检测结果，S6 决策 6）。
    void RenderCameraTiles();
    // 检测结果两格（S6 决策 6）：左格 = 该次检测所用的**原始帧**，右格 = 同帧 + 检测框。
    // 两格数据都来自 `m_lastDetection`，**不碰实时流**。
    void RenderDetectionTiles();
    // 「视频流 + 可见性持有」独立函数对（保留能力，见 SetStreamPreviewEnabled 的说明）。
    // Setup 订阅 `frameReady` 并在本页可见时 acquire；Teardown 反订阅并 release。
    void SetupStreamPreview();
    void TeardownStreamPreview();
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

    // 循环生产控件（TR-091）：读 config 初始化 / 写回 config / 组装 LoopConfig
    void LoadLoopConfigFromConfig();
    void PersistLoopConfig();
    SequenceWorker::LoopConfig BuildLoopConfig() const;
    void ResetCycleLabel();   // 启动前复位进度文案（单轮 / 循环 0/N / 循环 0/∞）

    // 按钮启用的唯一出口（杜绝多处 setEnabled 互相覆盖绕过互锁）
    void UpdateControlsEnabled();
    void SetHint(const QString& text, const QString& color);
    void UpdateStatusByState(SequenceWorker::WorkerState state, SequenceWorker::PauseReason reason);
    void RequestHomeAll();   // 一键回零（不占按钮位，供后续物理按钮/MainWindow 转接）

    QLabel*     m_statusLabel      = nullptr;
    QLabel*     m_coordPanel       = nullptr;
    QComboBox*  m_schemeCombo      = nullptr;
    QTextEdit*  m_logTextEdit      = nullptr;
    QLabel*     m_cameraRgbLabel   = nullptr;
    QLabel*     m_cameraOverlayLabel = nullptr;
    // P3-4（2026-09-22）：本页不可见时只缓存帧、**不产出位图**。
    // ⚠ 2026-09-23（S6 决策 6）起 `frameReady` **不再是构造期订阅**——只有
    // `SetStreamPreviewEnabled(true)` 经 `SetupStreamPreview()` 才建立（默认关闭，
    // 见本文件 :33-43）；本页默认路径不订阅原始帧流，`m_lastFrame` 因此可能长期为空。
    // 【改造前】它由 `MainWindow` 启动即 `new AutoRunPage()` 的构造期建立，页面切走后 Qt
    // 只会跳过「重绘」、不会跳过「转换」→ 仍以 15 fps 在 UI 线程做 `ColorToQImage`
    // （6.2 MB 拷贝）+ 全分辨率 `QPixmap::fromImage`，与用户正在看的页面抢线程。
    // 「不可见只缓存、不产位图」这条策略两条路径都保留；切回本页时由 `showEvent` 补一帧。
    CameraFramePtr m_lastFrame;
    // 最近一次检测结果（S6 决策 6）。与 `m_lastFrame` 同款策略：本页不可见时只缓存、不产位图，
    // 切回本页时由 `showEvent` 补绘。**「未运行」时本页就停在上一轮的结果图上**（2026-09-23 用户裁定）。
    SequenceWorker::DetectionResult m_lastDetection;
    // 预览源：false = 检测结果（默认）；true = 原始视频流 + 可见性持有。
    bool m_streamPreviewOn = false;
    // 2026-09-15：底部 m_hintLabel 已删除——提示改由 MainWindow 顶栏全局横幅承载
    // （SetHint 转发 requestGlobalHint），释放本页竖向空间。

    // 循环生产控件（TR-091，一行排布；布局待真机效果后定）
    QComboBox*  m_loopModeCombo    = nullptr;   // 单轮 / 指定次数 / 无限循环
    QSpinBox*   m_loopCountSpin    = nullptr;   // 1..9999，仅「指定次数」可用
    QCheckBox*  m_loopSafePosCheck = nullptr;   // 「回安全位」（循环间隔）
    QLabel*     m_cycleLabel       = nullptr;   // 进度：单轮 / 循环 3/10 / 循环 3/∞

    QPointer<SequenceWorker> m_worker;   // TR-084：观察者持弱引用，防 worker 析构后悬垂
    bool m_safePosSessionActive = false;   // TR-088：actionStarted 时记录的安全位临时会话标志（完成槽不能直查 safeSession——已被清）

    // 5 按钮（下标分派，不再靠 text.contains 匹配——改文案会静默错配）
    QPushButton*    m_btnStartOrResume = nullptr;
    QPushButton*    m_btnPause         = nullptr;
    QPushButton*    m_btnStop          = nullptr;
    QPushButton*    m_btnAlarmClear    = nullptr;
    QPushButton*    m_btnInit          = nullptr;

    // 互锁状态位（TR-075 已接入）：
    // m_autoModeActive 由 MainWindow 顶栏【手动/自动】开关下发（SetAutoModeActive）；
    // 急停锁存不放本页——全局硬件级状态由 HardwareManager 自持，gate 直读 IsEStopPending()
    bool m_autoModeActive = true;
    QString m_lastError;             // OnError 保存，供清报警提示（避免跨线程读 worker）

    // 关节位置缓存（避免每 50ms 实时读舵机串口，走 stateUpdated/servoStateUpdated 缓存）
    double m_j1 = 0.0, m_j2 = 0.0, m_z = 0.0, m_r = 0.0;
    Kinematics m_kin;
    bool   m_kinParamsLoaded = false;
    double m_kinL1 = 0.0, m_kinL2 = 0.0, m_kinZ0 = 0.0, m_kinH1 = 0.0;
    double m_kinTcpX = 0.0, m_kinTcpY = 0.0, m_kinTcpZ = 0.0;
    QString m_lastCoordText;
};