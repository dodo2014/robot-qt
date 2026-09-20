#pragma once

#include <QObject>
#include <QImage>
#include <QString>

#include <deque>
#include <mutex>

class QTimer;

// 异步存图（独立线程）。
//
// **P4 加固**（`doc/vision_test_page_redesign_plan.md` §7.4，对应缺陷 D4 / D5）：
//   D4 原 `SaveImage` 对 `queue_` 是**无界 `push_back`**——三路存图 + 连续检测下
//      每帧 3 张（1280×720 `QImage` ≈ 3.5 MB）会无界增长到 OOM；
//   D5 原 `ProcessQueue` 是 `while(true)` **无预算排空**——积压 90 张时该线程要连续
//      阻塞数十秒（其消息循环停摆，`Stop()` 也只能排在后面），且**每张都
//      `emit imageSaved`** → UI 每秒收 90 个信号 + spdlog 刷屏。
class FrameSaver : public QObject
{
    Q_OBJECT

public:
    // 队列上限（D4）。超限丢**最旧**并记一条 `saveError`：丢新的会让「刚点的那次
    // 保存」静默失效，丢旧的保住最近的操作。30 张 ≈ 105 MB 峰值，对 1280×720 尚可。
    static constexpr int kMaxQueueSize = 30;
    // 单 tick 处理上限（D5）。30 ms 间隔 × 3 张 × (10~30 ms/张) ≈ 每 tick 30~90 ms，
    // 积压靠后续 tick 逐步消化，**不阻塞**该线程的消息循环。
    static constexpr int kMaxPerTick = 3;

    explicit FrameSaver(QObject* parent = nullptr);
    ~FrameSaver() override;

    void SetOutputDir(const QString& dir);

public slots:
    void Start();
    void Stop();
    void SaveImage(const QImage& image, const QString& subdir = QStringLiteral("snapshots"));

signals:
    void imageSaved(const QString& path);
    void saveError(const QString& message);

private slots:
    void ProcessQueue();

private:
    struct Job {
        QImage  image;
        QString subdir;
    };

    QTimer*             timer_  = nullptr;
    std::deque<Job>     queue_;
    std::mutex          mutex_;
    QString             outputDir_;
    bool                running_ = false;
};
