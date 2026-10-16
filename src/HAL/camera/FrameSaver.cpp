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

void FrameSaver::SaveImage(const QImage& image, const QString& subdir, qint64 timestampMs)
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
        // TR-130（2026-10-15）：时间戳在**入队时刻**定死（0 = 取当前时间）。
        // 同一拍的三路存图由调用方传**同一个** ts——用户拿 rgb / depth 落盘名做
        // 对比的前提是两者时间戳严格一致；若在处理阶段逐 job 现取，tick 边界
        // （kMaxPerTick 分批）或积压都会让时间戳漂移到不同秒。
        queue_.push_back({ image, subdir,
                           timestampMs == 0 ? QDateTime::currentMSecsSinceEpoch() : timestampMs });
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

        // ⚠ 2026-09-22 用例 11.23 实测发现（TR-109）：原判据写成
        //   `if (!subdir.isEmpty() && !base.mkpath(subdir)) base.setPath(...)`
        // —— `!base.mkpath(...)` **恒为 false**。Qt 6 文档原文：
        //   「Returns true on success or if dirPath already existed; otherwise returns false.」
        // 于是 `setPath` 永不执行、`base` 一直停在 `outputDir_` 根目录：子目录被建出来却
        // 全空，文件全落在 `saves/` 根下（磁盘实证：rgb/ depth/ overlay/ 三目录 0 文件，
        // 9 张 PNG 全在根目录）。**建目录与切目录是两件事，切目录必须无条件执行。**
        QDir base(outputDir_);
        if (!job.subdir.isEmpty()) {
            if (!base.mkpath(job.subdir))
                SPDLOG_WARN("[FrameSaver] 子目录创建失败: {}（仍按该路径写入，失败会报 saveError）",
                            job.subdir.toStdString());
            base.setPath(outputDir_ + QLatin1Char('/') + job.subdir);
        }

        // 文件名规则（TR-129 / TR-130）：`类型_时间戳_编号.png`。
        // 类型 = 子目录名（rgb / depth / overlay）；时间戳 = **入队时刻定死的
        // job.tsMs**（同一拍三路一致，见 SaveImage 注释）；14 位**无分隔**
        // （yyyyMMddHHmmss，对齐需求示例 depth_20261015164203_0.png）。
        // 编号按「类型 × 秒」独立、从 0 自增——键含秒字符串，跨秒自然归零。
        // ⚠ `nameSeqs_` 只在本函数（saver 线程）访问，无需加锁。
        const QString type  = job.subdir.isEmpty() ? QStringLiteral("frame") : job.subdir;
        const QString stamp = QDateTime::fromMSecsSinceEpoch(job.tsMs).toString(QStringLiteral("yyyyMMddHHmmss"));
        const QString seqKey = type + QLatin1Char('/') + stamp;
        int& nextIdx = nameSeqs_[seqKey];
        QString path;
        do {
            path = base.filePath(QStringLiteral("%1_%2_%3.png").arg(type, stamp).arg(nextIdx));
            ++nextIdx;
        } while (QFile::exists(path));   // 兜底：同名残留（如上次运行恰在同一秒落过文件）时顺延，计数器同步前进

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
