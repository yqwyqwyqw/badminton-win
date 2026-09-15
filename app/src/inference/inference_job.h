// Qt-facing wrapper around the C++ analysis kernel.
//
// Keeps the file contract the QML layer already depends on, so the UI does not
// change when the Python subprocess goes away:
//   <output>/tracknet/progress.json          polled every 500 ms by the controller
//   <output>/tracknet/rally-feed.json        drives the rally list
//   <output>/tracknet/rally-index.csv        human-readable summary
//   <output>/tracknet/chunks/chunk-*.csv     resume unit
//   <output>/tracknet/trajectory-partial.csv merged trajectory
//   <output>/tracknet/analysis/*.csv|json    same provisional analysis files
//   <output>/tracknet/rallies/rally-*.mp4    per-rally preview clips
//
// The kernel runs on a worker thread; signals are delivered to the owning thread.

#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "job_runner.h"

class InferenceJob final : public QObject
{
    Q_OBJECT

public:
    struct Request {
        QString ffmpegPath;
        QString modelPath;
        QString videoPath;   // 720p proxy
        QString outputDir;   // <output>/tracknet
        QString sourcePath;  // original video, used to label the analysis only
        int batchSize = 8;
        double chunkSeconds = 6.0;
        double overlapSeconds = 0.5;
        int medianSamples = 41;
        bool useDirectML = true;
        int intraOpThreads = 8;
        qint64 maxFrames = 0;   // 0 = whole proxy
        int maxRallies = 0;     // 0 = all; >0 stops after that many rallies (trial mode)
        bool writeRallyClips = true;
    };

    explicit InferenceJob(QObject *parent = nullptr);
    ~InferenceJob() override;

    bool isRunning() const { return m_running.load(); }

    void start(const Request &request);
    void cancel();

signals:
    void progressed();
    void finished(bool ok, const QString &message);

private:
    void run(Request request);
    void writeProgress(const badminton::inference::AnalysisProgress &progress) const;
    // Publishes every rally that has closed so far (the UI shows them as they
    // arrive).  Called after each chunk with `final == false`, and once more with
    // `final == true` so the trailing rally is included at the end.
    void publishRallies(const badminton::inference::Trajectory &trajectory,
                        const badminton::inference::VideoMetadata &metadata, bool final);
    void writeFeedFiles(const badminton::inference::VideoMetadata &metadata) const;
    std::string makeClip(const badminton::inference::Rally &rally, std::size_t index) const;

    struct EmittedRally {
        badminton::inference::Rally rally;
        std::string clip;
    };

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancel{false};
    std::thread m_worker;
    QString m_outputDir;
    std::string m_outputDirUtf8;
    std::string m_ffmpegPath;
    std::string m_videoPath;
    int m_maxRallies = 0;
    bool m_writeClips = true;
    std::vector<EmittedRally> m_emitted;
};
