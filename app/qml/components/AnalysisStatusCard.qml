import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BadmintonAnalyzer

Rectangle {
    id: root

    required property real progress
    required property string processedText
    required property int rallyCount
    property bool paused: false
    signal pauseToggled()

    implicitHeight: 82
    radius: Theme.radiusLarge
    color: Theme.panel
    border.color: Theme.border

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 18
        anchors.rightMargin: 14
        anchors.topMargin: 12
        anchors.bottomMargin: 12
        spacing: 16

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 7

            RowLayout {
                Layout.fillWidth: true

                Text {
                    text: root.paused ? qsTr("分析任务已暂停") : qsTr("正在生成可预览回合")
                    color: Theme.text
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }

                Item { Layout.fillWidth: true }

                Text {
                    text: root.processedText
                    color: Theme.textMuted
                    font.pixelSize: 11
                }
            }

            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: 1
                value: root.progress

                background: Rectangle {
                    implicitHeight: 7
                    radius: 4
                    color: Theme.timeline
                }

                contentItem: Item {
                    implicitHeight: 7

                    Rectangle {
                        width: parent.width * root.progress
                        height: parent.height
                        radius: 4
                        color: root.paused ? Theme.warning : Theme.accent
                    }
                }
            }
        }

        Rectangle {
            Layout.preferredWidth: 90
            Layout.preferredHeight: 48
            radius: Theme.radiusSmall
            color: Theme.panelRaised

            Column {
                anchors.centerIn: parent
                spacing: 1

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.rallyCount
                    color: Theme.text
                    font.pixelSize: 17
                    font.weight: Font.Bold
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("已发现回合")
                    color: Theme.textMuted
                    font.pixelSize: 9
                }
            }
        }

        Button {
            Layout.preferredWidth: 76
            Layout.preferredHeight: 36
            text: root.paused ? qsTr("继续") : qsTr("暂停")
            onClicked: root.pauseToggled()

            contentItem: Text {
                text: parent.text
                color: Theme.text
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }

            background: Rectangle {
                radius: Theme.radiusSmall
                color: parent.hovered ? Theme.panelHover : Theme.panelRaised
                border.color: Theme.borderStrong
            }
        }
    }
}
