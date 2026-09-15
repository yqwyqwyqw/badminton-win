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
    property bool previewMuted: true
    property url previewSource: analyzer.rallyCount > selectedRally
                                ? analyzer.clipUrl(selectedRally)
                                : analyzer.proxyUrl

    function formatTime(milliseconds) {
        const totalSeconds = Math.max(0, Math.floor(milliseconds / 1000))
        const minutes = Math.floor(totalSeconds / 60)
        const seconds = totalSeconds % 60
        return String(minutes).padStart(2, "0") + ":" + String(seconds).padStart(2, "0")
    }

    onPreviewSourceChanged: {
        trialPlayer.stop()
        trialPlayer.source = previewSource
    }

    AudioOutput {
        id: trialAudio
        muted: root.previewMuted
        volume: 0.6
    }

    MediaPlayer {
        id: trialPlayer
        source: root.previewSource
        audioOutput: trialAudio
        videoOutput: trialVideo
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 14

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 118
            radius: Theme.radiusLarge
            color: Theme.panel
            border.color: Theme.border

            RowLayout {
                anchors.fill: parent
                anchors.margins: 17
                spacing: 18

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Text {
                        text: qsTr("前几回合切分试验")
                        color: Theme.text
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.analyzer.detailText
                        color: Theme.textMuted
                        elide: Text.ElideRight
                        font.pixelSize: 10
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

                ColumnLayout {
                    spacing: 4
                    Text {
                        text: qsTr("试验回合数")
                        color: Theme.textMuted
                        font.pixelSize: 9
                    }
                    SpinBox {
                        id: rallyLimit
                        from: 1
                        to: 10
                        value: 5
                        editable: true
                        enabled: !root.analyzer.running
                    }
                }

                Button {
                    Layout.preferredWidth: 104
                    Layout.preferredHeight: 40
                    enabled: root.importer.ready
                    text: root.analyzer.running ? qsTr("停止") : qsTr("开始试验")
                    onClicked: root.analyzer.running
                               ? root.analyzer.cancel()
                               : root.analyzer.startTrial(
                                     root.importer.filePath,
                                     root.importer.durationMs,
                                     rallyLimit.value)
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
                id: trialVideo
                anchors.fill: parent
                visible: root.previewSource.toString().length > 0
                fillMode: VideoOutput.PreserveAspectFit
            }

            Column {
                anchors.centerIn: parent
                spacing: 9
                visible: root.previewSource.toString().length === 0

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.analyzer.running ? qsTr("正在等待首个完整回合") : qsTr("尚未生成回合预览")
                    color: Theme.text
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("点击“开始试验”，结果会逐个出现在右侧")
                    color: Theme.textMuted
                    font.pixelSize: 10
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 14
                width: previewLabel.implicitWidth + 22
                height: 30
                radius: 8
                visible: root.previewSource.toString().length > 0
                color: "#cc111925"
                border.color: Theme.borderStrong

                Text {
                    id: previewLabel
                    anchors.centerIn: parent
                    text: root.analyzer.rallyCount > root.selectedRally
                          ? qsTr("回合 %1 预览").arg(root.selectedRally + 1)
                          : qsTr("720p 代理预览")
                    color: Theme.text
                    font.pixelSize: 10
                    font.weight: Font.Medium
                }
            }

            Rectangle {
                anchors.centerIn: parent
                width: 62
                height: 62
                radius: 31
                visible: root.previewSource.toString().length > 0
                         && trialPlayer.playbackState !== MediaPlayer.PlayingState
                color: previewPlay.containsMouse ? Theme.accent : "#d9364b5d"
                border.color: previewPlay.containsMouse ? Theme.accent : "#718298"

                Text {
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: 2
                    text: "▶"
                    color: Theme.text
                    font.pixelSize: 19
                }
                MouseArea {
                    id: previewPlay
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: trialPlayer.play()
                }
            }

            MouseArea {
                anchors.fill: parent
                anchors.bottomMargin: 58
                visible: trialPlayer.playbackState === MediaPlayer.PlayingState
                onClicked: trialPlayer.pause()
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 12
                height: 44
                radius: 9
                visible: root.previewSource.toString().length > 0
                color: "#e60e141e"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    spacing: 9

                    Button {
                        Layout.preferredWidth: 30
                        Layout.preferredHeight: 30
                        text: trialPlayer.playbackState === MediaPlayer.PlayingState ? "Ⅱ" : "▶"
                        onClicked: trialPlayer.playbackState === MediaPlayer.PlayingState
                                   ? trialPlayer.pause() : trialPlayer.play()
                    }
                    Slider {
                        id: trialSlider
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(1, trialPlayer.duration)
                        onMoved: trialPlayer.position = value
                        Binding on value {
                            value: trialPlayer.position
                            when: !trialSlider.pressed
                        }
                    }
                    Text {
                        text: root.formatTime(trialPlayer.position) + " / " + root.formatTime(trialPlayer.duration)
                        color: Theme.textMuted
                        font.pixelSize: 10
                    }
                    Button {
                        Layout.preferredWidth: 62
                        Layout.preferredHeight: 30
                        text: root.previewMuted ? qsTr("开启声音") : qsTr("静音")
                        onClicked: root.previewMuted = !root.previewMuted
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 28

            Text {
                Layout.fillWidth: true
                text: root.analyzer.errorMessage.length > 0
                      ? root.analyzer.errorMessage
                      : root.analyzer.actionMessage
                color: root.analyzer.errorMessage.length > 0 ? Theme.danger : Theme.accent
                elide: Text.ElideMiddle
                font.pixelSize: 10
            }

            Button {
                visible: root.analyzer.outputDirectory.length > 0
                text: qsTr("打开结果目录")
                onClicked: root.analyzer.openOutputFolder()
            }
        }
    }
}
