#include "FrameSaver.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTimer>

#include <spdlog/spdlog.h>

FrameSaver::FrameSaver(QObject* parent)
    : QObject(parent)
{
}

FrameSaver::~FrameSaver()
{
    Stop();
}

void FrameSaver::SetOutputDir(const QString& dir)
{
    std::lock_guard<std::mutex> lock(mutex_);
    outputDir_ = dir;
    if (!outputDir_.isEmpty())
        QDir().mkpath(outputDir_);
}

void FrameSaver::Start()
{
    if (running_) return;
    if (!timer_) {
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, &FrameSaver::ProcessQueue);
        timer_->setInterval(30);
    }
    timer_->start();
    running_ = true;
    SPDLOG_INFO("[FrameSaver] started");
}

void FrameSaver::Stop()
{
    if (timer_) timer_->stop();
    running_ = false;

    // P4 / D5：**不再**同步排空队列。原实现在这里调 `ProcessQueue()`，于是退出时
    // 会同步把积压全部编码落盘（30 张 ≈ 0.3~1 s 卡住退出路径）。改为**丢弃**——
    // `saves/` 是调试产物，丢几张的代价远小于退出卡顿。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }
    SPDLOG_INFO("[FrameSaver] stopped (queue discarded)");
}

void FrameSaver::SaveImage(const QImage& image, const QString& subdir)
{
    if (image.isNull()) return;

    QString dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // D4：上限保护。超限丢最旧（保住最近一次点击的保存）。
        while (static_cast<int>(queue_.size()) >= kMaxQueueSize) {
            queue_.pop_front();
            dropped = QStringLiteral("保存队列已满（上限 %1 张），已丢弃最旧一张").arg(kMaxQueueSize);
        }
        queue_.push_back({ image, subdir });
    }
    // 锁外发信号（本函数在存图线程执行，emit 经队列投递到 UI 线程）
    if (!dropped.isEmpty()) emit saveError(dropped);
}

void FrameSaver::ProcessQueue()
{
    if (outputDir_.isEmpty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        return;
    }

    // D5：单 tick 预算。原 `while(true)` 会把积压一次排空，积压 90 张时本线程
    // 连续阻塞数十秒。
    int     done       = 0;
    QString lastSaved;
    QString firstError;

    while (done < kMaxPerTick) {
        Job job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) break;
            job = std::move(queue_.front());
            queue_.pop_front();
        }

        QDir base(outputDir_);
        if (!job.subdir.isEmpty() && !base.mkpath(job.subdir))
            base.setPath(outputDir_ + QLatin1Char('/') + job.subdir);

        const QString name = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
        QString path = base.filePath(name + QStringLiteral(".png"));
        // 同一 tick 内连存多张时毫秒戳可能相同 → 加序号后缀，避免静默覆盖
        for (int k = 1; QFile::exists(path); ++k)
            path = base.filePath(QStringLiteral("%1_%2.png").arg(name).arg(k));

        if (job.image.save(path, "PNG")) {
            lastSaved = path;   // 只记最后一次 → 下面合并成一次通知
        } else if (firstError.isEmpty()) {
            firstError = QStringLiteral("写入失败: %1").arg(path);
        }
        ++done;
    }

    // D5：**合并通知**。原实现每张都 emit，连续存图时 UI 每秒收 90 个信号
    // （每个都触发 SetStatus → spdlog 刷屏）。改为每 tick 最多各发一次。
    if (!lastSaved.isEmpty())  emit imageSaved(lastSaved);
    if (!firstError.isEmpty()) emit saveError(firstError);
}
