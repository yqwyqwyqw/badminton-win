#include "VideoImportController.h"

#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QMediaFormat>
#include <QMediaMetaData>
#include <QSet>
#include <QSize>
#include <QRegularExpression>

namespace {
const QSet<QString> supportedExtensions = {
    QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv"),
    QStringLiteral("m4v"), QStringLiteral("avi"), QStringLiteral("webm")
};

QDateTime dateFromFileName(const QString &fileName)
{
    static const QList<QRegularExpression> patterns = {
        QRegularExpression(QStringLiteral(
            R"((?:^|\D)((?:19|20)\d{2})(\d{2})(\d{2})[_-]?(\d{2})(\d{2})(\d{2})(?:\D|$))")),
        QRegularExpression(QStringLiteral(
            R"((?:^|\D)((?:19|20)\d{2})[-_.](\d{2})[-_.](\d{2})[_ -](\d{2})(\d{2})(\d{2})(?:\D|$))"))
    };
    for (const QRegularExpression &pattern : patterns) {
        const QRegularExpressionMatch match = pattern.match(fileName);
        if (!match.hasMatch())
            continue;
        const QDateTime value(
            QDate(match.captured(1).toInt(), match.captured(2).toInt(), match.captured(3).toInt()),
            QTime(match.captured(4).toInt(), match.captured(5).toInt(), match.captured(6).toInt()));
        if (value.isValid())
            return value;
    }
    return {};
}
}

VideoImportController::VideoImportController(QObject *parent)
    : QObject(parent)
{
    connect(&m_player, &QMediaPlayer::metaDataChanged, this, [this]() {
        refreshMetadata();
        emit changed();
    });
    connect(&m_player, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        if (duration > 0) {
            m_durationMs = duration;
            m_durationText = formatDuration(duration);
        }
        emit changed();
    });
    connect(&m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) {
            refreshMetadata();
            m_loading = false;
            m_ready = true;
            m_statusText = QStringLiteral("素材已导入");
            m_errorMessage.clear();
            emit changed();
        } else if (status == QMediaPlayer::InvalidMedia && hasVideo()) {
            fail(m_player.errorString().isEmpty()
                     ? QStringLiteral("无法读取该视频，请检查格式或文件是否损坏")
                     : m_player.errorString());
        }
    });
    connect(&m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString &message) {
        if (error != QMediaPlayer::NoError && hasVideo())
            fail(message.isEmpty() ? QStringLiteral("视频读取失败") : message);
    });
}

void VideoImportController::importVideo(const QUrl &url)
{
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        fail(QStringLiteral("文件不存在或不是有效文件"));
        return;
    }
    if (!supportedExtensions.contains(info.suffix().toLower())) {
        fail(QStringLiteral("暂不支持 .%1 格式").arg(info.suffix()));
        return;
    }

    m_player.stop();
    m_ready = false;
    m_loading = true;
    m_durationMs = 0;
    m_sourceWidth = 0;
    m_sourceHeight = 0;
    m_filePath = QDir::toNativeSeparators(info.absoluteFilePath());
    m_fileName = info.fileName();
    m_fileSizeText = formatBytes(info.size());
    m_durationText = QStringLiteral("读取中…");
    m_resolutionText = QStringLiteral("读取中…");
    m_frameRateText = QStringLiteral("读取中…");
    m_codecText = QStringLiteral("读取中…");
    const QDateTime fileNameDate = dateFromFileName(info.completeBaseName());
    if (fileNameDate.isValid()) {
        m_hasPreferredDate = true;
        m_originalDateText = fileNameDate.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        m_originalDateSourceText = QStringLiteral("文件名时间");
    } else {
        m_hasPreferredDate = false;
        QDateTime fileDate = info.birthTime();
        m_originalDateSourceText = QStringLiteral("文件创建时间");
        if (!fileDate.isValid()) {
            fileDate = info.lastModified();
            m_originalDateSourceText = QStringLiteral("文件修改时间");
        }
        m_originalDateText = fileDate.isValid()
            ? fileDate.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : QStringLiteral("—");
    }
    m_statusText = QStringLiteral("正在读取素材信息");
    m_errorMessage.clear();
    emit changed();

    m_player.setSource(QUrl::fromLocalFile(info.absoluteFilePath()));
}

