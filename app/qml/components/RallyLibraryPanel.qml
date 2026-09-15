import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import BadmintonAnalyzer

Item {
    id: root

    required property QtObject importer
    required property QtObject analyzer
    required property int selectedRally
    property bool muted: true
    property url selectedClip: analyzer.rallyCount > selectedRally
                               ? analyzer.clipUrl(selectedRally) : ""
    signal previousRequested()
    signal nextRequested()

    function formatTime(milliseconds) {
        const total = Math.max(0, Math.floor(milliseconds / 1000))
        return String(Math.floor(total / 60)).padStart(2, "0")
               + ":" + String(total % 60).padStart(2, "0")
    }

    onSelectedClipChanged: {
        rallyPlayer.stop()
        rallyPlayer.source = selectedClip
    }

    AudioOutput {
        id: rallyAudio
        muted: root.muted
        volume: 0.6
    }

    MediaPlayer {
        id: rallyPlayer
        source: root.selectedClip
        audioOutput: rallyAudio
        videoOutput: rallyVideo
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 110
            radius: Theme.radiusLarge
            color: Theme.panel
            border.color: Theme.border

            RowLayout {
                anchors.fill: parent
                anchors.margins: 17
                spacing: 18

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 5
                    Text {
                        text: qsTr("正式回合分析")
                        color: Theme.text
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: root.analyzer.detailText
                        color: Theme.textMuted
                        font.pixelSize: 10
                        elide: Text.ElideRight
                    }
                    ProgressBar {
                        Layout.fillWidth: true
                        from: 0
                        to: 1
                        value: root.analyzer.progress
                        background: Rectangle {
                            implicitHeight: 7
                            radius: 4
                            color: Theme.timeline
                        }
                        contentItem: Item {
                            implicitHeight: 7
                            Rectangle {
                                width: parent.width * root.analyzer.progress
                                height: parent.height
                                radius: 4
                                color: root.analyzer.state === "error" ? Theme.danger : Theme.accent
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: root.analyzer.stageText
                            color: root.analyzer.state === "error" ? Theme.danger : Theme.text
                            font.pixelSize: 10
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: root.analyzer.etaText
                            color: Theme.textMuted
                            font.pixelSize: 10
                        }
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 92
                    Layout.preferredHeight: 58
                    radius: Theme.radiusSmall
                    color: Theme.panelRaised
                    Column {
                        anchors.centerIn: parent
                        spacing: 2
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: root.analyzer.rallyCount
                            color: Theme.text
                            font.pixelSize: 18
                            font.weight: Font.Bold
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: qsTr("已识别回合")
                            color: Theme.textMuted
                            font.pixelSize: 9
                        }
                    }
                }

                Button {
                    Layout.preferredWidth: 118
                    Layout.preferredHeight: 40
                    enabled: root.importer.ready
                    text: root.analyzer.running
                          ? qsTr("停止分析")
                          : (root.analyzer.progress > 0 && root.analyzer.progress < 1
                             ? qsTr("继续分析") : qsTr("分析完整视频"))
                    onClicked: root.analyzer.running
                               ? root.analyzer.cancel()
                               : root.analyzer.startFullAnalysis(
                                     root.importer.filePath,
                                     root.importer.durationMs,
                                     root.importer.sourceWidth,
                                     root.importer.sourceHeight)
                    contentItem: Text {
                        text: parent.text
                        color: parent.enabled ? Theme.accentText : Theme.textDim
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                    }
                    background: Rectangle {
                        radius: Theme.radiusSmall
                        color: parent.enabled
                               ? (root.analyzer.running ? Theme.warning : Theme.accent)
                               : Theme.panelRaised
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radiusLarge
            color: "#070a0f"
            border.color: Theme.border
            clip: true

            VideoOutput {
                id: rallyVideo
                anchors.fill: parent
                visible: root.selectedClip.toString().length > 0
                fillMode: VideoOutput.PreserveAspectFit
            }

            Column {
                anchors.centerIn: parent
                spacing: 8
                visible: root.selectedClip.toString().length === 0
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("暂无可播放回合")
                    color: Theme.text
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("开始完整分析后，回合会边处理边出现")
                    color: Theme.textMuted
                    font.pixelSize: 10
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 14
                width: titleText.implicitWidth + 22
                height: 30
                radius: 8
                visible: root.selectedClip.toString().length > 0
                color: "#cc111925"
                border.color: Theme.borderStrong
                Text {
                    id: titleText
                    anchors.centerIn: parent
                    text: qsTr("回合 %1 / %2").arg(root.selectedRally + 1).arg(root.analyzer.rallyCount)
                    color: Theme.text
                    font.pixelSize: 10
                }
            }

            Rectangle {
                anchors.centerIn: parent
                width: 62
                height: 62
                radius: 31
                visible: root.selectedClip.toString().length > 0
                         && rallyPlayer.playbackState !== MediaPlayer.PlayingState
                color: playArea.containsMouse ? Theme.accent : "#d9364b5d"
                border.color: playArea.containsMouse ? Theme.accent : "#718298"
                Text {
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: 2
                    text: "▶"
                    color: Theme.text
                    font.pixelSize: 19
                }
                MouseArea {
                    id: playArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: rallyPlayer.play()
                }
            }

            MouseArea {
                anchors.fill: parent
                anchors.bottomMargin: 60
                visible: rallyPlayer.playbackState === MediaPlayer.PlayingState
                onClicked: rallyPlayer.pause()
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 12
                height: 46
                radius: 9
                visible: root.selectedClip.toString().length > 0
                color: "#e60e141e"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 9

                    Button {
                        Layout.preferredWidth: 60
                        Layout.preferredHeight: 30
                        enabled: root.selectedRally > 0
                        text: qsTr("上一回合")
                        onClicked: root.previousRequested()
                    }
                    Button {
                        Layout.preferredWidth: 30
                        Layout.preferredHeight: 30
                        text: rallyPlayer.playbackState === MediaPlayer.PlayingState ? "Ⅱ" : "▶"
                        onClicked: rallyPlayer.playbackState === MediaPlayer.PlayingState
                                   ? rallyPlayer.pause() : rallyPlayer.play()
                    }
                    Slider {
                        id: rallySlider
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(1, rallyPlayer.duration)
                        onMoved: rallyPlayer.position = value
                        Binding on value {
                            value: rallyPlayer.position
                            when: !rallySlider.pressed
                        }
                    }
                    Text {
                        text: root.formatTime(rallyPlayer.position) + " / " + root.formatTime(rallyPlayer.duration)
                        color: Theme.textMuted
                        font.pixelSize: 10
                    }
                    Button {
                        Layout.preferredWidth: 62
                        Layout.preferredHeight: 30
                        text: root.muted ? qsTr("开启声音") : qsTr("静音")
                        onClicked: root.muted = !root.muted
                    }
                    Button {
                        Layout.preferredWidth: 60
                        Layout.preferredHeight: 30
                        enabled: root.selectedRally + 1 < root.analyzer.rallyCount
                        text: qsTr("下一回合")
                        onClicked: root.nextRequested()
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            text: root.analyzer.errorMessage.length > 0
                  ? root.analyzer.errorMessage : root.analyzer.actionMessage
            color: root.analyzer.errorMessage.length > 0 ? Theme.danger : Theme.accent
            elide: Text.ElideMiddle
            font.pixelSize: 10
        }
    }
}
