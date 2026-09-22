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
    // 识别动作**（拿不到帧）。启停权归持有者集合。
    //
    // **签名不可变**：`MainWindow.cpp` 在模式切换时调用（`SetParamsLocked(bool)`）。
    void SetParamsLocked(bool locked);

private slots:
    void OnRefreshDevices();
    void OnStartStream();
    void OnStopStream();
    void OnFrameReady(const CameraFramePtr& frame);
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
    // 三格位图重绘（**昂贵路径**）。`force=false` 时按「帧时间戳 + 三格目标尺寸」去重——
    // 相机 15 fps、渲染定时器 30 Hz，不去重则同一帧被重复转换缩放；停流后
    // `hasFrame_` 恒为真（保留末帧），去重能让负载真正归零。
    // `force=true` 用于「画面没变但必须重画」的场合（切叠加框开关、单次检测出框、切回本页）。
    //
    // P3（2026-09-22）：① 缩放提到 `QPixmap::fromImage` **之前**（转换量 2.07 M → 0.13 M 像素）；
    // ② 彩图走 `ColorToQImageView` 零拷贝只读视图（省 6.2 MB `QImage::copy()`）；
    // ③ 深度图直接产出显示尺寸。三格显示面积合计约 0.39 M 像素，改造前却做了约 8 遍
    // 全分辨率（2.07 M）像素操作。
    // ⚠ 本函数**不**自行判可见性——周期路径的门控在 `OnRenderTick`，`showEvent` 的补绘
    // 与 `ApplyDetectionResult` 的强制重绘都要能进来。
    void RenderTiles(bool force = false);
    // 三格信息条文本（FPS / 时间戳 / 分辨率）——廉价，每拍都刷，与位图去重解耦。
    void UpdateTileInfos();
    QImage BuildRgbImage();
    // `target` 非空 → 直接产出该尺寸（最近邻采样，跳过全分辨率中间图，P3-2）；
    // 留空 = 全分辨率，供存图路径使用。
    QImage BuildDepthImage(const QSize& target = QSize());
    // Overlay = 同一张彩图 + 识别框。`base` 传入调用方已转好的彩图可省掉一次
    // `ColorToQImage`（6.2 MB 拷贝 + `QImage::copy()`）；传空则由本函数自行转换。
    QImage BuildOverlayImage(const QImage& base = QImage());

    // 检测调度（P1，2026-09-22）。`ScheduleDetection` 在主线程做两件事：取参数快照、
    // 把帧投给检测线程；结果回到主线程后由 `ApplyDetectionResult` 落表 / 画框 / 记日志。
    // ⚠ 参数快照必须在主线程取——`IPuffAlgorithm::SnapshotParams()` 读 `ConfigManager`，
    // 而项目硬约束是「worker 线程绝不读 ConfigManager」（`json` 读写非线程安全）。
    void ScheduleDetection(const CameraFramePtr& frame, bool allowAutoSave);
    void ApplyDetectionResult(const QVector<PuffResult>& results, double elapsedMs, bool allowAutoSave);
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

    // ---- 检测工作线程（P1，2026-09-22）----
    // 把「对整幅 1920×1080 逐像素扫描」的检测挪出 UI 线程（原先每帧一个 10 ms 级
    // 不可中断块，是视觉页拖窗 / 切页卡顿的直接原因）。
    // `detectCtx_` 只是**线程归属载体**：走 `QMetaObject::invokeMethod` 的 functor
    // 重载，不需要 `Q_OBJECT`，因此不必新增源文件（新增文件会触发 CMake reconfigure）。
    QThread*       detectThread_ = nullptr;
    QObject*       detectCtx_    = nullptr;
    // 在途检测标记，同时充当**背压闸门**：检测比帧率慢时只保留最新一帧，不排队堆积。
    bool           detectBusy_   = false;
    CameraFramePtr pendingFrame_;          // 忙时暂存的最新一帧（共享指针，不拷帧数据）
    bool           pendingAutoSave_ = false;

    // 最新帧（P2：共享只读指针——缓存动作只是持有同一份，不再深拷贝 14.5 MB/帧）
    CameraFramePtr      latestFrame_;
    QVector<PuffResult> lastResults_;
    double              lastDetectMs_ = 0.0;
    // 上一拍**真正画过**的帧时间戳（-1 = 从未画过）。`RenderTiles` 去重依据，
    // 与三格目标尺寸共同构成「这一拍有没有必要重算位图」的判据。
    int64_t             lastRenderedTs_ = -1;
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
