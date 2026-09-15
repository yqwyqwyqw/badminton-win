#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

class InferenceJob;

class TrialAnalysisController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool hasResults READ hasResults NOTIFY changed)
    Q_PROPERTY(bool proxyReady READ proxyReady NOTIFY changed)
    Q_PROPERTY(bool fullAnalysis READ fullAnalysis NOTIFY changed)
    Q_PROPERTY(bool exporting READ exporting NOTIFY changed)
    Q_PROPERTY(double exportProgress READ exportProgress NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString stageText READ stageText NOTIFY changed)
    Q_PROPERTY(QString detailText READ detailText NOTIFY changed)
    Q_PROPERTY(QString etaText READ etaText NOTIFY changed)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY changed)
    Q_PROPERTY(QString actionMessage READ actionMessage NOTIFY changed)
    Q_PROPERTY(QString outputDirectory READ outputDirectory NOTIFY changed)
    Q_PROPERTY(int rallyCount READ rallyCount NOTIFY changed)
    Q_PROPERTY(QVariantList rallies READ rallies NOTIFY changed)
    Q_PROPERTY(QUrl proxyUrl READ proxyUrl NOTIFY changed)

public:
    explicit TrialAnalysisController(QObject *parent = nullptr);

    bool running() const;
    bool hasResults() const { return !m_rallies.isEmpty(); }
    bool proxyReady() const;
    bool fullAnalysis() const { return m_fullAnalysis; }
    bool exporting() const { return m_exportProcess.state() != QProcess::NotRunning; }
    double exportProgress() const { return m_exportProgress; }
    double progress() const { return m_progress; }
    QString state() const { return m_state; }
    QString stageText() const { return m_stageText; }
    QString detailText() const { return m_detailText; }
    QString etaText() const { return m_etaText; }
    QString errorMessage() const { return m_errorMessage; }
    QString actionMessage() const { return m_actionMessage; }
    QString outputDirectory() const { return m_outputDirectory; }
    int rallyCount() const { return m_rallies.size(); }
    QVariantList rallies() const { return m_rallies; }
    QUrl proxyUrl() const;

    Q_INVOKABLE void prepareSource(
        const QString &sourcePath, qint64 sourceDurationMs, int sourceWidth, int sourceHeight);
    Q_INVOKABLE void startTrial(
        const QString &sourcePath, qint64 sourceDurationMs,
        int sourceWidth, int sourceHeight, int maxRallies);
    Q_INVOKABLE void startFullAnalysis(
        const QString &sourcePath, qint64 sourceDurationMs,
        int sourceWidth, int sourceHeight);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void reset();
    Q_INVOKABLE QUrl clipUrl(int index) const;
    Q_INVOKABLE bool exportRally(int index, const QUrl &folderUrl, const QString &quality);
    Q_INVOKABLE bool exportAssembly(
        const QVariantList &rows, const QUrl &folderUrl,
        const QString &quality, const QString &baseName);
    Q_INVOKABLE void openOutputFolder() const;

signals:
    void changed();

private:
    enum class Stage { Idle, Proxy, Inference, Complete, Paused, Error };

    void configurePaths(const QString &sourcePath);
    void beginAnalysis(
        const QString &sourcePath, qint64 sourceDurationMs,
        int sourceWidth, int sourceHeight, int maxRallies, bool fullAnalysis);
    void startProxy(bool softwareFallback);
    void startInference();
    void inferenceProgressed();
    void inferenceFinished(bool ok, const QString &message);
    void readProcessOutput();
    void readProcessError();
    void processLine(const QString &line);
    void processFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void refreshFiles();
    void loadProgress();
    void loadRallies();
    void fail(const QString &message);
    void appendLog(const QString &text);
    void readExportOutput();
    void exportFinished(int exitCode, QProcess::ExitStatus exitStatus);
    // 新建项目后清理上一个项目的分析缓存（等分析线程收尾，避免边写边删）
    void runPendingCleanup();
    static QString discoverProjectRoot();
    static QString formatSeconds(double seconds);
    static QString formatSize(qint64 bytes);
    // Packaged layout first (<exe>/resources/...), then the source tree (dev).
    QString resolveResource(const QString &packagedRelative, const QString &developmentRelative) const;

    QProcess m_process;
    QProcess m_exportProcess;
    InferenceJob *m_job = nullptr;
    QTimer m_refreshTimer;
    QTimer m_cleanupTimer;
    QString m_pendingCleanupPath;
    int m_cleanupRetries = 0;
    Stage m_stage = Stage::Idle;
    QString m_state = QStringLiteral("idle");
    QString m_stageText = QStringLiteral("等待开始回合分析");
    QString m_detailText = QStringLiteral("将只分析指定数量的前几个回合");
    QString m_etaText;
    QString m_errorMessage;
    QString m_actionMessage;
    QString m_logTail;
    QString m_projectRoot;
    QString m_sourcePath;
    QString m_outputDirectory;
    QString m_proxyPath;
    QString m_proxyTemporaryPath;
    QString m_analysisDirectory;
    QString m_ffmpegPath;
    QString m_modelPath;
    QString m_exportTemporaryPath;
    QString m_exportFinalPath;
    qint64 m_sourceDurationMs = 0;
    qint64 m_exportDurationMs = 0;
    int m_sourceWidth = 0;
    int m_sourceHeight = 0;
    int m_maxRallies = 5;
    double m_progress = 0.0;
    double m_exportProgress = 0.0;
    bool m_proxySoftwareFallback = false;
    bool m_cancelRequested = false;
    bool m_fullAnalysis = false;
    QVariantList m_rallies;
};
