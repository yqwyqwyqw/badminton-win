#include "TrialAnalysisController.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

TrialAnalysisController::TrialAnalysisController(QObject *parent)
    : QObject(parent)
{
    connect(&m_process, &QProcess::readyReadStandardOutput,
            this, &TrialAnalysisController::readProcessOutput);
    connect(&m_process, &QProcess::readyReadStandardError,
            this, &TrialAnalysisController::readProcessError);
    connect(&m_process, &QProcess::finished,
            this, &TrialAnalysisController::processFinished);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(QStringLiteral("无法启动处理程序：%1").arg(m_process.errorString()));
    });

    m_refreshTimer.setInterval(500);
    connect(&m_refreshTimer, &QTimer::timeout, this, &TrialAnalysisController::refreshFiles);
}

bool TrialAnalysisController::proxyReady() const
{
    return QFileInfo(m_proxyPath).size() > 1024;
}

QUrl TrialAnalysisController::proxyUrl() const
{
    return proxyReady() ? QUrl::fromLocalFile(m_proxyPath) : QUrl();
}

void TrialAnalysisController::startTrial(
    const QString &sourcePath, qint64 sourceDurationMs, int maxRallies)
{
    if (running())
        return;
    const QFileInfo source(sourcePath);
    if (!source.exists() || !source.isFile()) {
        fail(QStringLiteral("请先导入有效的视频文件"));
        return;
    }

    m_sourcePath = source.absoluteFilePath();
    m_sourceDurationMs = sourceDurationMs;
    m_maxRallies = qBound(1, maxRallies, 10);
    m_progress = 0.0;
    m_errorMessage.clear();
    m_actionMessage.clear();
    m_logTail.clear();
    m_cancelRequested = false;
    const QString previousOutputDirectory = m_outputDirectory;
    configurePaths(m_sourcePath);
    if (m_outputDirectory != previousOutputDirectory) {
        m_rallies.clear();
        m_etaText.clear();
        emit changed();
    }

    const QStringList required = {
        m_ffmpegPath, m_pythonPath, m_runnerPath, m_modelPath, m_vendorPath
    };
    for (const QString &path : required) {
        if (!QFileInfo::exists(path)) {
            fail(QStringLiteral("缺少运行依赖：%1").arg(QDir::toNativeSeparators(path)));
            return;
        }
    }

    QDir().mkpath(m_outputDirectory);
    QDir().mkpath(m_analysisDirectory);
    loadRallies();
    if (m_rallies.size() >= m_maxRallies) {
        m_stage = Stage::Complete;
        m_state = QStringLiteral("complete");
        m_progress = 1.0;
        m_stageText = QStringLiteral("试验结果已从缓存载入");
        m_detailText = QStringLiteral("已获得 %1 个回合").arg(m_rallies.size());
        emit changed();
        return;
    }

    m_refreshTimer.start();
    if (proxyReady())
        startInference();
    else
        startProxy(false);
}

void TrialAnalysisController::cancel()
{
    if (!running())
        return;
    m_cancelRequested = true;
    m_stageText = QStringLiteral("正在停止，已完成分块会保留");
    emit changed();
    m_process.terminate();
    QTimer::singleShot(2500, this, [this]() {
        if (m_process.state() != QProcess::NotRunning)
            m_process.kill();
    });
}

void TrialAnalysisController::reset()
{
    if (running())
        cancel();
    m_refreshTimer.stop();
    m_stage = Stage::Idle;
    m_state = QStringLiteral("idle");
    m_stageText = QStringLiteral("等待开始切分试验");
    m_detailText = QStringLiteral("将只分析指定数量的前几个回合");
    m_etaText.clear();
    m_errorMessage.clear();
    m_actionMessage.clear();
    m_progress = 0.0;
    m_rallies.clear();
    emit changed();
}

QUrl TrialAnalysisController::clipUrl(int index) const
{
    if (index < 0 || index >= m_rallies.size())
        return {};
    const QString path = m_rallies.at(index).toMap().value(QStringLiteral("clipPath")).toString();
    return QFileInfo::exists(path) ? QUrl::fromLocalFile(path) : QUrl();
}

