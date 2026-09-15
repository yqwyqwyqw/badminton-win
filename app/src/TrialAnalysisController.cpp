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
    connect(&m_exportProcess, &QProcess::readyReadStandardOutput,
            this, &TrialAnalysisController::readExportOutput);
    connect(&m_exportProcess, &QProcess::readyReadStandardError, this, [this]() {
        appendLog(QString::fromUtf8(m_exportProcess.readAllStandardError()));
    });
    connect(&m_exportProcess, &QProcess::finished,
            this, &TrialAnalysisController::exportFinished);
    connect(&m_exportProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_actionMessage = QStringLiteral("无法启动导出程序：%1").arg(m_exportProcess.errorString());
            emit changed();
        }
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

void TrialAnalysisController::prepareSource(
    const QString &sourcePath, qint64 sourceDurationMs, int sourceWidth, int sourceHeight)
{
    if (running())
        return;
    const QFileInfo source(sourcePath);
    if (!source.exists() || !source.isFile())
        return;

    m_sourcePath = source.absoluteFilePath();
    m_sourceDurationMs = sourceDurationMs;
    m_sourceWidth = sourceWidth;
    m_sourceHeight = sourceHeight;
    const QString previousOutputDirectory = m_outputDirectory;
    configurePaths(m_sourcePath);
    if (m_outputDirectory != previousOutputDirectory) {
        m_rallies.clear();
        m_etaText.clear();
        m_progress = 0.0;
        emit changed();
    }
    m_maxRallies = 0;
    loadProgress();
    loadRallies();
}

void TrialAnalysisController::startTrial(
    const QString &sourcePath, qint64 sourceDurationMs,
    int sourceWidth, int sourceHeight, int maxRallies)
{
    beginAnalysis(
        sourcePath, sourceDurationMs, sourceWidth, sourceHeight,
        qBound(1, maxRallies, 10), false);
}

void TrialAnalysisController::startFullAnalysis(
    const QString &sourcePath, qint64 sourceDurationMs,
    int sourceWidth, int sourceHeight)
{
    beginAnalysis(sourcePath, sourceDurationMs, sourceWidth, sourceHeight, 0, true);
}

