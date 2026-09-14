import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BadmintonAnalyzer

Rectangle {
    id: root

    required property int rallyNumber
    required property string timeRange
    property bool playing: false

    radius: Theme.radiusLarge
    color: "#070a0f"
    border.color: Theme.border
    clip: true

    Rectangle {
        anchors.fill: parent
        anchors.margins: 1
        radius: Theme.radiusLarge
        color: "#090d13"

        Canvas {
            anchors.fill: parent
            opacity: 0.26
            onPaint: {
                const context = getContext("2d")
                context.reset()
                context.strokeStyle = "#355269"
                context.lineWidth = 1
                const step = 38
                for (let x = 0; x < width; x += step) {
                    context.beginPath()
                    context.moveTo(x, 0)
                    context.lineTo(x, height)
                    context.stroke()
                }
                for (let y = 0; y < height; y += step) {
                    context.beginPath()
                    context.moveTo(0, y)
                    context.lineTo(width, y)
                    context.stroke()
                }
            }
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
        }

        Column {
            anchors.centerIn: parent
            spacing: 13

            Rectangle {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 60
                height: 60
                radius: 30
                color: playArea.containsMouse ? Theme.accent : "#d9364b5d"
                border.color: playArea.containsMouse ? Theme.accent : "#718298"

                Text {
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: root.playing ? 0 : 2
                    text: root.playing ? "Ⅱ" : "▶"
                    color: root.playing ? Theme.accentText : Theme.text
                    font.pixelSize: root.playing ? 21 : 18
                    font.weight: Font.Bold
                }

                MouseArea {
                    id: playArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.playing = !root.playing
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.playing ? qsTr("正在播放占位画面") : qsTr("视频预览区域")
                color: Theme.text
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("后续接入原视频时间索引播放")
                color: Theme.textMuted
                font.pixelSize: 11
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 15
            width: previewLabel.implicitWidth + 22
            height: 30
            radius: 8
            color: "#c0141c27"
            border.color: Theme.borderStrong

            Text {
                id: previewLabel
                anchors.centerIn: parent
                text: qsTr("回合 %1 · %2").arg(root.rallyNumber).arg(root.timeRange)
                color: Theme.text
                font.pixelSize: 11
                font.weight: Font.Medium
            }
        }

        Text {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 15
            text: "16:9  ·  720p 代理"
            color: Theme.textDim
            font.pixelSize: 10
        }
    }
}