bool TrialAnalysisController::exportRally(int index, const QUrl &folderUrl)
{
    if (index < 0 || index >= m_rallies.size() || !folderUrl.isLocalFile())
        return false;
    const QString sourcePath = m_rallies.at(index).toMap()
                                   .value(QStringLiteral("clipPath")).toString();
    const QFileInfo source(sourcePath);
    QDir folder(folderUrl.toLocalFile());
    if (!source.exists() || !folder.exists()) {
        m_actionMessage = QStringLiteral("导出失败：源短片或目标文件夹不存在");
        emit changed();
        return false;
    }

    QString destination = folder.filePath(source.fileName());
    int suffix = 2;
    while (QFileInfo::exists(destination)) {
        destination = folder.filePath(
            QStringLiteral("%1-%2.%3").arg(source.completeBaseName()).arg(suffix++).arg(source.suffix()));
    }
    const bool copied = QFile::copy(source.absoluteFilePath(), destination);
    m_actionMessage = copied
        ? QStringLiteral("已导出：%1").arg(QDir::toNativeSeparators(destination))
        : QStringLiteral("导出失败：无法复制文件");
    emit changed();
    return copied;
}

void TrialAnalysisController::openOutputFolder() const
{
    if (!m_outputDirectory.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_outputDirectory));
}

void TrialAnalysisController::configurePaths(const QString &sourcePath)
{
    m_projectRoot = discoverProjectRoot();
    const QFileInfo source(sourcePath);
    QString safeName = source.completeBaseName();
    safeName.replace(QRegularExpression(QStringLiteral(R"([^\p{L}\p{N}._-]+)")), QStringLiteral("_"));
    safeName = safeName.left(48);
    const QByteArray identity = source.absoluteFilePath().toUtf8()
        + QByteArray::number(source.size())
        + QByteArray::number(source.lastModified().toMSecsSinceEpoch());
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha1).toHex().left(10));

    m_outputDirectory = QDir(m_projectRoot).filePath(
        QStringLiteral("validation-output/ui-trials/%1-%2").arg(safeName, key));
    m_proxyPath = QDir(m_outputDirectory).filePath(QStringLiteral("proxy-720p.mp4"));
    m_proxyTemporaryPath = QDir(m_outputDirectory).filePath(QStringLiteral("proxy-720p.part.mp4"));
    m_analysisDirectory = QDir(m_outputDirectory).filePath(QStringLiteral("tracknet"));
    m_ffmpegPath = QDir(m_projectRoot).filePath(
        QStringLiteral(".tools/ffmpeg/ffmpeg-9.0.1-full_build-shared/bin/ffmpeg.exe"));
    m_pythonPath = QDir(m_projectRoot).filePath(
        QStringLiteral(".venv-validation/Scripts/python.exe"));
    m_runnerPath = QDir(m_projectRoot).filePath(
        QStringLiteral("validation/run_chunked_tracknet.py"));
    m_modelPath = QDir(m_projectRoot).filePath(
        QStringLiteral(".tools/models/TrackNetV3/ckpts/TrackNet_best.pt"));
    m_vendorPath = QDir(m_projectRoot).filePath(
        QStringLiteral(".tools/vendor/BadmintonTrackNet"));
}

