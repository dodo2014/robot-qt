#pragma once

#include <QObject>
#include <QHash>
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
    // `timestampMs`：入队时刻定死的文件名时间戳（epoch ms）；**0 = 取当前时间**。
    // 同一拍的多路存图（rgb / depth / overlay）必须传同一个值——落盘名的时间戳
    // 一致是用户做深度图 / RGB 对比的前提（TR-130）。
    void SaveImage(const QImage& image, const QString& subdir = QStringLiteral("snapshots"),
                   qint64 timestampMs = 0);

signals:
    void imageSaved(const QString& path);
    void saveError(const QString& message);

private slots:
    void ProcessQueue();

private:
    struct Job {
        QImage  image;
        QString subdir;
        qint64  tsMs = 0;   // 入队时刻定死的文件名时间戳（epoch ms，TR-130）
    };

    // 文件名编号状态（TR-129 / TR-130）：`类型_时间戳_编号.png` 规则下，
    // 按「类型 × 秒」（键 = `类型/时间戳串`）记住下一个可用编号，同一秒内从 0 自增、
    // 跨秒自然归零（键不同即新计数）。
    // ⚠ 仅 `ProcessQueue`（saver 线程单线程执行）访问，无需加锁。
    QHash<QString, int> nameSeqs_;   // 键 = "类型/时间戳串"，值 = 下一个可用编号

    QTimer*             timer_  = nullptr;
    std::deque<Job>     queue_;
    std::mutex          mutex_;
    QString             outputDir_;
    bool                running_ = false;
};
