import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtMultimedia
import BadmintonAnalyzer

Item {
    id: root

    required property QtObject importer
    required property QtObject analyzer
    required property QtObject assembly
    property bool muted: true
    property int activeIndex: 0
    property real pendingPosition: -1
    property bool pendingPlay: false
    property bool hasSnapshot: false
    property var exportRowsSnapshot: []
    property string selectedQuality: "720p"
    readonly property bool snapshotOutdated: root.hasSnapshot && root.assembly.exportDirty
    signal returnRequested()

    component PageButton: Button {
        id: pageButton
        property bool accentStyle: false
        implicitHeight: 32
        leftPadding: 10
        rightPadding: 10
        hoverEnabled: true
        contentItem: Text {
            text: pageButton.text
            color: !pageButton.enabled
                   ? Theme.textDim
                   : (pageButton.accentStyle ? Theme.accentText : Theme.textMuted)
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            font.pixelSize: 10
            font.weight: pageButton.accentStyle ? Font.DemiBold : Font.Medium
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            color: !pageButton.enabled
                   ? Theme.panel
                   : (pageButton.accentStyle
                      ? Theme.accent
                      : (pageButton.hovered ? Theme.panelHover : Theme.panelRaised))
            border.color: pageButton.accentStyle ? Theme.accent : Theme.borderStrong
        }
    }

    component QualityButton: Button {
        id: qualityButton
        required property string qualityCode
        property string detailText: ""
        checkable: true
        checked: root.selectedQuality === qualityButton.qualityCode
        implicitHeight: 48
        onClicked: root.selectedQuality = qualityButton.qualityCode
        contentItem: Column {
            anchors.centerIn: parent
            spacing: 1
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qualityButton.text
                color: qualityButton.checked ? Theme.accent : Theme.text
                font.pixelSize: 10
                font.weight: Font.DemiBold
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qualityButton.detailText
                color: Theme.textDim
                font.pixelSize: 8
            }
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            color: qualityButton.checked ? Theme.accentSoft
                                         : (qualityButton.hovered ? Theme.panelHover : Theme.panelRaised)
            border.color: qualityButton.checked ? Theme.accent : Theme.border
        }
    }

    function clearSnapshot() {
        root.hasSnapshot = false
        root.exportRowsSnapshot = []
        exportModel.clear()
        root.activeIndex = 0
        root.pendingPosition = -1
        root.selectedQuality = "720p"
    }

    function qualityLabel(code) {
        if (code === "source")
            return qsTr("原始画质 · %1").arg(root.importer.resolutionText)
        if (code === "1080p")
            return qsTr("1080p 高清")
        return qsTr("720p 快速")
    }

    function formatSeconds(value) {
        const safe = Math.max(0, Number(value) || 0)
        const minutes = Math.floor(safe / 60)
        const seconds = safe - minutes * 60
        return String(minutes).padStart(2, "0") + ":" + seconds.toFixed(2).padStart(5, "0")
    }

    function syncRows() {
        const incoming = root.assembly.exportRows()
        root.exportRowsSnapshot = incoming
        root.hasSnapshot = true
        exportModel.clear()
        for (let i = 0; i < incoming.length; ++i) {
            if (incoming[i].selected)
                exportModel.append(incoming[i])
        }
        if (root.activeIndex >= exportModel.count)
            root.activeIndex = Math.max(0, exportModel.count - 1)
        root.seekCurrent(false)
    }

    function selectedDuration() {
        let total = 0
        for (let i = 0; i < exportModel.count; ++i) {
            const row = exportModel.get(i)
            total += Math.max(0, Number(row.endSeconds) - Number(row.startSeconds))
        }
        return total
    }

    function seekCurrent(autoplay) {
        if (root.activeIndex < 0 || root.activeIndex >= exportModel.count)
            return
        const row = exportModel.get(root.activeIndex)
        root.pendingPosition = Number(row.startSeconds) * 1000
        root.pendingPlay = autoplay === true
        if (exportPlayer.source.toString() !== root.importer.sourceUrl.toString()) {
            exportPlayer.source = root.importer.sourceUrl
        } else if (exportPlayer.mediaStatus === MediaPlayer.LoadedMedia
                   || exportPlayer.mediaStatus === MediaPlayer.BufferedMedia) {
            exportPlayer.position = root.pendingPosition
            root.pendingPosition = -1
            if (root.pendingPlay)
                exportPlayer.play()
        }
    }

    function selectRow(index, autoplay) {
        if (index < 0 || index >= exportModel.count)
            return
        root.activeIndex = index
        root.seekCurrent(autoplay)
    }

    function moveSegment(direction) {
        const next = root.activeIndex + direction
        if (next >= 0 && next < exportModel.count)
            root.selectRow(next, false)
    }

    function openConfirm() {
        if (exportModel.count > 0 && !root.analyzer.exporting)
            exportConfirm.open()
    }

    ListModel { id: exportModel }

    Connections {
        target: root.assembly
        function onExportPushed() { root.syncRows() }
    }

    Connections {
        target: root.importer
        function onChanged() {
            if (!root.importer.hasVideo)
                root.clearSnapshot()
        }
    }

    AudioOutput {
        id: exportAudio
        muted: root.muted
        volume: 0.6
    }

    MediaPlayer {
        id: exportPlayer
        source: root.importer.sourceUrl
        audioOutput: exportAudio
        videoOutput: exportVideo
    }

    Connections {
        target: exportPlayer
        function onMediaStatusChanged() {
            if (root.pendingPosition < 0)
                return
            if (exportPlayer.mediaStatus === MediaPlayer.LoadedMedia
                    || exportPlayer.mediaStatus === MediaPlayer.BufferedMedia) {
                exportPlayer.position = root.pendingPosition
                root.pendingPosition = -1
                if (root.pendingPlay)
                    exportPlayer.play()
                root.pendingPlay = false
            }
        }
        function onPositionChanged(position) {
            if (exportPlayer.playbackState !== MediaPlayer.PlayingState
                    || root.activeIndex < 0 || root.activeIndex >= exportModel.count)
                return
            const row = exportModel.get(root.activeIndex)
            if (position + 60 < Number(row.endSeconds) * 1000)
                return
            if (root.activeIndex + 1 < exportModel.count)
                root.selectRow(root.activeIndex + 1, true)
            else
                exportPlayer.pause()
        }
    }

    FolderDialog {
        id: exportFolder
        title: qsTr("选择最终视频导出目录")
        onAccepted: root.analyzer.exportAssembly(
                        root.exportRowsSnapshot,
                        selectedFolder,
                        root.selectedQuality,
                        fileNameField.text)
    }

    Dialog {
        id: exportConfirm
        anchors.centerIn: parent
        modal: true
        width: 420
        title: qsTr("确认输出最终视频")
        closePolicy: Popup.CloseOnEscape
        padding: 12
        onAccepted: exportFolder.open()

        contentItem: ColumnLayout {
            spacing: 8
            Text {
                text: qsTr("将按当前顺序输出 %1 个回合").arg(exportModel.count)
                color: Theme.text
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
            Text {
                text: qsTr("总时长：%1 · 画质：%2")
                      .arg(root.formatSeconds(root.selectedDuration()))
                      .arg(root.qualityLabel(root.selectedQuality))
                color: Theme.textMuted
                font.pixelSize: 11
            }
            Text {
                text: qsTr("将生成一个新视频，不会修改原始素材")
                color: Theme.textMuted
                font.pixelSize: 10
            }
        }

        background: Rectangle {
            radius: Theme.radiusMedium
            color: Theme.panelRaised
            border.color: Theme.borderStrong
        }

        header: Rectangle {
            implicitHeight: 42
            color: Theme.panelRaised
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 12
                text: exportConfirm.title
                color: Theme.text
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.border
            }
        }

        footer: Rectangle {
            implicitHeight: 50
            color: Theme.panelRaised
            RowLayout {
                anchors.fill: parent
                anchors.margins: 10
                Item { Layout.fillWidth: true }
                PageButton {
                    Layout.preferredWidth: 76
                    text: qsTr("取消")
                    onClicked: exportConfirm.reject()
                }
                PageButton {
                    Layout.preferredWidth: 96
                    accentStyle: true
                    text: qsTr("选择目录")
                    onClicked: exportConfirm.accept()
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 70
            radius: Theme.radiusMedium
            color: Theme.panel
            border.color: root.snapshotOutdated ? Theme.warning : Theme.border

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 12
                anchors.topMargin: 9
                anchors.bottomMargin: 9
                spacing: 12

                ColumnLayout {
                    Layout.preferredWidth: 172
                    spacing: 1
                    Text {
                        text: qsTr("视频导出")
                        color: Theme.text
                        font.pixelSize: 17
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: qsTr("确认快照后输出最终视频")
                        color: Theme.textMuted
                        font.pixelSize: 9
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 44
                    radius: Theme.radiusSmall
                    color: root.snapshotOutdated ? "#362b1b" : Theme.navigation
                    border.color: root.snapshotOutdated ? "#805d2e" : Theme.border

                    Column {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 11
                        anchors.rightMargin: 11
                        spacing: 2
                        Text {
                            width: parent.width
                            text: !root.hasSnapshot
                                  ? qsTr("尚未接收拼接方案")
                                  : (root.snapshotOutdated
                                     ? qsTr("拼接方案已修改，当前仍使用上次快照")
                                     : qsTr("导出快照已锁定"))
                            color: root.snapshotOutdated ? Theme.warning
                                                        : (root.hasSnapshot ? Theme.accent : Theme.textMuted)
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            text: root.hasSnapshot
                                  ? qsTr("返回拼接页重新推送，才会更新这里的内容")
                                  : qsTr("请先在回合拼接页选择并推送回合")
                            color: Theme.textDim
                            font.pixelSize: 8
                            elide: Text.ElideRight
                        }
                    }
                }

                ColumnLayout {
                    Layout.preferredWidth: 112
                    spacing: 1
                    Text {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("%1 个回合").arg(exportModel.count)
                        color: Theme.text
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.alignment: Qt.AlignRight
                        text: qsTr("总时长 %1").arg(root.formatSeconds(root.selectedDuration()))
                        color: Theme.accent
                        font.pixelSize: 9
                    }
                }

                PageButton {
                    Layout.preferredWidth: 104
                    Layout.preferredHeight: 36
                    text: qsTr("返回回合拼接")
                    onClicked: root.returnRequested()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: Theme.radiusLarge
                color: "#070a0f"
                border.color: Theme.border
                clip: true

                VideoOutput {
                    id: exportVideo
                    anchors.fill: parent
                    fillMode: VideoOutput.PreserveAspectFit
                }

                Column {
                    anchors.centerIn: parent
                    spacing: 8
                    visible: exportModel.count === 0
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("没有可导出的回合")
                        color: Theme.text
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("返回回合拼接页选择需要输出的回合")
                        color: Theme.textMuted
                        font.pixelSize: 10
                    }
                }

                Rectangle {
                    anchors.centerIn: parent
                    width: 60
                    height: 60
                    radius: 30
                    visible: exportModel.count > 0
                    color: exportPlayArea.containsMouse ? Theme.accent : "#d9364b5d"
                    border.color: exportPlayArea.containsMouse ? Theme.accent : "#718298"
                    Text {
                        anchors.centerIn: parent
                        text: exportPlayer.playbackState === MediaPlayer.PlayingState ? "Ⅱ" : "▶"
                        color: Theme.text
                        font.pixelSize: 18
                    }
                    MouseArea {
                        id: exportPlayArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: exportPlayer.playbackState === MediaPlayer.PlayingState
                                   ? exportPlayer.pause() : root.seekCurrent(true)
                    }
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 12
                    height: 46
                    radius: 9
                    color: "#e60e141e"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 8
                        PageButton {
                            Layout.preferredWidth: 58
                            Layout.preferredHeight: 30
                            enabled: root.activeIndex > 0
                            text: qsTr("上一段")
                            onClicked: root.moveSegment(-1)
                        }
                        Slider {
                            id: exportSlider
                            Layout.fillWidth: true
                            from: 0
                            to: Math.max(1, exportPlayer.duration)
                            onMoved: exportPlayer.position = value
                            Binding on value {
                                value: exportPlayer.position
                                when: !exportSlider.pressed
                            }
                        }
                        Text {
                            text: root.formatSeconds(exportPlayer.position / 1000)
                            color: Theme.textMuted
                            font.pixelSize: 10
                        }
                        PageButton {
                            Layout.preferredWidth: 62
                            Layout.preferredHeight: 30
                            text: root.muted ? qsTr("开启声音") : qsTr("静音")
                            onClicked: root.muted = !root.muted
                        }
                        PageButton {
                            Layout.preferredWidth: 58
                            Layout.preferredHeight: 30
                            enabled: root.activeIndex + 1 < exportModel.count
                            text: qsTr("下一段")
                            onClicked: root.moveSegment(1)
                        }
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 420
                Layout.fillHeight: true
                radius: Theme.radiusLarge
                color: Theme.navigation
                border.color: Theme.border

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: qsTr("输出顺序")
                            color: Theme.text
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: qsTr("只读快照")
                            color: Theme.textDim
                            font.pixelSize: 9
                        }
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        model: exportModel
                        spacing: 6
                        clip: true
                        ScrollBar.vertical: ScrollBar { }

                        delegate: ItemDelegate {
                            id: exportDelegate
                            required property int index
                            required property var modelData
                            readonly property bool hasRemark:
                                String(exportDelegate.modelData.remark || "").trim().length > 0
                            width: parent ? parent.width : 390
                            height: exportDelegate.hasRemark ? 66 : 54
                            onClicked: root.selectRow(index, false)
                            background: Rectangle {
                                radius: Theme.radiusSmall
                                color: root.activeIndex === index ? Theme.panelHover : Theme.panel
                                border.color: root.activeIndex === index ? Theme.accent : Theme.border
                            }
                            contentItem: RowLayout {
                                spacing: 8

                                Rectangle {
                                    Layout.preferredWidth: 42
                                    Layout.preferredHeight: 38
                                    radius: Theme.radiusSmall
                                    color: root.activeIndex === exportDelegate.index
                                           ? Theme.accent : Theme.panelRaised
                                    Column {
                                        anchors.centerIn: parent
                                        spacing: 0
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: qsTr("成片")
                                            color: root.activeIndex === exportDelegate.index
                                                   ? Theme.accentText : Theme.textDim
                                            font.pixelSize: 7
                                        }
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: String(exportDelegate.index + 1).padStart(2, "0")
                                            color: root.activeIndex === exportDelegate.index
                                                   ? Theme.accentText : Theme.textMuted
                                            font.pixelSize: 11
                                            font.weight: Font.Bold
                                        }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text {
                                        text: qsTr("原回合 %1").arg(exportDelegate.modelData.rally)
                                        color: Theme.text
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        visible: exportDelegate.hasRemark
                                        text: exportDelegate.hasRemark
                                              ? String(exportDelegate.modelData.remark).trim() : ""
                                        color: Theme.accent
                                        font.pixelSize: 9
                                        elide: Text.ElideRight
                                    }
                                }

                                ColumnLayout {
                                    Layout.preferredWidth: 76
                                    spacing: 1
                                    Text {
                                        Layout.alignment: Qt.AlignRight
                                        text: root.formatSeconds(exportDelegate.modelData.endSeconds
                                                                 - exportDelegate.modelData.startSeconds)
                                        color: Theme.text
                                        font.pixelSize: 10
                                        font.weight: Font.Medium
                                    }
                                    Text {
                                        Layout.alignment: Qt.AlignRight
                                        text: qsTr("片段时长")
                                        color: Theme.textDim
                                        font.pixelSize: 8
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 226
                        radius: Theme.radiusMedium
                        color: Theme.panel
                        border.color: Theme.border

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 7
                            Text {
                                text: qsTr("导出设置")
                                color: Theme.text
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                            }
                            Text {
                                text: qsTr("选择输出画质；不会超过源视频分辨率")
                                color: Theme.textDim
                                font.pixelSize: 8
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                QualityButton {
                                    Layout.fillWidth: true
                                    qualityCode: "720p"
                                    text: qsTr("720p 快速")
                                    detailText: qsTr("速度优先")
                                }
                                QualityButton {
                                    Layout.fillWidth: true
                                    visible: root.importer.sourceHeight > 1080
                                    qualityCode: "1080p"
                                    text: qsTr("1080p 高清")
                                    detailText: qsTr("推荐")
                                }
                                QualityButton {
                                    Layout.fillWidth: true
                                    qualityCode: "source"
                                    text: qsTr("原始画质")
                                    detailText: root.importer.resolutionText
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: qsTr("文件名"); color: Theme.textMuted; font.pixelSize: 10 }
                                TextField {
                                    id: fileNameField
                                    Layout.fillWidth: true
                                    text: qsTr("羽毛球回合合集")
                                    maximumLength: 80
                                    color: Theme.text
                                    selectionColor: Theme.accentSoft
                                    selectedTextColor: Theme.text
                                    placeholderTextColor: Theme.textDim
                                    background: Rectangle {
                                        radius: Theme.radiusSmall
                                        color: Theme.navigation
                                        border.color: fileNameField.activeFocus
                                                      ? Theme.accent : Theme.borderStrong
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                ProgressBar {
                                    Layout.fillWidth: true
                                    from: 0
                                    to: 1
                                    value: root.analyzer.exportProgress
                                    background: Rectangle {
                                        implicitHeight: 7
                                        radius: 4
                                        color: Theme.timeline
                                    }
                                    contentItem: Item {
                                        implicitHeight: 7
                                        Rectangle {
                                            width: parent.width * root.analyzer.exportProgress
                                            height: parent.height
                                            radius: 4
                                            color: Theme.accent
                                        }
                                    }
                                }
                                Text {
                                    Layout.preferredWidth: 34
                                    horizontalAlignment: Text.AlignRight
                                    text: qsTr("%1%").arg(Math.round(root.analyzer.exportProgress * 100))
                                    color: root.analyzer.exporting ? Theme.accent : Theme.textDim
                                    font.pixelSize: 9
                                }
                            }

                            PageButton {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 40
                                accentStyle: true
                                enabled: exportModel.count > 0 && !root.analyzer.exporting
                                text: root.analyzer.exporting
                                      ? qsTr("正在导出 · %1%").arg(Math.round(root.analyzer.exportProgress * 100))
                                      : qsTr("导出最终视频")
                                onClicked: root.openConfirm()
                            }
                        }
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
