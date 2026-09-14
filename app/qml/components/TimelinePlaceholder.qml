import QtQuick
import QtQuick.Layouts
import BadmintonAnalyzer

Rectangle {
    id: root

    required property int selectedRally
    required property int rallyCount
    property real cursorPosition: 0.34

    radius: Theme.radiusLarge
    color: Theme.panel
    border.color: Theme.border

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 12
        anchors.bottomMargin: 11
        spacing: 7

        RowLayout {
            Layout.fillWidth: true

            Text {
                text: qsTr("时间线")
                color: Theme.text
                font.pixelSize: 11
                font.weight: Font.DemiBold
            }

            Item { Layout.fillWidth: true }

            Text {
                text: "02:03 / 06:04"
                color: Theme.textMuted
                font.pixelSize: 10
            }
        }

        Item {
            id: trackArea
            Layout.fillWidth: true
            Layout.fillHeight: true

            Rectangle {
                id: track
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                height: 30
                radius: 5
                color: Theme.timeline

                Repeater {
                    model: root.rallyCount

                    Rectangle {
                        required property int index
                        x: track.width * (0.04 + index * 0.18)
                        width: track.width * (index === 4 ? 0.12 : 0.08)
                        height: track.height
                        radius: 4
                        color: index === root.selectedRally ? Theme.accent : "#406076"
                        opacity: index === root.selectedRally ? 1 : 0.78
                    }
                }

                Rectangle {
                    x: Math.max(0, Math.min(parent.width - width, parent.width * root.cursorPosition))
                    y: -7
                    width: 2
                    height: parent.height + 14
                    radius: 1
                    color: Theme.text
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onPressed: function(mouse) { root.cursorPosition = mouse.x / width }
                    onPositionChanged: function(mouse) {
                        if (pressed)
                            root.cursorPosition = Math.max(0, Math.min(1, mouse.x / width))
                    }
                }
            }
        }
    }
}
