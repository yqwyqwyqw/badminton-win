// Integration check: drive the real TrialAnalysisController (the class the QML
// layer talks to) through a complete analysis with no Python involved, then report
// what landed on disk.  This is the evidence that Milestone A works end to end:
// the controller generates the proxy with ffmpeg, runs the in-process ONNX kernel,
// and the QML-facing contract files (progress.json / rally-feed.json / clips) are
// produced by the C++ path.
//
// Dev-only tool; links the product sources.

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include "TrialAnalysisController.h"
#include "job_runner.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("BadmintonAnalyzer"));
    // Deliberately the product's own application name: the GUI writes its cache
    // under a non-ASCII directory, and a narrow-path mistake in the kernel only
    // shows up there (that is how the "progress bar never moves" bug slipped
    // through).  Override with BADMINTON_APP_NAME to test another layout.
    const QByteArray appNameOverride = qgetenv("BADMINTON_APP_NAME");
    QCoreApplication::setApplicationName(appNameOverride.isEmpty()
                                             ? QStringLiteral("羽毛球回合分析器")
                                             : QString::fromUtf8(appNameOverride));

    if (argc < 2) {
        out() << "usage: integration_check <source-video> [durationMs] [width] [height]"
              << Qt::endl;
        return 2;
    }
    const QString source = QString::fromLocal8Bit(argv[1]);
    const qint64 durationMs = argc > 2 ? QString::fromLocal8Bit(argv[2]).toLongLong() : 56460;
    const int width = argc > 3 ? QString::fromLocal8Bit(argv[3]).toInt() : 3840;
    const int height = argc > 4 ? QString::fromLocal8Bit(argv[4]).toInt() : 2160;

    // ---- preflight: kernel paths must survive a non-ASCII directory ---------
    // The kernel carries paths as UTF-8 std::string.  On Windows any stray
    // fs::path(narrow string) decodes them with the ANSI code page and silently
    // creates a *different* directory -- exactly how the GUI ended up watching an
    // empty cache while the inference wrote into a garbage-named one.  Assert on
    // the directory that actually appeared next to the requested one.
    bool path_ok = true;
    {
        const QString probeParent = QDir::tempPath();
        const QString probeName = QStringLiteral("羽毛球路径检查");
        const QString probeDir = QDir(probeParent).filePath(probeName);
        QDir(probeDir).removeRecursively();
        const QStringList before = QDir(probeParent).entryList(QDir::Dirs | QDir::NoDotAndDotDot);

        const std::string probeUtf8 = QDir::fromNativeSeparators(probeDir).toUtf8().toStdString();
        const std::filesystem::path kernelPath = badminton::inference::ChunkCsvPath(probeUtf8, 0);
        std::filesystem::create_directories(kernelPath.parent_path());
        {
            std::ofstream stream(kernelPath, std::ios::binary | std::ios::trunc);
            stream << "Frame,Visibility,X,Y\n";
        }

        QStringList created;
        for (const QString &entry : QDir(probeParent).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (!before.contains(entry))
                created << entry;
        }
        const QString expectedFile = QDir(probeDir).filePath(QStringLiteral("chunks/chunk-00000.csv"));
        const bool singleDir = created.size() == 1 && created.first() == probeName;
        const bool fileOk = QFileInfo::exists(expectedFile);
        path_ok = singleDir && fileOk;
        out() << "path preflight: " << (path_ok ? "PASS" : "FAIL")
              << "  created=" << created.join(QStringLiteral(", "))
              << "  file=" << (fileOk ? QStringLiteral("ok") : QStringLiteral("MISSING"))
              << Qt::endl;
        out().flush();
        QDir(probeDir).removeRecursively();
    }

    TrialAnalysisController controller;
    QString lastStage;
    qreal lastProgress = 0.0;

    // `progress` is the exact property the QML progress bar binds to, so printing
    // it here proves the bar moves during the run (the bug this guards against
    // left it pinned at 0% while the GPU was busy).
    QObject::connect(&controller, &TrialAnalysisController::changed, [&]() {
        const QString stage = controller.stageText();
        const qreal progress = controller.progress();
        if (stage != lastStage) {
            lastStage = stage;
            out() << "  [" << controller.state() << "] " << stage
                  << QStringLiteral("  (progress %1%)").arg(progress * 100.0, 0, 'f', 1)
                  << Qt::endl;
            out().flush();
            lastProgress = progress;
        } else if (progress - lastProgress >= 0.05) {
            lastProgress = progress;
            out() << "  ...progress " << QString::number(progress * 100.0, 'f', 1) << "%"
                  << Qt::endl;
            out().flush();
        }
    });

    QElapsedTimer timer;
    timer.start();
    out() << "source: " << source << Qt::endl;
    out() << "starting full analysis (no Python in the loop)" << Qt::endl;
    out().flush();
    controller.startFullAnalysis(source, durationMs, width, height);

    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, [&]() {
        out() << "TIMEOUT after 15 minutes" << Qt::endl;
        controller.cancel();
        app.exit(3);
    });
    // Stop the event loop once the controller settles.
    QTimer poll;
    poll.setInterval(250);
    QObject::connect(&poll, &QTimer::timeout, [&]() {
        const QString state = controller.state();
        if (state == QStringLiteral("complete") || state == QStringLiteral("error")
            || state == QStringLiteral("paused")) {
            app.quit();
        }
    });
    deadline.start(15 * 60 * 1000);
    poll.start();
    app.exec();

    out() << "final state: " << controller.state() << "  elapsed "
          << timer.elapsed() / 1000.0 << "s" << Qt::endl;
    if (!controller.errorMessage().isEmpty())
        out() << "error: " << controller.errorMessage() << Qt::endl;

    const QString output = controller.outputDirectory();
    out() << "output dir: " << output << Qt::endl;
    const QDir outputDir(output);
    const QDir tracknet(outputDir.filePath(QStringLiteral("tracknet")));
    const QStringList expected = {
        QStringLiteral("proxy-720p.mp4"),
        QStringLiteral("tracknet/progress.json"),
        QStringLiteral("tracknet/rally-feed.json"),
        QStringLiteral("tracknet/rally-index.csv"),
        QStringLiteral("tracknet/trajectory-partial.csv"),
        QStringLiteral("tracknet/chunks/chunk-00000.csv"),
        QStringLiteral("tracknet/analysis/rallies-provisional.csv"),
        QStringLiteral("tracknet/analysis/hits-provisional.csv"),
    };
    int missing = 0;
    for (const QString &relative : expected) {
        const bool exists = QFileInfo::exists(outputDir.filePath(relative));
        if (!exists)
            ++missing;
        out() << (exists ? "  ok   " : "  MISS ") << relative << Qt::endl;
    }

    const QFileInfo feed(tracknet.filePath(QStringLiteral("rally-feed.json")));
    if (feed.exists()) {
        QFile file(feed.absoluteFilePath());
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
            const QJsonArray rallies = document.array();
            out() << "rallies in rally-feed.json: " << rallies.size() << Qt::endl;
            for (const QJsonValue &value : rallies) {
                const QJsonObject rally = value.toObject();
                const QString clip = rally.value(QStringLiteral("clip")).toString();
                out() << "  rally " << rally.value(QStringLiteral("rally")).toInt()
                      << "  " << rally.value(QStringLiteral("startSeconds")).toDouble()
                      << "s - " << rally.value(QStringLiteral("endSeconds")).toDouble()
                      << "s  hits=" << rally.value(QStringLiteral("hitCount")).toInt()
                      << "  clip=" << (clip.isEmpty() ? QStringLiteral("(none)") : clip)
                      << (clip.isEmpty() || QFileInfo::exists(clip) ? QString() : QStringLiteral(" MISSING"))
                      << Qt::endl;
            }
        }
    }

    const QFileInfo progress(tracknet.filePath(QStringLiteral("progress.json")));
    if (progress.exists()) {
        QFile file(progress.absoluteFilePath());
        if (file.open(QIODevice::ReadOnly))
            out() << "progress.json: " << QString::fromUtf8(file.readAll()).simplified() << Qt::endl;
    }

    const bool ok = controller.state() == QStringLiteral("complete") && missing == 0
                    && controller.rallyCount() > 0;
    out() << "result(analysis): " << (ok ? "PASS" : "FAIL") << Qt::endl;

    // Export step: the same controller calls the QML export page uses.  "720p"
    // exercises the clip-copy path, "1080p" the re-encode + scale path (the 4K
    // "original" path is the same code but takes minutes on a 23 s rally).
    bool export_ok = true;
    if (controller.rallyCount() >= 2) {
        const QString exportDir = QDir::tempPath() + QStringLiteral("/badminton-export-check");
        QDir().mkpath(exportDir);
        const struct { int index; QString quality; QString suffix; } jobs[] = {
            {0, QStringLiteral("720p"), QStringLiteral("720p")},
            {1, QStringLiteral("1080p"), QStringLiteral("1080p")},
        };
        for (const auto &job : jobs) {
            const QString base = QStringLiteral("rally-%1-%2.mp4")
                                     .arg(job.index + 1, 4, 10, QLatin1Char('0'))
                                     .arg(job.suffix);
            // Clean first: the controller avoids overwriting by appending a suffix.
            for (const QString &candidate : {base,
                                             QStringLiteral("rally-%1-%2-2.mp4")
                                                 .arg(job.index + 1, 4, 10, QLatin1Char('0'))
                                                 .arg(job.suffix)}) {
                QFile::remove(QDir(exportDir).filePath(candidate));
            }
            const bool started = controller.exportRally(job.index, QUrl::fromLocalFile(exportDir),
                                                        job.quality);
            const QString expected = QDir(exportDir).filePath(base);
            QElapsedTimer exportTimer;
            exportTimer.start();
            while (controller.exporting() && exportTimer.elapsed() < 240000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            const QFileInfo file(expected);
            const bool good = started && file.exists() && file.size() > 1024;
            export_ok = export_ok && good;
            out() << "export " << job.quality << ": " << (good ? "ok" : "FAIL") << "  "
                  << expected << "  " << file.size() / 1024 << " KB  ("
                  << exportTimer.elapsed() / 1000.0 << "s)" << Qt::endl;
            out() << "  message: " << controller.actionMessage() << Qt::endl;
            out().flush();
        }
    }
    out() << "result(export): " << (export_ok ? "PASS" : "FAIL") << Qt::endl;

    // reset()（界面上是「新建项目」）必须把本项目的分析缓存清掉：代理、分块轨迹、
    // 短片、报告都在这个目录里。清理是延迟执行的（等分析线程收尾），所以这里轮询一下。
    const QString cacheDir = controller.outputDirectory();
    const bool cacheExisted = QFileInfo::exists(cacheDir) && !cacheDir.isEmpty();
    controller.reset();
    // 清理是延迟执行的（等分析线程收尾），这里按真实时间等待定时器触发；
    // processEvents 在没有事件时会立刻返回，所以必须配 msleep。
    QElapsedTimer cleanupWait;
    cleanupWait.start();
    while (QFileInfo::exists(cacheDir) && cleanupWait.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(50);
    }
    const bool cleanupOk = cacheExisted && !QFileInfo::exists(cacheDir);
    out() << "reset() 清理缓存: " << (cleanupOk ? "ok" : "FAIL") << "  缓存目录=" << cacheDir
          << "  提示=" << controller.actionMessage() << Qt::endl;

    const bool all_ok = ok && export_ok && path_ok && cleanupOk;
    out() << "result(path-utf8): " << (path_ok ? "PASS" : "FAIL") << Qt::endl;
    out() << "result(reset-cleanup): " << (cleanupOk ? "PASS" : "FAIL") << Qt::endl;
    out() << "result: " << (all_ok ? "PASS" : "FAIL") << Qt::endl;
    return all_ok ? 0 : 1;
}
