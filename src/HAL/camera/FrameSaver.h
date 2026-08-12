#pragma once

#include <QObject>
#include <QImage>
#include <QString>

#include <deque>
#include <mutex>

class QTimer;

class FrameSaver : public QObject
{
    Q_OBJECT

public:
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
