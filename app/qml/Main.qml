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

    property int currentStep: 1
    property int selectedRally: 0
    property bool analysisPaused: false

    ListModel {
        id: rallyModel

        ListElement { rallyNumber: 1; timeRange: "00:03 - 00:07"; autoHits: 3; effectiveHits: 3; reviewState: "已识别" }
        ListElement { rallyNumber: 2; timeRange: "00:10 - 00:15"; autoHits: 5; effectiveHits: 4; reviewState: "已人工修正" }
        ListElement { rallyNumber: 3; timeRange: "00:26 - 00:30"; autoHits: 4; effectiveHits: 2; reviewState: "已人工修正" }
        ListElement { rallyNumber: 4; timeRange: "00:36 - 00:41"; autoHits: 6; effectiveHits: 4; reviewState: "待复核" }
        ListElement { rallyNumber: 5; timeRange: "00:45 - 00:53"; autoHits: 6; effectiveHits: 6; reviewState: "已识别" }
    }

    header: AppHeader {
        projectName: qsTr("序列1 · 演示工程")
        statusText: analysisPaused ? qsTr("分析已暂停") : qsTr("正在分析 · 38%")
        statusActive: !analysisPaused
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

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 14

                AnalysisStatusCard {
                    Layout.fillWidth: true
                    progress: 0.38
                    processedText: qsTr("已解析 02:18 / 06:04")
                    rallyCount: rallyModel.count
                    paused: window.analysisPaused
                    onPauseToggled: window.analysisPaused = !window.analysisPaused
                }

                VideoPreview {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    rallyNumber: rallyModel.get(window.selectedRally).rallyNumber
                    timeRange: rallyModel.get(window.selectedRally).timeRange
                }

                TimelinePlaceholder {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 104
                    selectedRally: window.selectedRally
                    rallyCount: rallyModel.count
                }
            }
        }

        RallyListPanel {
            Layout.preferredWidth: 340
            Layout.fillHeight: true
            model: rallyModel
            selectedIndex: window.selectedRally
            onRallySelected: function(index) { window.selectedRally = index }
            onHitCountChanged: function(index, count) {
                rallyModel.setProperty(index, "effectiveHits", count)
                rallyModel.setProperty(index, "reviewState", "已人工修正")
            }
        }
    }
}