void TrialAnalysisController::startProxy(bool softwareFallback)
{
    m_stage = Stage::Proxy;
    m_state = QStringLiteral("proxy");
    m_proxySoftwareFallback = softwareFallback;
    m_progress = 0.0;
    m_etaText.clear();
    m_stageText = softwareFallback
        ? QStringLiteral("正在生成 720p 代理（兼容模式）")
        : QStringLiteral("正在生成 720p 代理");
    m_detailText = QStringLiteral("代理只用于流畅预览和分析，原片不会被修改");
    QFile::remove(m_proxyTemporaryPath);

    QStringList arguments = {
        QStringLiteral("-hide_banner"), QStringLiteral("-y"), QStringLiteral("-nostdin"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-progress"), QStringLiteral("pipe:1")
    };
    if (!softwareFallback) {
        arguments << QStringLiteral("-hwaccel") << QStringLiteral("cuda")
                  << QStringLiteral("-hwaccel_output_format") << QStringLiteral("cuda");
    }
    arguments << QStringLiteral("-i") << m_sourcePath
              << QStringLiteral("-map") << QStringLiteral("0:v:0")
              << QStringLiteral("-map") << QStringLiteral("0:a:0?")
              << QStringLiteral("-vf")
              << (softwareFallback
                      ? QStringLiteral("scale=1280:720:flags=lanczos")
                      : QStringLiteral("scale_cuda=1280:720,hwdownload,format=nv12"))
              << QStringLiteral("-c:v") << QStringLiteral("libx264")
              << QStringLiteral("-preset") << QStringLiteral("veryfast")
              << QStringLiteral("-crf") << QStringLiteral("23")
              << QStringLiteral("-c:a") << QStringLiteral("aac")
              << QStringLiteral("-b:a") << QStringLiteral("128k")
              << QStringLiteral("-movflags") << QStringLiteral("+faststart")
              << m_proxyTemporaryPath;

    m_process.setWorkingDirectory(m_projectRoot);
    m_process.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    m_process.setProgram(m_ffmpegPath);
    m_process.setArguments(arguments);
    m_process.start();
    emit changed();
}

void TrialAnalysisController::startInference()
{
    m_stage = Stage::Inference;
    m_state = QStringLiteral("inference");
    m_progress = 0.0;
    m_stageText = QStringLiteral("TrackNet 正在识别前 %1 个回合").arg(m_maxRallies);
    m_detailText = QStringLiteral("每识别完一个完整回合，就会立即出现在右侧");

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString existingPythonPath = environment.value(QStringLiteral("PYTHONPATH"));
    environment.insert(
        QStringLiteral("PYTHONPATH"),
        existingPythonPath.isEmpty()
            ? m_vendorPath
            : m_vendorPath + QDir::listSeparator() + existingPythonPath);

    const QStringList arguments = {
        m_runnerPath,
        QStringLiteral("--video"), m_proxyPath,
        QStringLiteral("--tracknet-file"), m_modelPath,
        QStringLiteral("--output-dir"), m_analysisDirectory,
        QStringLiteral("--chunk-seconds"), QStringLiteral("6"),
        QStringLiteral("--overlap-seconds"), QStringLiteral("0.5"),
        QStringLiteral("--batch-size"), QStringLiteral("8"),
        QStringLiteral("--eval-mode"), QStringLiteral("nonoverlap"),
        QStringLiteral("--max-rallies"), QString::number(m_maxRallies),
        QStringLiteral("--ffmpeg"), m_ffmpegPath
    };

    m_process.setWorkingDirectory(m_projectRoot);
    m_process.setProcessEnvironment(environment);
    m_process.setProgram(m_pythonPath);
    m_process.setArguments(arguments);
    m_process.start();
    emit changed();
}

void TrialAnalysisController::readProcessOutput()
{
    const QString output = QString::fromUtf8(m_process.readAllStandardOutput());
    appendLog(output);
    for (const QString &line : output.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        processLine(line.trimmed());
}

void TrialAnalysisController::readProcessError()
{
    appendLog(QString::fromUtf8(m_process.readAllStandardError()));
}

void TrialAnalysisController::processLine(const QString &line)
{
    if (m_stage == Stage::Proxy && line.startsWith(QStringLiteral("out_time_"))) {
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        if (equals > 0 && m_sourceDurationMs > 0) {
            bool ok = false;
            const qint64 microseconds = line.mid(equals + 1).toLongLong(&ok);
            if (ok)
                m_progress = qBound(0.0, microseconds / 1000.0 / m_sourceDurationMs, 0.99);
        }
    } else if (m_stage == Stage::Inference && line.startsWith(QStringLiteral("PROGRESS "))) {
        const QRegularExpressionMatch percent =
            QRegularExpression(QStringLiteral(R"(percent=([0-9.]+))")).match(line);
        const QRegularExpressionMatch eta =
            QRegularExpression(QStringLiteral(R"(eta=([0-9.]+|None)s?)")).match(line);
        if (percent.hasMatch())
            m_progress = qBound(0.0, percent.captured(1).toDouble() / 100.0, 1.0);
        if (eta.hasMatch() && eta.captured(1) != QStringLiteral("None"))
            m_etaText = QStringLiteral("预计剩余 %1").arg(formatSeconds(eta.captured(1).toDouble()));
    } else if (line.startsWith(QStringLiteral("RALLY_READY"))) {
        loadRallies();
    }
    emit changed();
}

void TrialAnalysisController::processFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (m_cancelRequested) {
        m_cancelRequested = false;
        m_stage = Stage::Paused;
        m_state = QStringLiteral("paused");
        m_stageText = QStringLiteral("试验已停止，可继续运行");
        m_detailText = QStringLiteral("已完成的代理、轨迹分块和回合短片均已保留");
        m_etaText.clear();
        m_refreshTimer.stop();
        refreshFiles();
        emit changed();
        return;
    }

    const bool succeeded = exitStatus == QProcess::NormalExit && exitCode == 0;
    if (m_stage == Stage::Proxy) {
        if (!succeeded && !m_proxySoftwareFallback) {
            startProxy(true);
            return;
        }
        if (!succeeded) {
            fail(QStringLiteral("720p 代理生成失败：%1").arg(m_logTail.right(500)));
            return;
        }
        QFile::remove(m_proxyPath);
        if (!QFile::rename(m_proxyTemporaryPath, m_proxyPath)) {
            fail(QStringLiteral("代理已生成，但无法写入最终文件"));
            return;
        }
        m_progress = 1.0;
        emit changed();
        startInference();
        return;
    }

    if (m_stage == Stage::Inference) {
        refreshFiles();
        if (!succeeded) {
            fail(QStringLiteral("TrackNet 试验失败：%1").arg(m_logTail.right(700)));
            return;
        }
        m_stage = Stage::Complete;
        m_state = QStringLiteral("complete");
        m_progress = 1.0;
        m_etaText.clear();
        m_stageText = QStringLiteral("切分试验完成");
        m_detailText = QStringLiteral("已生成 %1 个可预览回合").arg(m_rallies.size());
        m_refreshTimer.stop();
        emit changed();
    }
}

void TrialAnalysisController::refreshFiles()
{
    if (m_stage == Stage::Inference) {
        loadProgress();
        loadRallies();
    }
}

void TrialAnalysisController::loadProgress()
{
    QFile file(QDir(m_analysisDirectory).filePath(QStringLiteral("progress.json")));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.contains(QStringLiteral("percent")))
        m_progress = qBound(0.0, object.value(QStringLiteral("percent")).toDouble() / 100.0, 1.0);
    const double eta = object.value(QStringLiteral("etaSeconds")).toDouble(-1);
    m_etaText = eta >= 0 ? QStringLiteral("预计剩余 %1").arg(formatSeconds(eta)) : QString();
    emit changed();
}

