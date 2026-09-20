#pragma once

#include <QWidget>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QTableWidget>
#include <QPlainTextEdit>
#include <QImage>
#include <QTimer>
#include <QVector>

#include "ICamera.h"
#include "IPuffAlgorithm.h"

class QThread;
class FrameSaver;

// 视觉检测页（P4 重构，规格 `doc/vision_test_page_redesign_plan.md`）。
//
// 布局：左 320 px `QScrollArea` 控制栏（「相机控制」+「算法测试」两组）
//     + 右 2×2 四宫格（RGB / 深度伪彩 / Overlay / 数据+日志）。
// 与旧版的根本差别：旧版是**单画面 + 视图切换按钮**，一次只能看一路；
// 新版三格**同时显示**，右下格用 `QTabWidget` 放 11 列结果表与页面级日志。
class VisionTestPage : public QWidget
{
    Q_OBJECT

public:
    explicit VisionTestPage(QWidget* parent = nullptr);
    ~VisionTestPage() override;

    // 模式互锁（TR-075 → P3 决策 10 → P4 §6.2）。
    //
    // 自动运行期间本页是**生产观察通道**：会改变采集/检测行为的控件全部置灰
    // （相机配置、6 个流参数、算法选择、模板路径、**采集启停**）；
    // 检测 / 存图 / 识别框开关 / 三个画面等**只读**操作保持可用。
    //
    // ⚠ 采集启停由「可用」改为「置灰」是 P3 决策 10 的修正：流是全局共享资源，
    // 自动运行期间点「停止采集」会 release 掉 AutoRun 的持有者，**打断正在执行的
    // `ExecuteVision`**（拿不到帧）。启停权归持有者集合。
    //
    // **签名不可变**：`MainWindow.cpp` 在模式切换时调用（`SetParamsLocked(bool)`）。
    void SetParamsLocked(bool locked);

private slots:
    void OnRefreshDevices();
    void OnStartStream();
    void OnStopStream();
    void OnFrameReady(const CameraFrame& frame);
    void OnRenderTick();
    void OnSingleDetect();
    void OnToggleContinuous();
    void OnOverlayToggle();
    void OnBrowseTemplate();
    void OnSnapshot();
    void OnLoadImage();
    void OnOpenLogDir();
    void OnFpsTick();
    void OnImageSaved(const QString& path);
    void OnSaveError(const QString& message);
    void OnCameraStateChanged(CameraLinkState state);

private:
    void SetupUI();
    QWidget* CreateControlPanel();
    QWidget* CreateQuadArea();

    void UpdateCameraState();
    void UpdateButtons();
    void RenderTiles();
    QImage BuildRgbImage();
    QImage BuildDepthImage();
    QImage BuildOverlayImage();

    void RunDetection(const CameraFrame& frame, bool allowAutoSave);
    void ShowResults(const QVector<PuffResult>& results, double elapsedMs);
    // 三路存图入队（异步，走 FrameSaver 线程）。抽成函数是因为「单次检测自动存图」
    // 与「保存当前帧」两处都要按同一组开关各存一路。
    void QueueSaveRgb();
    void QueueSaveDepth();
    void QueueSaveOverlay();
    void SetStatus(const QString& text);
    void AppendLog(const QString& text);

    // 按**枚举结果**填充 6 个流参数下拉框（V1/V2 修正：不照抄需求里的预置项）。
    // 实现不支持枚举（`SimCamera`）时回落为「只显示 config 现值」。
    void ReloadStreamProfiles();
    // 从 `communication.camera.*` 回填控件（`showEvent` 也要调：设备配置页有同样的
    // 写入口，切回本页必须看到现值，否则下拉框会把配置悄悄覆盖掉）。
    void RefreshStreamParamsFromConfig();
    void showEvent(QShowEvent* event) override;

    static QVector<PuffResult> ToQVector(const std::vector<PuffResult>& v);

    // ---- 相机控制组 ----
    QComboBox*   cameraTypeCombo_  = nullptr;
    QComboBox*   deviceSnCombo_    = nullptr;
    QPushButton* refreshDevBtn_    = nullptr;
    QComboBox*   colorResCombo_    = nullptr;
    QComboBox*   colorFormatCombo_ = nullptr;
    QComboBox*   colorFpsCombo_    = nullptr;
    QComboBox*   depthResCombo_    = nullptr;
    QComboBox*   depthFormatCombo_ = nullptr;
    QComboBox*   depthFpsCombo_    = nullptr;
    QCheckBox*   d2cAlignCheck_    = nullptr;
    QLabel*      cameraStateDot_   = nullptr;
    QLabel*      cameraStateText_  = nullptr;
    QPushButton* startStreamBtn_   = nullptr;
    QPushButton* stopStreamBtn_    = nullptr;

    // ---- 算法测试组 ----
    QComboBox*   algoCombo_         = nullptr;
    QLineEdit*   templatePathEdit_  = nullptr;
    QPushButton* browseTemplateBtn_ = nullptr;
    QCheckBox*   saveRgbCheck_      = nullptr;
    QCheckBox*   saveDepthCheck_    = nullptr;
    QCheckBox*   saveOverlayCheck_  = nullptr;
    QPushButton* singleDetectBtn_   = nullptr;
    QPushButton* continuousBtn_     = nullptr;
    QPushButton* loadImgBtn_        = nullptr;
    QPushButton* saveFrameBtn_      = nullptr;

    // ---- 四宫格 ----
    QLabel*         rgbViewLabel_     = nullptr;
    QLabel*         rgbInfoLabel_     = nullptr;
    QLabel*         depthViewLabel_   = nullptr;
    QLabel*         depthInfoLabel_   = nullptr;
    QLabel*         overlayViewLabel_ = nullptr;
    QLabel*         overlayInfoLabel_ = nullptr;
    QCheckBox*      overlayToggleCheck_ = nullptr;
    QTableWidget*   resultTable_      = nullptr;
    QPlainTextEdit* logView_          = nullptr;

    QLabel*      statusLabel_ = nullptr;

    FrameSaver* frameSaver_  = nullptr;
    QThread*    saverThread_ = nullptr;

    CameraFrame         latestFrame_;
    QVector<PuffResult> lastResults_;
    double              lastDetectMs_ = 0.0;
    bool                hasFrame_     = false;
    bool                streaming_    = false;
    bool                showOverlay_  = true;
    bool                continuous_   = false;
    bool                locked_       = false;   // SetParamsLocked 状态（UpdateButtons 用）

    QTimer* fpsTimer_    = nullptr;
    QTimer* renderTimer_ = nullptr;
    int     frameCount_  = 0;
    double  fps_         = 0.0;
};
