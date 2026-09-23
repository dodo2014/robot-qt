#pragma once

#include <QWidget>
#include <QButtonGroup>
#include <QStackedWidget>
#include <QListWidget>
#include <QLabel>
#include <QComboBox>

class QLineEdit;
class QCheckBox;
class QShowEvent;

class ConfigPage : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigPage(QWidget* parent = nullptr);

signals:
    void paramsChanged();
    // 自动运行页预览源切换（S6 决策 6 的「视频流预览」保留能力，TR-113）。
    // true = 原始视频流 + 可见性持有；false = 检测结果叠加图（默认）。
    // 与 `paramsChanged` **刻意分开**：后者会触发 Kinematics 重建，与本开关无关。
    void autoRunPreviewSourceChanged(bool streamPreview);

private slots:
    void OnTabClicked(int index);

private:
    void SetupUI();
    // 安全位字段刷新（2026-09-09 TR-079）：示教由手动控制页直接写 config（按设计不 emit
    // paramsChanged），配置页构造时读的初值不会自动更新 → 每次页面显示时重新读 config 回填，
    // 否则"示教完切到配置页看到的还是旧值，重启才对"。
    void RefreshSafePosFields();
    // 视觉参数刷新（C3，2026-09-20）：`vision.*` 三个键在视觉测试页也有写入口，
    // 两处必须双向同步 —— 否则改完视觉页再切回本页看到的仍是旧值，
    // 更糟的是误触 editingFinished 会把陈旧值写回 config。同样在每次显示时回填。
    void RefreshVisionFields();
    void showEvent(QShowEvent* event) override;

    QWidget* CreateTab1Comm();
    QWidget* CreateTab2Kinematics();
    QWidget* CreateTab3Vision();
    QWidget* CreateTab4TCP();
    QWidget* CreateElecMapTab();

    QButtonGroup*   tabGroup_   = nullptr;
    QStackedWidget* tabStack_   = nullptr;
    QLabel*         axisTitle_  = nullptr;
    QListWidget*    axisList_   = nullptr;
    QStackedWidget* transStack_ = nullptr;

    // 安全位输入控件（控件 + config 路径），供 RefreshSafePosFields 回填
    QVector<QPair<QLineEdit*, QByteArray>> safePosInputs_;
    QCheckBox* safePosEnabledChk_ = nullptr;

    // 视觉参数输入控件（控件 + config 路径 + 缺键时的兜底值），供 RefreshVisionFields 回填
    struct VisionField {
        QLineEdit* edit     = nullptr;
        QByteArray path;
        double     fallback = 0.0;
    };
    QVector<VisionField> visionInputs_;
};
