import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import BadmintonAnalyzer

Rectangle {
    id: root

    required property QtObject analyzer
    required property int selectedIndex
    property int exportIndex: -1
    signal rallySelected(int index)

    color: Theme.navigation
    border.color: Theme.border

    FolderDialog {
        id: exportFolder
        title: qsTr("选择回合短片导出目录")
        onAccepted: root.analyzer.exportRally(root.exportIndex, selectedFolder)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true

            ColumnLayout {
                spacing: 2
                Text {
                    text: qsTr("试验回合")
                    color: Theme.text
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Text {
                    text: qsTr("识别完成后逐个出现")
                    color: Theme.textMuted
                    font.pixelSize: 10
                }
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                Layout.preferredWidth: 40
                Layout.preferredHeight: 28
                radius: 14
                color: Theme.accentSoft
                Text {
                    anchors.centerIn: parent
                    text: root.analyzer.rallyCount
                    color: Theme.accent
                    font.pixelSize: 12
                    font.weight: Font.Bold
                }
            }
        }

        ListView {
            id: rallyList
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: root.analyzer.rallies
            spacing: 9
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }

            delegate: ItemDelegate {
                id: rallyDelegate
                required property int index
                required property var modelData
                width: rallyList.width
                height: 106
                hoverEnabled: true
                onClicked: root.rallySelected(index)

                background: Rectangle {
                    radius: Theme.radiusMedium
                    color: root.selectedIndex === rallyDelegate.index
                           ? Theme.panelHover
                           : (rallyDelegate.hovered ? Theme.panelRaised : Theme.panel)
                    border.color: root.selectedIndex === rallyDelegate.index ? Theme.accent : Theme.border
                }

                contentItem: ColumnLayout {
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: qsTr("回合 %1").arg(rallyDelegate.modelData.rally)
                            color: Theme.text
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: rallyDelegate.modelData.timeText
                            color: Theme.textMuted
                            font.pixelSize: 10
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: qsTr("自动拍数 %1").arg(rallyDelegate.modelData.hitCount)
                            color: Theme.textMuted
                            font.pixelSize: 10
                        }
                        Item { Layout.fillWidth: true }
                        Button {
                            Layout.preferredWidth: 58
                            Layout.preferredHeight: 28
                            text: qsTr("预览")
                            onClicked: root.rallySelected(rallyDelegate.index)
                        }
                        Button {
                            Layout.preferredWidth: 58
                            Layout.preferredHeight: 28
                            text: qsTr("导出")
                            onClicked: {
                                root.exportIndex = rallyDelegate.index
                                exportFolder.open()
                            }
                        }
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: rallyList.count === 0
                text: root.analyzer.running
                      ? qsTr("正在分析…")
                      : qsTr("尚无回合结果")
                color: Theme.textMuted
                font.pixelSize: 12
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 58
            radius: Theme.radiusMedium
            color: Theme.panel
            border.color: Theme.border

            Column {
                anchors.centerIn: parent
                spacing: 3
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("短片用于快速复核")
                    color: Theme.text
                    font.pixelSize: 10
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("正式成片后续从原视频导出")
                    color: Theme.textDim
                    font.pixelSize: 9
                }
            }
        }
    }
}
