#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTimer>
#include <QUrl>

#include "VideoImportController.h"
#include "TrialAnalysisController.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("BadmintonAnalyzer"));
    QCoreApplication::setApplicationName(QStringLiteral("羽毛球回合分析器"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    VideoImportController videoImporter;
    TrialAnalysisController trialAnalyzer;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("videoImporter"), &videoImporter);
    engine.rootContext()->setContextProperty(QStringLiteral("trialAnalyzer"), &trialAnalyzer);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule(QStringLiteral("BadmintonAnalyzer"), QStringLiteral("Main"));

    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() > 1) {
        const QUrl startupVideo = QUrl::fromLocalFile(arguments.at(1));
        QTimer::singleShot(0, &videoImporter, [&videoImporter, startupVideo]() {
            videoImporter.importVideo(startupVideo);
        });
    }

    return app.exec();
}
