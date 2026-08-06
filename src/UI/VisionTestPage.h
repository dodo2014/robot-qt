#pragma once

#include <QWidget>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QTableWidget>
#include <QImage>
#include <QTimer>
#include <QVector>

#include "ICamera.h"
#include "IPuffAlgorithm.h"

class QThread;
class FrameSaver;

class VisionTestPage : public QWidget
{
    Q_OBJECT

public:
    explicit VisionTestPage(QWidget* parent = nullptr);
    ~VisionTestPage() override;

private slots:
    void OnOpenCamera();
    void OnCloseCamera();
    void OnStartStream();
    void OnStopStream();
    void OnFrameReady(const CameraFrame& frame);
    void OnSingleDetect();
    void OnToggleContinuous();
    void OnToggleOverlay();
    void OnShowRGB();
    void OnShowDepth();
    void OnSnapshot();
    void OnLoadImage();
    void OnFpsTick();
    void OnImageSaved(const QString& path);
    void OnSaveError(const QString& message);

private:
    void SetupUI();
    QWidget* CreateControlPanel();
    QWidget* CreatePreviewArea();
    void UpdateCameraState();
    void RenderLatest();
    QImage BuildDisplayImage();
    void RunDetection(const CameraFrame& frame);
    void ShowResults(const QVector<PuffResult>& results);
    void SetStatus(const QString& text);
    static QVector<PuffResult> ToQVector(const std::vector<PuffResult>& v);

    QComboBox*   cameraTypeCombo_  = nullptr;
    QLineEdit*   deviceIdEdit_     = nullptr;
    QComboBox*   resolutionCombo_  = nullptr;
    QComboBox*   fpsCombo_         = nullptr;
    QLabel*      cameraStateDot_   = nullptr;
    QLabel*      cameraStateText_  = nullptr;

    QLabel*      previewLabel_     = nullptr;
    QPushButton* rgbBtn_           = nullptr;
    QPushButton* depthBtn_         = nullptr;
    QPushButton* overlayBtn_       = nullptr;
    QLabel*      infoLabel_        = nullptr;

    QComboBox*   algoCombo_        = nullptr;
    QPushButton* continuousBtn_    = nullptr;
    QTableWidget* resultTable_     = nullptr;

    QDoubleSpinBox* confSpin_      = nullptr;
    QDoubleSpinBox* zminSpin_      = nullptr;
    QDoubleSpinBox* zmaxSpin_      = nullptr;
    QDoubleSpinBox* exposureSpin_  = nullptr;

    QLabel*      statusLabel_      = nullptr;

    FrameSaver* frameSaver_   = nullptr;
    QThread*    saverThread_  = nullptr;

    CameraFrame latestFrame_;
    QVector<PuffResult> lastResults_;
    bool hasFrame_    = false;
    bool streaming_   = false;
    bool showOverlay_ = true;
    bool continuous_  = false;
    bool depthView_   = false;

    QTimer* fpsTimer_  = nullptr;
    int    frameCount_ = 0;
    double fps_        = 0.0;
};
