#pragma once

#include <QObject>
#include <QMediaPlayer>
#include <QUrl>

class VideoImportController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY changed)
    Q_PROPERTY(QString filePath READ filePath NOTIFY changed)
    Q_PROPERTY(QUrl sourceUrl READ sourceUrl NOTIFY changed)
    Q_PROPERTY(QString fileName READ fileName NOTIFY changed)
    Q_PROPERTY(QString fileSizeText READ fileSizeText NOTIFY changed)
    Q_PROPERTY(QString durationText READ durationText NOTIFY changed)
    Q_PROPERTY(QString resolutionText READ resolutionText NOTIFY changed)
    Q_PROPERTY(QString frameRateText READ frameRateText NOTIFY changed)
    Q_PROPERTY(QString codecText READ codecText NOTIFY changed)
    Q_PROPERTY(QString originalDateText READ originalDateText NOTIFY changed)
    Q_PROPERTY(QString originalDateSourceText READ originalDateSourceText NOTIFY changed)
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY changed)

public:
    explicit VideoImportController(QObject *parent = nullptr);

    bool hasVideo() const { return !m_filePath.isEmpty(); }
    bool ready() const { return m_ready; }
    bool loading() const { return m_loading; }
    qint64 durationMs() const { return m_durationMs; }
    QString filePath() const { return m_filePath; }
    QUrl sourceUrl() const { return m_filePath.isEmpty() ? QUrl() : QUrl::fromLocalFile(m_filePath); }
    QString fileName() const { return m_fileName; }
    QString fileSizeText() const { return m_fileSizeText; }
    QString durationText() const { return m_durationText; }
    QString resolutionText() const { return m_resolutionText; }
    QString frameRateText() const { return m_frameRateText; }
    QString codecText() const { return m_codecText; }
    QString originalDateText() const { return m_originalDateText; }
    QString originalDateSourceText() const { return m_originalDateSourceText; }
    QString statusText() const { return m_statusText; }
    QString errorMessage() const { return m_errorMessage; }

    Q_INVOKABLE void importVideo(const QUrl &url);
    Q_INVOKABLE void clear();

signals:
    void changed();

private:
    void refreshMetadata();
    void fail(const QString &message);
    static QString formatBytes(qint64 bytes);
    static QString formatDuration(qint64 milliseconds);

    QMediaPlayer m_player;
    bool m_ready = false;
    bool m_loading = false;
    qint64 m_durationMs = 0;
    QString m_filePath;
    QString m_fileName;
    QString m_fileSizeText = QStringLiteral("—");
    QString m_durationText = QStringLiteral("—");
    QString m_resolutionText = QStringLiteral("—");
    QString m_frameRateText = QStringLiteral("—");
    QString m_codecText = QStringLiteral("—");
    QString m_originalDateText = QStringLiteral("—");
    QString m_originalDateSourceText;
    bool m_hasPreferredDate = false;
    QString m_statusText = QStringLiteral("等待导入视频");
    QString m_errorMessage;
};