void VideoImportController::clear()
{
    m_player.stop();
    m_player.setSource({});
    m_ready = false;
    m_loading = false;
    m_durationMs = 0;
    m_sourceWidth = 0;
    m_sourceHeight = 0;
    m_filePath.clear();
    m_fileName.clear();
    m_fileSizeText = QStringLiteral("—");
    m_durationText = QStringLiteral("—");
    m_resolutionText = QStringLiteral("—");
    m_frameRateText = QStringLiteral("—");
    m_codecText = QStringLiteral("—");
    m_originalDateText = QStringLiteral("—");
    m_originalDateSourceText.clear();
    m_hasPreferredDate = false;
    m_statusText = QStringLiteral("等待导入视频");
    m_errorMessage.clear();
    emit changed();
}

void VideoImportController::refreshMetadata()
{
    const QMediaMetaData metadata = m_player.metaData();
    const QSize resolution = metadata.value(QMediaMetaData::Resolution).toSize();
    if (resolution.isValid()) {
        m_sourceWidth = resolution.width();
        m_sourceHeight = resolution.height();
        m_resolutionText = QStringLiteral("%1 × %2").arg(resolution.width()).arg(resolution.height());
    }

    const double frameRate = metadata.value(QMediaMetaData::VideoFrameRate).toDouble();
    if (frameRate > 0)
        m_frameRateText = QStringLiteral("%1 fps").arg(frameRate, 0, 'f', frameRate < 100 ? 2 : 1);

    const QVariant codecValue = metadata.value(QMediaMetaData::VideoCodec);
    if (codecValue.isValid()) {
        const auto codec = codecValue.value<QMediaFormat::VideoCodec>();
        const QString codecName = QMediaFormat::videoCodecName(codec);
        if (!codecName.isEmpty())
            m_codecText = codecName;
    }

    if (!m_hasPreferredDate) {
        const QVariant dateValue = metadata.value(QMediaMetaData::Date);
        QDateTime recordedAt = dateValue.toDateTime();
        if (!recordedAt.isValid())
            recordedAt = QDateTime::fromString(dateValue.toString(), Qt::ISODate);
        if (recordedAt.isValid()) {
            if (recordedAt.timeSpec() == Qt::UTC || recordedAt.offsetFromUtc() != 0)
                recordedAt = recordedAt.toLocalTime();
            m_originalDateText = recordedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            m_originalDateSourceText = QStringLiteral("视频元数据");
            m_hasPreferredDate = true;
        } else {
            const QDate recordedDate = dateValue.toDate();
            if (recordedDate.isValid()) {
                m_originalDateText = recordedDate.toString(QStringLiteral("yyyy-MM-dd"));
                m_originalDateSourceText = QStringLiteral("视频元数据");
                m_hasPreferredDate = true;
            }
        }
    }

    const qint64 duration = m_player.duration();
    if (duration > 0) {
        m_durationMs = duration;
        m_durationText = formatDuration(duration);
    }
}

void VideoImportController::fail(const QString &message)
{
    m_loading = false;
    m_ready = false;
    m_statusText = QStringLiteral("导入失败");
    m_errorMessage = message;
    emit changed();
}

QString VideoImportController::formatBytes(qint64 bytes)
{
    static const QStringList units = {
        QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB")
    };
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < units.size() - 1) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, unit == 0 ? 'f' : 'f', unit == 0 ? 0 : 2)
        .arg(units.at(unit));
}

QString VideoImportController::formatDuration(qint64 milliseconds)
{
    const qint64 seconds = milliseconds / 1000;
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 remainingSeconds = seconds % 60;
    return hours > 0
        ? QStringLiteral("%1:%2:%3")
              .arg(hours, 2, 10, QLatin1Char('0'))
              .arg(minutes, 2, 10, QLatin1Char('0'))
              .arg(remainingSeconds, 2, 10, QLatin1Char('0'))
        : QStringLiteral("%1:%2")
              .arg(minutes, 2, 10, QLatin1Char('0'))
              .arg(remainingSeconds, 2, 10, QLatin1Char('0'));
}
