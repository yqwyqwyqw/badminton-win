import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtMultimedia
import BadmintonAnalyzer

Item {
    id: root

    required property QtObject importer
    property bool previewMuted: true
    signal continueRequested()

    function formatPlaybackTime(milliseconds) {
        const totalSeconds = Math.max(0, Math.floor(milliseconds / 1000))
        const hours = Math.floor(totalSeconds / 3600)
        const minutes = Math.floor((totalSeconds % 3600) / 60)
        const seconds = totalSeconds % 60
        const mm = String(minutes).padStart(2, "0")
        const ss = String(seconds).padStart(2, "0")
        return hours > 0 ? String(hours).padStart(2, "0") + ":" + mm + ":" + ss : mm + ":" + ss
    }

    AudioOutput {
        id: previewAudio
        muted: root.previewMuted
        volume: 0.6
    }

    MediaPlayer {
        id: previewPlayer
        source: root.importer.sourceUrl
        audioOutput: previewAudio
        videoOutput: previewOutput
    }

    FileDialog {
        id: videoDialog
        title: qsTr("选择羽毛球视频")
        fileMode: FileDialog.OpenFile
        nameFilters: [
            qsTr("视频文件 (*.mp4 *.mov *.mkv *.m4v *.avi *.webm)"),
            qsTr("所有文件 (*)")
        ]
        onAccepted: root.importer.importVideo(selectedFile)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 16

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4

            Text {
                text: qsTr("导入视频")
                color: Theme.text
                font.pixelSize: 22
                font.weight: Font.Bold
            }

            Text {
                text: qsTr("选择一段本地羽毛球素材。文件只在本机读取，不会上传。")
                color: Theme.textMuted
                font.pixelSize: 12
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radiusLarge
            color: dropArea.containsDrag ? Theme.accentSoft : Theme.panel
            border.color: dropArea.containsDrag ? Theme.accent : Theme.borderStrong
            border.width: dropArea.containsDrag ? 2 : 1

            VideoOutput {
                id: previewOutput
                anchors.fill: parent
                anchors.margins: 1
                visible: root.importer.ready
                fillMode: VideoOutput.PreserveAspectFit
            }

            DropArea {
                id: dropArea
                anchors.fill: parent
                onDropped: function(drop) {
                    if (drop.hasUrls && drop.urls.length > 0) {
                        root.importer.importVideo(drop.urls[0])
                        drop.acceptProposedAction()
                    }
                }
            }

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width - 80, 660)
                spacing: 16
                visible: !root.importer.ready

                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 72
                    Layout.preferredHeight: 72
                    radius: 22
                    color: Theme.panelRaised
                    border.color: root.importer.ready ? Theme.accent : Theme.borderStrong

                    Text {
                        anchors.centerIn: parent
                        text: root.importer.ready ? "✓" : "＋"
                        color: root.importer.ready ? Theme.accent : Theme.textMuted
                        font.pixelSize: 31
                        font.weight: Font.Medium
                    }
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: root.importer.ready
                          ? qsTr("素材读取完成")
                          : (root.importer.loading ? qsTr("正在读取素材信息…") : qsTr("拖入视频，或从本机选择"))
                    color: Theme.text
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("支持 MP4、MOV、MKV、M4V、AVI 和 WebM")
                    color: Theme.textMuted
                    font.pixelSize: 11
                }

                Button {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 150
                    Layout.preferredHeight: 42
                    text: root.importer.hasVideo ? qsTr("更换视频") : qsTr("选择视频")
                    onClicked: videoDialog.open()

                    contentItem: Text {
                        text: parent.text
                        color: Theme.accentText
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }

                    background: Rectangle {
                        radius: Theme.radiusSmall
                        color: parent.hovered ? "#68e5ba" : Theme.accent
                    }
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.maximumWidth: 620
                    visible: root.importer.errorMessage.length > 0
                    text: root.importer.errorMessage
                    color: Theme.danger
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: 11
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 14
                width: sourceLabel.implicitWidth + 22
                height: 30
                radius: 8
                visible: root.importer.ready
                color: "#cc111925"
                border.color: Theme.borderStrong

                Text {
                    id: sourceLabel
                    anchors.centerIn: parent
                    text: qsTr("原片预览 · %1").arg(root.importer.fileName)
                    color: Theme.text
                    font.pixelSize: 10
                    font.weight: Font.Medium
                }
            }

            Rectangle {
                anchors.centerIn: parent
                width: 66
                height: 66
                radius: 33
                visible: root.importer.ready && previewPlayer.playbackState !== MediaPlayer.PlayingState
                color: playArea.containsMouse ? Theme.accent : "#d9364b5d"
                border.color: playArea.containsMouse ? Theme.accent : "#718298"

                Text {
                    anchors.centerIn: parent
                    anchors.horizontalCenterOffset: 2
                    text: "▶"
                    color: playArea.containsMouse ? Theme.accentText : Theme.text
                    font.pixelSize: 20
                }

                MouseArea {
                    id: playArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: previewPlayer.play()
                }
            }

            MouseArea {
                anchors.fill: parent
                anchors.bottomMargin: 64
                visible: root.importer.ready && previewPlayer.playbackState === MediaPlayer.PlayingState
                cursorShape: Qt.PointingHandCursor
                onClicked: previewPlayer.pause()
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 12
                height: 48
                radius: 10
                visible: root.importer.ready
                color: "#e60e141e"
                border.color: Theme.border

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 10

                    Button {
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        text: previewPlayer.playbackState === MediaPlayer.PlayingState ? "Ⅱ" : "▶"
                        onClicked: previewPlayer.playbackState === MediaPlayer.PlayingState
                                   ? previewPlayer.pause()
                                   : previewPlayer.play()
                        contentItem: Text {
                            text: parent.text
                            color: Theme.text
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: 13
                        }
                        background: Rectangle {
                            radius: 7
                            color: parent.hovered ? Theme.panelHover : Theme.panelRaised
                        }
                    }

                    Slider {
                        id: playbackSlider
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(1, previewPlayer.duration)
                        onMoved: previewPlayer.position = value

                        Binding on value {
                            value: previewPlayer.position
                            when: !playbackSlider.pressed
                        }
                    }

                    Text {
                        text: root.formatPlaybackTime(previewPlayer.position)
                              + " / " + root.formatPlaybackTime(previewPlayer.duration)
                        color: Theme.textMuted
                        font.pixelSize: 10
                    }

                    Button {
                        Layout.preferredWidth: 46
                        Layout.preferredHeight: 32
                        text: root.previewMuted ? qsTr("开启声音") : qsTr("静音")
                        onClicked: root.previewMuted = !root.previewMuted
                        contentItem: Text {
                            text: parent.text
                            color: root.previewMuted ? Theme.textMuted : Theme.accent
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: 10
                        }
                        background: Rectangle {
                            radius: 7
                            color: parent.hovered ? Theme.panelHover : Theme.panelRaised
                        }
                    }

                    Button {
                        Layout.preferredWidth: 70
                        Layout.preferredHeight: 32
                        text: qsTr("更换视频")
                        onClicked: videoDialog.open()
                        contentItem: Text {
                            text: parent.text
                            color: Theme.text
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            font.pixelSize: 10
                        }
                        background: Rectangle {
                            radius: 7
                            color: parent.hovered ? Theme.panelHover : Theme.panelRaised
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: root.importer.hasVideo ? 184 : 0
            visible: root.importer.hasVideo
            radius: Theme.radiusLarge
            color: Theme.panel
            border.color: root.importer.ready ? Theme.accent : Theme.border

            RowLayout {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 20

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 7

                    Text {
                        Layout.fillWidth: true
                        text: root.importer.fileName
                        color: Theme.text
                        elide: Text.ElideMiddle
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }

                    Text {
                        Layout.fillWidth: true
                        text: root.importer.filePath
                        color: Theme.textMuted
                        elide: Text.ElideMiddle
                        font.pixelSize: 10
                    }

                    GridLayout {
                        columns: 3
                        columnSpacing: 22
                        rowSpacing: 4

                        Repeater {
                            model: [
                                { label: qsTr("大小"), value: root.importer.fileSizeText },
                                { label: qsTr("时长"), value: root.importer.durationText },
                                { label: qsTr("画面"), value: root.importer.resolutionText },
                                { label: qsTr("帧率"), value: root.importer.frameRateText },
                                { label: qsTr("编码"), value: root.importer.codecText },
                                {
                                    label: qsTr("原始日期 · %1").arg(root.importer.originalDateSourceText),
                                    value: root.importer.originalDateText
                                }
                            ]

                            ColumnLayout {
                                required property var modelData
                                spacing: 2

                                Text { text: modelData.label; color: Theme.textDim; font.pixelSize: 9 }
                                Text { text: modelData.value; color: Theme.text; font.pixelSize: 12; font.weight: Font.Medium }
                            }
                        }
                    }
                }

                Button {
                    Layout.preferredWidth: 142
                    Layout.preferredHeight: 42
                    enabled: root.importer.ready
                    text: qsTr("进入切分试验")
                    onClicked: root.continueRequested()

                    contentItem: Text {
                        text: parent.text
                        color: parent.enabled ? Theme.accentText : Theme.textDim
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }

                    background: Rectangle {
                        radius: Theme.radiusSmall
                        color: parent.enabled ? Theme.accent : Theme.panelRaised
                    }
                }
            }
        }
    }
}