void TrialAnalysisController::beginAnalysis(
    const QString &sourcePath, qint64 sourceDurationMs,
    int sourceWidth, int sourceHeight, int maxRallies, bool fullAnalysis)
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
    m_sourceWidth = sourceWidth;
    m_sourceHeight = sourceHeight;
    m_maxRallies = maxRallies;
    m_fullAnalysis = fullAnalysis;
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
    if (m_maxRallies > 0 && m_rallies.size() >= m_maxRallies) {
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
    m_fullAnalysis = false;
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

bool TrialAnalysisController::exportRally(
    int index, const QUrl &folderUrl, const QString &quality)
{
    if (exporting()) {
        m_actionMessage = QStringLiteral("已有一个回合正在导出");
        emit changed();
        return false;
    }
    if (index < 0 || index >= m_rallies.size() || !folderUrl.isLocalFile())
        return false;
    const QVariantMap rally = m_rallies.at(index).toMap();
    const QString clipPath = rally.value(QStringLiteral("clipPath")).toString();
    const QFileInfo clip(clipPath);
    QDir folder(folderUrl.toLocalFile());
    if (!folder.exists() || !QFileInfo::exists(m_sourcePath)) {
        m_actionMessage = QStringLiteral("导出失败：源短片或目标文件夹不存在");
        emit changed();
        return false;
    }

    const QString qualityName = quality == QStringLiteral("source")
        ? QStringLiteral("original") : quality;
    const int rallyNumber = rally.value(QStringLiteral("rally"), index + 1).toInt();
    const QString baseName = QStringLiteral("rally-%1-%2")
                                 .arg(rallyNumber, 4, 10, QLatin1Char('0'))
                                 .arg(qualityName);
    QString destination = folder.filePath(baseName + QStringLiteral(".mp4"));
    int suffix = 2;
    while (QFileInfo::exists(destination)) {
        destination = folder.filePath(QStringLiteral("%1-%2.mp4").arg(baseName).arg(suffix++));
    }

    if (quality == QStringLiteral("720p") && m_sourceHeight >= 720 && clip.exists()) {
        const bool copied = QFile::copy(clip.absoluteFilePath(), destination);
        m_actionMessage = copied
            ? QStringLiteral("已导出 720p 快速版：%1").arg(QDir::toNativeSeparators(destination))
            : QStringLiteral("导出失败：无法复制短片");
        m_exportProgress = copied ? 1.0 : 0.0;
        emit changed();
        return copied;
    }

    const double start = qMax(0.0, rally.value(QStringLiteral("startSeconds")).toDouble() - 0.5);
    const double end = rally.value(QStringLiteral("endSeconds")).toDouble() + 0.8;
    const double duration = qMax(0.1, end - start);
    m_exportDurationMs = qRound64(duration * 1000.0);
    m_exportProgress = 0.0;
    m_logTail.clear();
    m_exportFinalPath = destination;
    m_exportTemporaryPath = folder.filePath(baseName + QStringLiteral(".part.mp4"));
    QFile::remove(m_exportTemporaryPath);

    QStringList arguments = {
        QStringLiteral("-hide_banner"), QStringLiteral("-y"), QStringLiteral("-nostdin"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-progress"), QStringLiteral("pipe:1"),
        QStringLiteral("-ss"), QString::number(start, 'f', 3),
        QStringLiteral("-i"), m_sourcePath,
        QStringLiteral("-t"), QString::number(duration, 'f', 3),
        QStringLiteral("-map"), QStringLiteral("0:v:0"),
        QStringLiteral("-map"), QStringLiteral("0:a:0?")
    };
    if (quality == QStringLiteral("1080p") && m_sourceHeight > 1080)
        arguments << QStringLiteral("-vf") << QStringLiteral("scale=-2:1080:flags=lanczos");
    else if (quality == QStringLiteral("720p") && m_sourceHeight > 720)
        arguments << QStringLiteral("-vf") << QStringLiteral("scale=-2:720:flags=lanczos");

    arguments << QStringLiteral("-c:v") << QStringLiteral("libx264")
              << QStringLiteral("-preset") << QStringLiteral("fast")
              << QStringLiteral("-crf")
              << (quality == QStringLiteral("source") ? QStringLiteral("16") : QStringLiteral("18"))
              << QStringLiteral("-c:a") << QStringLiteral("aac")
              << QStringLiteral("-b:a") << QStringLiteral("192k")
              << QStringLiteral("-movflags") << QStringLiteral("+faststart")
              << m_exportTemporaryPath;

    m_actionMessage = quality == QStringLiteral("source")
        ? QStringLiteral("正在按原始分辨率导出最高质量版本…")
        : QStringLiteral("正在导出 %1 版本…").arg(quality);
    m_exportProcess.setWorkingDirectory(m_projectRoot);
    m_exportProcess.setProgram(m_ffmpegPath);
    m_exportProcess.setArguments(arguments);
    m_exportProcess.start();
    emit changed();
    return true;
}

bool TrialAnalysisController::exportAssembly(
    const QVariantList &rows, const QUrl &folderUrl,
    const QString &quality, const QString &baseName)
{
    if (exporting()) {
        m_actionMessage = QStringLiteral("已有一个视频正在导出");
        emit changed();
        return false;
    }
    if (rows.isEmpty() || !folderUrl.isLocalFile() || !QFileInfo::exists(m_sourcePath)) {
        m_actionMessage = QStringLiteral("导出失败：没有可拼接回合或源视频不存在");
        emit changed();
        return false;
    }

    QDir folder(folderUrl.toLocalFile());
    if (!folder.exists()) {
        m_actionMessage = QStringLiteral("导出失败：目标文件夹不存在");
        emit changed();
        return false;
    }

    QVariantList validRows;
    qint64 totalDurationMs = 0;
    const double sourceDuration = qMax(0.1, m_sourceDurationMs / 1000.0);
    for (const QVariant &value : rows) {
        const QVariantMap row = value.toMap();
        if (!row.value(QStringLiteral("selected")).toBool())
            continue;
        const double start = qBound(0.0, row.value(QStringLiteral("startSeconds")).toDouble(), sourceDuration);
        const double end = qBound(0.0, row.value(QStringLiteral("endSeconds")).toDouble(), sourceDuration);
        if (end <= start + 0.05)
            continue;
        QVariantMap normalized = row;
        normalized.insert(QStringLiteral("startSeconds"), start);
        normalized.insert(QStringLiteral("endSeconds"), end);
        validRows.append(normalized);
        totalDurationMs += qRound64((end - start) * 1000.0);
    }
    if (validRows.isEmpty()) {
        m_actionMessage = QStringLiteral("导出失败：请至少保留一个有效回合");
        emit changed();
        return false;
    }

    QString safeBaseName = baseName.trimmed();
    if (safeBaseName.isEmpty())
        safeBaseName = QStringLiteral("羽毛球回合合集");
    safeBaseName.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|]+)")), QStringLiteral("_"));
    safeBaseName = safeBaseName.left(80);
    const QString qualityName = quality == QStringLiteral("source")
        ? QStringLiteral("original") : quality;
    const QString fileStem = safeBaseName + QStringLiteral("-") + qualityName;
    QString destination = folder.filePath(fileStem + QStringLiteral(".mp4"));
    int suffix = 2;
    while (QFileInfo::exists(destination))
        destination = folder.filePath(QStringLiteral("%1-%2.mp4").arg(fileStem).arg(suffix++));

    // Probe once so videos without an audio stream can still be concatenated.
    bool hasAudio = true;
    const QString ffprobePath = QFileInfo(m_ffmpegPath).dir().filePath(QStringLiteral("ffprobe.exe"));
    if (QFileInfo::exists(ffprobePath)) {
        QProcess probe;
        probe.setProgram(ffprobePath);
        probe.setArguments({
            QStringLiteral("-v"), QStringLiteral("error"),
            QStringLiteral("-select_streams"), QStringLiteral("a:0"),
            QStringLiteral("-show_entries"), QStringLiteral("stream=index"),
            QStringLiteral("-of"), QStringLiteral("csv=p=0"), m_sourcePath
        });
        probe.start();
        if (probe.waitForFinished(3000))
            hasAudio = !QString::fromUtf8(probe.readAllStandardOutput()).trimmed().isEmpty();
    }

    const int count = validRows.size();
    QStringList filterParts;
    QStringList videoSources;
    for (int i = 0; i < count; ++i)
        videoSources << QStringLiteral("[vsrc%1]").arg(i);
    filterParts << QStringLiteral("[0:v:0]split=%1%2")
                       .arg(count).arg(videoSources.join(QString()));

    QStringList audioSources;
    if (hasAudio) {
        for (int i = 0; i < count; ++i)
            audioSources << QStringLiteral("[asrc%1]").arg(i);
        filterParts << QStringLiteral("[0:a:0]asplit=%1%2")
                           .arg(count).arg(audioSources.join(QString()));
    }

    QStringList concatInputs;
    for (int i = 0; i < count; ++i) {
        const QVariantMap row = validRows.at(i).toMap();
        const QString start = QString::number(row.value(QStringLiteral("startSeconds")).toDouble(), 'f', 3);
        const QString end = QString::number(row.value(QStringLiteral("endSeconds")).toDouble(), 'f', 3);
        filterParts << QStringLiteral("[vsrc%1]trim=start=%2:end=%3,setpts=PTS-STARTPTS[v%1]")
                           .arg(i).arg(start).arg(end);
        concatInputs << QStringLiteral("[v%1]").arg(i);
        if (hasAudio) {
            filterParts << QStringLiteral("[asrc%1]atrim=start=%2:end=%3,asetpts=PTS-STARTPTS[a%1]")
                               .arg(i).arg(start).arg(end);
            concatInputs << QStringLiteral("[a%1]").arg(i);
        }
    }
    if (hasAudio) {
        filterParts << concatInputs.join(QString())
                           + QStringLiteral("concat=n=%1:v=1:a=1[vcat][acat]").arg(count);
    } else {
        filterParts << concatInputs.join(QString())
                           + QStringLiteral("concat=n=%1:v=1:a=0[vcat]").arg(count);
    }

    QString videoLabel = QStringLiteral("[vcat]");
    if (quality == QStringLiteral("1080p") && m_sourceHeight > 1080) {
        filterParts << QStringLiteral("[vcat]scale=-2:1080:flags=lanczos[vout]");
        videoLabel = QStringLiteral("[vout]");
    } else if (quality == QStringLiteral("720p") && m_sourceHeight > 720) {
        filterParts << QStringLiteral("[vcat]scale=-2:720:flags=lanczos[vout]");
        videoLabel = QStringLiteral("[vout]");
    }

    m_exportDurationMs = totalDurationMs;
    m_exportProgress = 0.0;
    m_logTail.clear();
    m_exportFinalPath = destination;
    m_exportTemporaryPath = folder.filePath(fileStem + QStringLiteral(".part.mp4"));
    QFile::remove(m_exportTemporaryPath);

    QStringList arguments = {
        QStringLiteral("-hide_banner"), QStringLiteral("-y"), QStringLiteral("-nostdin"),
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-progress"), QStringLiteral("pipe:1"),
        QStringLiteral("-i"), m_sourcePath,
        QStringLiteral("-filter_complex"), filterParts.join(QStringLiteral(";")),
        QStringLiteral("-map"), videoLabel
    };
    if (hasAudio)
        arguments << QStringLiteral("-map") << QStringLiteral("[acat]");
    arguments << QStringLiteral("-c:v") << QStringLiteral("libx264")
              << QStringLiteral("-preset") << QStringLiteral("fast")
              << QStringLiteral("-crf")
              << (quality == QStringLiteral("source") ? QStringLiteral("16") : QStringLiteral("18"))
              << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
    if (hasAudio)
        arguments << QStringLiteral("-c:a") << QStringLiteral("aac")
                  << QStringLiteral("-b:a") << QStringLiteral("192k")
                  << QStringLiteral("-shortest");
    arguments << QStringLiteral("-movflags") << QStringLiteral("+faststart")
              << m_exportTemporaryPath;

    m_actionMessage = QStringLiteral("正在拼接 %1 个回合并导出 %2 版本…")
                          .arg(validRows.size()).arg(qualityName);
    m_exportProcess.setWorkingDirectory(m_projectRoot);
    m_exportProcess.setProgram(m_ffmpegPath);
    m_exportProcess.setArguments(arguments);
    m_exportProcess.start();
    emit changed();
    return true;
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
    m_stageText = m_fullAnalysis
        ? QStringLiteral("TrackNet 正在分析完整视频")
        : QStringLiteral("TrackNet 正在识别前 %1 个回合").arg(m_maxRallies);
    m_detailText = QStringLiteral("每识别完一个完整回合，就会立即出现在右侧");

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString existingPythonPath = environment.value(QStringLiteral("PYTHONPATH"));
    environment.insert(
        QStringLiteral("PYTHONPATH"),
        existingPythonPath.isEmpty()
            ? m_vendorPath
            : m_vendorPath + QDir::listSeparator() + existingPythonPath);

    QStringList arguments = {
        m_runnerPath,
        QStringLiteral("--video"), m_proxyPath,
        QStringLiteral("--tracknet-file"), m_modelPath,
        QStringLiteral("--output-dir"), m_analysisDirectory,
        QStringLiteral("--chunk-seconds"), QStringLiteral("6"),
        QStringLiteral("--overlap-seconds"), QStringLiteral("0.5"),
        QStringLiteral("--batch-size"), QStringLiteral("8"),
        QStringLiteral("--eval-mode"), QStringLiteral("nonoverlap"),
        QStringLiteral("--ffmpeg"), m_ffmpegPath
    };
    if (m_maxRallies > 0)
        arguments << QStringLiteral("--max-rallies") << QString::number(m_maxRallies);

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

void TrialAnalysisController::readExportOutput()
{
    const QString output = QString::fromUtf8(m_exportProcess.readAllStandardOutput());
    for (const QString &line : output.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (!line.startsWith(QStringLiteral("out_time_")) || m_exportDurationMs <= 0)
            continue;
        const qsizetype equals = line.indexOf(QLatin1Char('='));
        bool ok = false;
        const qint64 microseconds = line.mid(equals + 1).toLongLong(&ok);
        if (ok)
            m_exportProgress = qBound(0.0, microseconds / 1000.0 / m_exportDurationMs, 0.99);
    }
    emit changed();
}

void TrialAnalysisController::exportFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    const bool succeeded = exitStatus == QProcess::NormalExit && exitCode == 0;
    if (!succeeded) {
        QFile::remove(m_exportTemporaryPath);
        m_exportProgress = 0.0;
        m_actionMessage = QStringLiteral("回合导出失败：%1").arg(m_logTail.right(600));
        emit changed();
        return;
    }
    if (!QFile::rename(m_exportTemporaryPath, m_exportFinalPath)) {
        m_exportProgress = 0.0;
        m_actionMessage = QStringLiteral("导出完成，但无法写入目标文件");
        emit changed();
        return;
    }
    m_exportProgress = 1.0;
    m_actionMessage = QStringLiteral("已导出：%1").arg(QDir::toNativeSeparators(m_exportFinalPath));
    emit changed();
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
        m_detailText = m_fullAnalysis
            ? QStringLiteral("完整视频分析完成，共识别 %1 个回合").arg(m_rallies.size())
            : QStringLiteral("已生成 %1 个可预览回合").arg(m_rallies.size());
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
    if (!running()) {
        const QString status = object.value(QStringLiteral("status")).toString();
        if (status == QStringLiteral("complete")) {
            m_stage = Stage::Complete;
            m_state = QStringLiteral("complete");
            m_stageText = QStringLiteral("已恢复完整分析结果");
            m_detailText = QStringLiteral("可以继续查看和导出回合");
        } else if (status == QStringLiteral("paused") || status == QStringLiteral("running")) {
            m_stage = Stage::Paused;
            m_state = QStringLiteral("paused");
            m_stageText = QStringLiteral("已恢复上次分析进度");
            m_detailText = QStringLiteral("点击继续即可从已完成分块恢复");
            m_etaText.clear();
        } else if (status == QStringLiteral("sample_complete")) {
            m_stage = Stage::Complete;
            m_state = QStringLiteral("complete");
            m_stageText = QStringLiteral("已恢复切分试验结果");
            m_detailText = QStringLiteral("可继续分析完整视频");
        }
    }
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
        if (m_maxRallies > 0 && loaded.size() >= m_maxRallies)
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
        m_detailText = m_maxRallies > 0
            ? QStringLiteral("已发现 %1 / %2 个回合")
                  .arg(m_rallies.size()).arg(m_maxRallies)
            : QStringLiteral("已识别 %1 个回合").arg(m_rallies.size());
        emit changed();
    }
}

void TrialAnalysisController::fail(const QString &message)
{
    m_stage = Stage::Error;
    m_state = QStringLiteral("error");
    m_stageText = m_fullAnalysis
        ? QStringLiteral("完整分析未完成")
        : QStringLiteral("切分试验未完成");
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
