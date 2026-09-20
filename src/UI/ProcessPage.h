#pragma once

#include <QWidget>
#include <QString>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QTableWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QLabel>
#include <QPointer>
#include <QHash>

class SequenceWorker;
class QPushButton;
// 方案树节点（定义在 Config/ProcessManager.h）。此处只用作指针返回类型，
// 故前置声明即可，避免 UI 头文件反向依赖 Config 的完整定义。
struct ActionData;

class ProcessPage : public QWidget
{
    Q_OBJECT

public:
    explicit ProcessPage(QWidget* parent = nullptr);

    void SetSequenceWorker(SequenceWorker* worker);

    // 单步会话是否活跃（m_stepActive）。MainWindow 模式互锁用：
    // 切自动时若有残留会话则调 AbortStepSession() 强制清理（Q12）
    bool IsStepSessionActive() const { return m_stepActive; }
    // 模式切换强制清理：停 worker + 清单步会话。仅确有残留时才有副作用
    void AbortStepSession();
    // 单步/自动执行进行中：此时禁止切换/删除方案（worker 跑的是启动时值拷贝的 scheme）
    bool IsExecutionActive() const;

private slots:
    void OnNewScheme();
    void OnDeleteScheme();
    void OnConfirmSwitch();
    void OnStepExecute();
    void OnRunSelectedAction();
    void OnNewAction();
    // P6：新建分组（组内子动作并发执行，见 doc/action_tree_concurrency_plan.md §八 第 5 项）
    void OnNewGroup();
    void OnEditAction();
    void OnDeleteAction();
    void OnAddPoint();
    void OnDeletePoint();
    void OnMoveUp();
    void OnMoveDown();
    void OnTeachRead();
    void OnSaveAction();
    void OnSchemeNameEdited();
    void OnMoveActionUp();
    void OnMoveActionDown();
    // 识别结果到达（P5）：worker 线程 Queued 投递，缓存后刷新视觉引导的只读坐标预览。
    void OnVisionResultReady(int actionId, double x, double y, double z);

private:
    void SetupUI();
    void ApplySpeedPercentToCurrentAction(int v);
    void ApplyGripperTargetToCurrentAction(double v);
    void RefreshGripperLimitRange();
    // 执行状态标签统一入口：text 已含状态符号，color 为前景色
    void SetStatusText(const QString& text, const QString& color);
    void RefreshSchemeCombo();

    // ---- P6：两级节点树（动作 / 分组）----
    // 重建节点树。**分组此时已展开**；选中按 `m_currentNodeId` 恢复。
    void RefreshNodeTree();
    // 按**节点编号**刷新右侧详情面板（动作 → 对应参数页；分组 → 分组页）。
    void RefreshNodeDetail(int nodeId);
    // 按编号在树控件里找条目（一级节点或分组子节点）。找不到返回 nullptr。
    QTreeWidgetItem* FindTreeItem(int nodeId) const;
    // 当前选中节点（可写）。未选中 / 编号已失效返回 nullptr。
    // ⚠ 一切编辑都经此取节点——**按编号定位，不按行号**（行号会随插入/删除/移动漂移）。
    ActionData* CurrentNode();
    // 编号对应的一级节点下标（-1 = 不是一级节点）
    int TopLevelIndex(int nodeId) const;

    // 视觉引导（P5）：
    // 重建「引用识别动作」下拉——只列**本方案内** type==Vision 的动作（决策 2：不允许跨方案引用）；
    // selectedId 在列表里找不到时**显式追加一条「#N（已不存在）」**，让悬垂引用可见而非静默指向别的动作。
    void RefreshGuideSourceCombo(int selectedId);
    // 按当前引用 + offsetZ + guideR + 识别结果缓存，刷新只读坐标预览与「保存会被拒绝」提示。
    void UpdateGuidePreview();

    int m_currentSchemeIdx = -1;
    // 当前选中节点的**持久化编号**（0 = 未选中）。P6 前这里是扁平行号 `m_currentActionIdx`——
    // 行号会随插入/删除/移动漂移，而编号不重排不回收，是引用（P5 `sourceActionId`）与 UI 之间
    // 唯一稳定的锚点。
    int m_currentNodeId = 0;

    QLineEdit* m_schemeNameEdit = nullptr;
    QComboBox* m_schemeCombo = nullptr;

    QTreeWidget* m_nodeTree = nullptr;

    QStackedWidget* m_detailStack = nullptr;
    QLabel* m_currentActionLabel = nullptr;
    QLabel* m_statusLabel = nullptr;

    QWidget* m_speedPercentRow = nullptr;
    QSpinBox* m_speedPercentSpin = nullptr;
    QSlider* m_speedPercentSlider = nullptr;

    QTableWidget* m_pointTable = nullptr;

    QComboBox* m_visionTypeCombo = nullptr;
    QLineEdit* m_exposureEdit = nullptr;
    QLineEdit* m_templateEdit = nullptr;
    QLineEdit* m_thresholdEdit = nullptr;

    QLineEdit* m_extrudeAmountEdit = nullptr;
    QLineEdit* m_extrudeSpeedEdit = nullptr;
    QLineEdit* m_suckBackAmountEdit = nullptr;
    QLineEdit* m_suckBackSpeedEdit = nullptr;

    QSpinBox* m_delaySpin = nullptr;

    // ---- 视觉引导（P5，doc/vision_guide_action_plan.md §5.1 / §5.5）----
    QComboBox* m_guideSourceCombo = nullptr;      // 引用的识别动作（userData = 该动作的 id）
    QDoubleSpinBox* m_guideOffsetZSpin = nullptr; // 「目标上方」Z 偏移（末端高度，正值向上）
    QDoubleSpinBox* m_guideRSpin = nullptr;       // 构造路径点的轴4(R) 固定值
    QLabel* m_guidePreviewLabel = nullptr;        // 两组坐标只读预览（不可编辑）

    // 识别结果缓存：key = 识别动作编号。由 SequenceWorker::visionResultReady 填充。
    // 坐标**不进 process.json**（决策 5）——这里是纯 UI 侧内存副本，程序退出即丢。
    struct VisionSnapshot { double x = 0.0; double y = 0.0; double z = 0.0; bool valid = false; };
    QHash<int, VisionSnapshot> m_visionSnapshots;

    // ---- 分组（P6，doc/action_tree_concurrency_plan.md §八 第 8 项）----
    // 分组页只展示编号 / 名称 / 子动作清单 / 组内并发校验结果（分组没有动作参数）。
    QLabel* m_groupInfoLabel = nullptr;

    QComboBox* m_gripperCombo = nullptr;
    QDoubleSpinBox* m_gripperTargetSpin = nullptr;
    QLabel* m_gripperUnitLabel = nullptr;
    QLabel* m_gripperHintLabel = nullptr;
    // 按方向各记一份最近行程值（mm）：切换下拉时恢复用户自己的数，不覆盖已填值
    double m_lastOpenTarget = -3.0;
    double m_lastCloseTarget = 0.0;

    QPointer<SequenceWorker> m_worker;   // TR-084：观察者持弱引用，防 worker 析构后悬垂
    QPushButton* m_runSelectedBtn = nullptr;
    QPushButton* m_stepBtn = nullptr;
    bool m_stepActive = false;
    // 会话终态（完成/已停止/出错）：StartExecution 在 emit 终态信号后还会补发 stateChanged("空闲")，
    // 不做保留的话终态会被紧随其后的「空闲」覆盖。新会话启动时清空。
    QString m_finalStatus;
    QString m_finalColor;
    void ResetStepSession();
};
