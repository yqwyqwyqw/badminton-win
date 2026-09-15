import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BadmintonAnalyzer

ApplicationWindow {
    id: window

    width: 1440
    height: 900
    minimumWidth: 1100
    minimumHeight: 700
    visible: true
    color: Theme.window
    title: qsTr("羽毛球回合分析器")
    font.family: "Microsoft YaHei UI"

    property int currentStep: 0
    property int selectedRally: 0

    onCurrentStepChanged: {
        if (currentStep >= 1 && videoImporter.ready) {
            trialAnalyzer.prepareSource(
                videoImporter.filePath,
                videoImporter.durationMs,
                videoImporter.sourceWidth,
                videoImporter.sourceHeight)
        }
    }

    header: AppHeader {
        projectName: videoImporter.hasVideo
                     ? videoImporter.fileName
                     : qsTr("尚未导入素材")
        statusText: window.currentStep === 0
                    ? (trialAnalyzer.actionMessage.length > 0
                       ? trialAnalyzer.actionMessage
                       : videoImporter.statusText)
                    : (window.currentStep === 1 || window.currentStep === 2
                       ? trialAnalyzer.stageText
                       : qsTr("等待拼接结果导出"))
        statusActive: window.currentStep === 0
                      ? videoImporter.ready
                      : (window.currentStep === 1 || window.currentStep === 2
                         ? trialAnalyzer.state !== "error"
                         : trialAnalyzer.hasResults)
        onNewProjectRequested: {
            trialAnalyzer.reset()
            videoImporter.clear()
            window.currentStep = 0
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        StepNavigation {
            Layout.preferredWidth: 232
            Layout.fillHeight: true
            currentStep: window.currentStep
            onStepSelected: function(index) { window.currentStep = index }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.window

            VideoImportPanel {
                anchors.fill: parent
                anchors.margins: 20
                visible: window.currentStep === 0
                importer: videoImporter
                onContinueRequested: window.currentStep = 1
            }

            RallyLibraryPanel {
                anchors.fill: parent
                anchors.margins: 20
                visible: window.currentStep === 1
                importer: videoImporter
                analyzer: trialAnalyzer
                selectedRally: window.selectedRally
                onPreviousRequested: window.selectedRally = Math.max(0, window.selectedRally - 1)
                onNextRequested: window.selectedRally = Math.min(
                                     trialAnalyzer.rallyCount - 1,
                                     window.selectedRally + 1)
            }

            RallyAssemblyPanel {
                id: assemblyPanel
                anchors.fill: parent
                anchors.margins: 20
                visible: window.currentStep === 2
                importer: videoImporter
                analyzer: trialAnalyzer
                onExportPushed: {
                    window.currentStep = 3
                }
            }

            VideoExportPanel {
                anchors.fill: parent
                anchors.margins: 20
                visible: window.currentStep === 3
                importer: videoImporter
                analyzer: trialAnalyzer
                assembly: assemblyPanel
                onReturnRequested: window.currentStep = 2
            }
        }

        TrialRallyPanel {
            Layout.preferredWidth: 340
            Layout.fillHeight: true
            visible: window.currentStep === 1
            analyzer: trialAnalyzer
            importer: videoImporter
            fullMode: true
            selectedIndex: window.selectedRally
            onRallySelected: function(index) { window.selectedRally = index }
        }
    }
}
