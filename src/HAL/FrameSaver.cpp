#include "FrameSaver.h"

#include <QDateTime>
#include <QDir>
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
    ProcessQueue();
}

void FrameSaver::SaveImage(const QImage& image, const QString& subdir)
{
    if (image.isNull()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back({ image, subdir });
}

void FrameSaver::ProcessQueue()
{
    if (outputDir_.isEmpty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        return;
    }

    while (true) {
        Job job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }

        QDir base(outputDir_);
        if (!job.subdir.isEmpty() && !base.mkpath(job.subdir))
            base.setPath(outputDir_ + QLatin1Char('/') + job.subdir);

        QString name = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
        QString path = base.filePath(name + QStringLiteral(".png"));
        if (job.image.save(path, "PNG"))
            emit imageSaved(path);
        else
            emit saveError(QStringLiteral("写入失败: %1").arg(path));
    }
}