void TrialAnalysisController::loadRallies()
{
    QFile file(QDir(m_analysisDirectory).filePath(QStringLiteral("rally-feed.json")));
    if (!file.open(QIODevice::ReadOnly))
        return;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray())
        return;

    QVariantList loaded;
    for (const QJsonValue &value : document.array()) {
        if (loaded.size() >= m_maxRallies)
            break;
        QVariantMap rally = value.toObject().toVariantMap();
        const QString clipPath = rally.value(QStringLiteral("clip")).toString();
        rally.insert(QStringLiteral("clipPath"), clipPath);
        rally.insert(QStringLiteral("clipUrl"), QUrl::fromLocalFile(clipPath));
        rally.insert(
            QStringLiteral("timeText"),
            QStringLiteral("%1 – %2")
                .arg(formatSeconds(rally.value(QStringLiteral("startSeconds")).toDouble()))
                .arg(formatSeconds(rally.value(QStringLiteral("endSeconds")).toDouble())));
        loaded.append(rally);
    }
    if (loaded != m_rallies) {
        m_rallies = loaded;
        m_detailText = QStringLiteral("已发现 %1 / %2 个回合")
                           .arg(m_rallies.size()).arg(m_maxRallies);
        emit changed();
    }
}

void TrialAnalysisController::fail(const QString &message)
{
    m_stage = Stage::Error;
    m_state = QStringLiteral("error");
    m_stageText = QStringLiteral("切分试验未完成");
    m_errorMessage = message;
    m_refreshTimer.stop();
    emit changed();
}

void TrialAnalysisController::appendLog(const QString &text)
{
    m_logTail += text;
    if (m_logTail.size() > 5000)
        m_logTail = m_logTail.right(5000);
}

QString TrialAnalysisController::discoverProjectRoot()
{
    QStringList starts = { QDir::currentPath(), QCoreApplication::applicationDirPath() };
    for (const QString &start : starts) {
        QDir directory(start);
        for (int depth = 0; depth < 7; ++depth) {
            if (directory.exists(QStringLiteral("validation/run_chunked_tracknet.py"))
                && directory.exists(QStringLiteral(".tools")))
                return directory.absolutePath();
            if (!directory.cdUp())
                break;
        }
    }
    return QDir::currentPath();
}

QString TrialAnalysisController::formatSeconds(double seconds)
{
    const int rounded = qMax(0, qRound(seconds));
    const int hours = rounded / 3600;
    const int minutes = (rounded % 3600) / 60;
    const int remainder = rounded % 60;
    return hours > 0
        ? QStringLiteral("%1:%2:%3")
              .arg(hours, 2, 10, QLatin1Char('0'))
              .arg(minutes, 2, 10, QLatin1Char('0'))
              .arg(remainder, 2, 10, QLatin1Char('0'))
        : QStringLiteral("%1:%2")
              .arg(minutes, 2, 10, QLatin1Char('0'))
              .arg(remainder, 2, 10, QLatin1Char('0'));
}
