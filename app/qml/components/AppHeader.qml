import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BadmintonAnalyzer

ToolBar {
    id: root

    required property string projectName
    required property string statusText
    property bool statusActive: true
    signal newProjectRequested()

    height: 66
    padding: 0

    background: Rectangle {
        color: Theme.navigation
        border.color: Theme.border
        border.width: 0

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.border
        }
    }

    contentItem: RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 22
        anchors.rightMargin: 18
        spacing: 14

        Rectangle {
            Layout.preferredWidth: 34
            Layout.preferredHeight: 34
            radius: 10
            color: Theme.accent

            Text {
                anchors.centerIn: parent
                text: "B"
                color: Theme.accentText
                font.pixelSize: 18
                font.weight: Font.Bold
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1

            Text {
                text: qsTr("羽毛球回合分析器")
                color: Theme.text
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }

            Text {
                text: root.projectName
                color: Theme.textMuted
                font.pixelSize: 11
            }
        }

        Rectangle {
            Layout.preferredWidth: statusRow.implicitWidth + 22
            Layout.preferredHeight: 30
            radius: 15
            color: root.statusActive ? Theme.accentSoft : Theme.panelRaised
            border.color: root.statusActive ? "#2e7964" : Theme.border

            RowLayout {
                id: statusRow
                anchors.centerIn: parent
                spacing: 7

                Rectangle {
                    width: 7
                    height: 7
                    radius: 4
                    color: root.statusActive ? Theme.accent : Theme.textMuted
                }

                Text {
                    text: root.statusText
                    color: root.statusActive ? Theme.accent : Theme.textMuted
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }
            }
        }

        Button {
            text: qsTr("新建项目")
            implicitWidth: 92
            implicitHeight: 34
            onClicked: root.newProjectRequested()

            contentItem: Text {
                text: parent.text
                color: Theme.text
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: 12
                font.weight: Font.Medium
            }

            background: Rectangle {
                radius: Theme.radiusSmall
                color: parent.hovered ? Theme.panelHover : Theme.panelRaised
                border.color: Theme.borderStrong
            }
        }
    }
}
