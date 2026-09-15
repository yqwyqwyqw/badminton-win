import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import BadmintonAnalyzer

Item {
    id: root

    required property QtObject importer
    required property QtObject analyzer
    property int activeIndex: -1
    property int activeRallyNumber: 0
    property bool muted: true
    property real pendingPosition: -1
    property bool pendingPlay: false
    property string lastIncomingSignature: ""
    property bool showSelectedOnly: false
    property int revision: 0
    property int lastPushedRevision: -1
    property bool hasPushed: false
    readonly property bool exportDirty: root.hasPushed && root.revision !== root.lastPushedRevision
    signal assemblyChanged()
    signal exportPushed()

    onAssemblyChanged: root.revision += 1

    component CompactButton: Button {
        id: compactControl
        property bool accentStyle: false
        implicitHeight: 28
        leftPadding: 8
        rightPadding: 8
        contentItem: Text {
            text: compactControl.text
            color: !compactControl.enabled
                   ? Theme.textDim
                   : (compactControl.accentStyle ? Theme.accentText : Theme.textMuted)
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            font.pixelSize: 9
            font.weight: compactControl.accentStyle ? Font.DemiBold : Font.Normal
        }
        background: Rectangle {
            radius: Theme.radiusSmall
            color: !compactControl.enabled
                   ? Theme.panel
                   : (compactControl.accentStyle
                      ? Theme.accent
                      : (compactControl.checked || compactControl.hovered
                         ? Theme.panelHover : Theme.panelRaised))
            border.color: compactControl.accentStyle
                          ? Theme.accent
                          : (compactControl.checked ? Theme.accent : Theme.border)
        }
    }

    function formatSeconds(value) {
        const safe = Math.max(0, Number(value) || 0)
        const minutes = Math.floor(safe / 60)
        const seconds = safe - minutes * 60
        return String(minutes).padStart(2, "0") + ":" + seconds.toFixed(2).padStart(5, "0")
    }

    function rowDuration(row) {
        return Math.max(0, Number(row.endSeconds) - Number(row.startSeconds))
    }

    function selectedCount() {
        let count = 0
        for (let i = 0; i < assemblyModel.count; ++i) {
            if (assemblyModel.get(i).selected)
                ++count
        }
        return count
    }

    function selectedDuration() {
        let duration = 0
        for (let i = 0; i < assemblyModel.count; ++i) {
            const row = assemblyModel.get(i)
            if (row.selected)
                duration += rowDuration(row)
        }
        return duration
    }

    function setAllSelected(selected) {
        for (let i = 0; i < assemblyModel.count; ++i)
            assemblyModel.setProperty(i, "selected", selected)
        root.assemblyChanged()
    }

    function setRowSelected(index, selected) {
        if (index < 0 || index >= assemblyModel.count)
            return
        assemblyModel.setProperty(index, "selected", selected)
        root.assemblyChanged()
    }

    function exportRows() {
        const rows = []
        for (let i = 0; i < assemblyModel.count; ++i) {
            const row = assemblyModel.get(i)
            rows.push({
                rally: row.rally,
                startSeconds: row.startSeconds,
                endSeconds: row.endSeconds,
                hitCount: row.hitCount,
                remark: row.remark || "",
                selected: row.selected
            })
        }
        return rows
    }

    function pushToExport() {
        if (root.selectedCount() <= 0)
            return
        root.hasPushed = true
        root.lastPushedRevision = root.revision
        root.exportPushed()
    }

    function restoreOriginalOrder() {
        for (let target = 0; target < assemblyModel.count; ++target) {
            let lowest = target
            for (let candidate = target + 1; candidate < assemblyModel.count; ++candidate) {
                if (Number(assemblyModel.get(candidate).rally)
                        < Number(assemblyModel.get(lowest).rally))
                    lowest = candidate
            }
            if (lowest !== target)
                assemblyModel.move(lowest, target, 1)
        }
        if (root.activeRallyNumber > 0) {
            for (let i = 0; i < assemblyModel.count; ++i) {
                if (assemblyModel.get(i).rally === root.activeRallyNumber) {
                    root.activeIndex = i
                    break
                }
            }
        }
        root.assemblyChanged()
    }

    function restoreActiveBoundary() {
        if (root.activeIndex < 0 || root.activeIndex >= assemblyModel.count)
            return
        const row = assemblyModel.get(root.activeIndex)
        assemblyModel.setProperty(root.activeIndex, "startSeconds", row.originalStartSeconds)
        assemblyModel.setProperty(root.activeIndex, "endSeconds", row.originalEndSeconds)
        root.updateTimeFields()
        root.assemblyChanged()
    }

    function syncRallies() {
        const incoming = root.analyzer.rallies || []
        const signature = incoming.map(function(item) {
            return String(item.rally) + ":" + String(item.startSeconds) + ":" + String(item.endSeconds)
        }).join("|")
        if (signature === root.lastIncomingSignature)
            return
        root.lastIncomingSignature = signature

        const oldByRally = ({})
        const oldOrder = []
        for (let i = 0; i < assemblyModel.count; ++i) {
            const old = assemblyModel.get(i)
            const key = String(old.rally)
            oldOrder.push(key)
            oldByRally[key] = {
                rally: old.rally,
                startSeconds: old.startSeconds,
                endSeconds: old.endSeconds,
                originalStartSeconds: old.originalStartSeconds,
                originalEndSeconds: old.originalEndSeconds,
                hitCount: old.hitCount,
                timeText: old.timeText,
                remark: old.remark || "",
                selected: old.selected
            }
        }

        const incomingByRally = ({})
        for (let i = 0; i < incoming.length; ++i)
            incomingByRally[String(incoming[i].rally)] = incoming[i]

        const rows = []
        for (let i = 0; i < oldOrder.length; ++i) {
            const key = oldOrder[i]
            if (incomingByRally[key])
                rows.push(oldByRally[key])
        }
        for (let i = 0; i < incoming.length; ++i) {
            const item = incoming[i]
            const key = String(item.rally)
            if (oldByRally[key])
                continue
            rows.push({
                rally: item.rally,
                startSeconds: item.startSeconds,
                endSeconds: item.endSeconds,
                originalStartSeconds: item.startSeconds,
                originalEndSeconds: item.endSeconds,
                hitCount: item.hitCount,
                timeText: item.timeText,
                remark: "",
                selected: true
            })
        }

        assemblyModel.clear()
        for (let i = 0; i < rows.length; ++i)
            assemblyModel.append(rows[i])
        root.assemblyChanged()

        if (root.activeRallyNumber > 0) {
            for (let i = 0; i < assemblyModel.count; ++i) {
                if (assemblyModel.get(i).rally === root.activeRallyNumber) {
                    root.activeIndex = i
                    return
                }
            }
        }
        root.activeIndex = assemblyModel.count > 0 ? 0 : -1
        root.activeRallyNumber = root.activeIndex >= 0 ? assemblyModel.get(root.activeIndex).rally : 0
        root.updateTimeFields()
    }

    function updateTimeFields() {
        if (root.activeIndex < 0 || root.activeIndex >= assemblyModel.count) {
            startField.text = ""
            endField.text = ""
            return
        }
        const row = assemblyModel.get(root.activeIndex)
        startField.text = Number(row.startSeconds).toFixed(2)
        endField.text = Number(row.endSeconds).toFixed(2)
    }

    function openRemark(index) {
        if (index < 0 || index >= assemblyModel.count)
            return
        root.activeIndex = index
        root.activeRallyNumber = assemblyModel.get(index).rally
        root.updateTimeFields()
        remarkInput.text = assemblyModel.get(index).remark || ""
        remarkDialog.open()
        remarkInput.forceActiveFocus()
    }

    function selectRow(index, autoplay) {
        if (index < 0 || index >= assemblyModel.count)
            return
        root.activeIndex = index
        root.activeRallyNumber = assemblyModel.get(index).rally
        root.pendingPosition = Number(assemblyModel.get(index).startSeconds) * 1000
        root.pendingPlay = autoplay === true
        if (assemblyPlayer.source.toString() !== root.importer.sourceUrl.toString())
            assemblyPlayer.source = root.importer.sourceUrl
        else if (assemblyPlayer.mediaStatus === MediaPlayer.LoadedMedia
                 || assemblyPlayer.mediaStatus === MediaPlayer.BufferedMedia) {
            assemblyPlayer.position = root.pendingPosition
            root.pendingPosition = -1
            if (root.pendingPlay)
                assemblyPlayer.play()
        }
        root.updateTimeFields()
    }

    function findSelected(start, direction) {
        for (let i = start + direction; i >= 0 && i < assemblyModel.count; i += direction) {
            if (assemblyModel.get(i).selected)
                return i
        }
        return -1
    }

    function moveRow(from, to) {
        if (from < 0 || to < 0 || from >= assemblyModel.count || to >= assemblyModel.count)
            return
        assemblyModel.move(from, to, 1)
        root.activeIndex = to
        root.activeRallyNumber = assemblyModel.get(to).rally
        root.assemblyChanged()
    }

    function nudgeBoundary(kind, amount) {
        if (root.activeIndex < 0 || root.activeIndex >= assemblyModel.count)
            return
        const row = assemblyModel.get(root.activeIndex)
        const duration = Math.max(0, Number(root.importer.durationMs) / 1000)
        if (kind === "start") {
            const value = Math.max(0, Math.min(Number(row.endSeconds) - 0.1, Number(row.startSeconds) + amount))
            assemblyModel.setProperty(root.activeIndex, "startSeconds", value)
        } else {
            const value = Math.min(duration, Math.max(Number(row.startSeconds) + 0.1, Number(row.endSeconds) + amount))
            assemblyModel.setProperty(root.activeIndex, "endSeconds", value)
        }
        root.updateTimeFields()
        root.assemblyChanged()
    }

    function applyBoundary(kind, text) {
        const value = Number(text)
        if (!isFinite(value) || root.activeIndex < 0 || root.activeIndex >= assemblyModel.count) {
            root.updateTimeFields()
            return
        }
        const row = assemblyModel.get(root.activeIndex)
        const duration = Math.max(0, Number(root.importer.durationMs) / 1000)
        if (kind === "start") {
            assemblyModel.setProperty(root.activeIndex, "startSeconds",
                                      Math.max(0, Math.min(Number(row.endSeconds) - 0.1, value)))
        } else {
            assemblyModel.setProperty(root.activeIndex, "endSeconds",
                                      Math.min(duration, Math.max(Number(row.startSeconds) + 0.1, value)))
        }
        root.updateTimeFields()
        root.assemblyChanged()
    }

    ListModel {
        id: assemblyModel
    }

    Component.onCompleted: root.syncRallies()

    Connections {
        target: root.analyzer
        function onChanged() { root.syncRallies() }
    }

    AudioOutput {
        id: assemblyAudio
        muted: root.muted
        volume: 0.6
    }

    MediaPlayer {
        id: assemblyPlayer
        source: root.importer.sourceUrl
        audioOutput: assemblyAudio
        videoOutput: assemblyVideo
    }

    Dialog {
        id: remarkDialog
        anchors.centerIn: parent
        modal: true
        width: 380
        title: root.activeRallyNumber > 0
               ? qsTr("回合 %1 备注").arg(root.activeRallyNumber)
               : qsTr("回合备注")
        closePolicy: Popup.CloseOnEscape
        padding: 12
        onAccepted: {
            if (root.activeIndex >= 0 && root.activeIndex < assemblyModel.count)
                assemblyModel.setProperty(root.activeIndex, "remark", remarkInput.text.trim())
            root.assemblyChanged()
        }

        contentItem: ColumnLayout {
            spacing: 8
            Text {
                text: qsTr("记录该回合的特点，方便后续挑选精彩片段")
                color: Theme.textMuted
                font.pixelSize: 10
            }
            TextField {
                id: remarkInput
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                maximumLength: 80
                placeholderText: qsTr("例如：多拍、网前对抗、精彩救球")
                color: Theme.text
                selectionColor: Theme.accentSoft
                selectedTextColor: Theme.text
                placeholderTextColor: Theme.textDim
                background: Rectangle {
                    radius: Theme.radiusSmall
                    color: Theme.navigation
                    border.color: remarkInput.activeFocus ? Theme.accent : Theme.borderStrong
                }
            }
            Text {
                Layout.alignment: Qt.AlignRight
                text: qsTr("不超过 80 字")
                color: Theme.textDim
                font.pixelSize: 9
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
                text: remarkDialog.title
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
                CompactButton {
                    Layout.preferredWidth: 76
                    text: qsTr("取消")
                    onClicked: remarkDialog.reject()
                }
                CompactButton {
                    Layout.preferredWidth: 76
                    accentStyle: true
                    text: qsTr("保存备注")
                    onClicked: remarkDialog.accept()
                }
            }
        }
    }

    Dialog {
        id: restoreOrderDialog
        anchors.centerIn: parent
        modal: true
        width: 390
        title: qsTr("恢复原始回合顺序")
        closePolicy: Popup.CloseOnEscape
        padding: 12
        onAccepted: root.restoreOriginalOrder()

        contentItem: Text {
            text: qsTr("将按自动识别时的回合编号重新排序，隐藏状态、时间微调和备注不会改变。")
            color: Theme.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: 11
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
                text: restoreOrderDialog.title
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
                CompactButton {
                    Layout.preferredWidth: 76
                    text: qsTr("取消")
                    onClicked: restoreOrderDialog.reject()
                }
                CompactButton {
                    Layout.preferredWidth: 96
                    accentStyle: true
                    text: qsTr("确认恢复")
                    onClicked: restoreOrderDialog.accept()
                }
            }
        }
    }

    Connections {
        target: assemblyPlayer
        function onMediaStatusChanged() {
            if (root.pendingPosition < 0)
                return
            if (assemblyPlayer.mediaStatus === MediaPlayer.LoadedMedia
                    || assemblyPlayer.mediaStatus === MediaPlayer.BufferedMedia) {
                assemblyPlayer.position = root.pendingPosition
                root.pendingPosition = -1
                if (root.pendingPlay)
                    assemblyPlayer.play()
                root.pendingPlay = false
            }
        }
        function onPositionChanged(position) {
            if (assemblyPlayer.playbackState !== MediaPlayer.PlayingState
                    || root.activeIndex < 0 || root.activeIndex >= assemblyModel.count)
                return
            const row = assemblyModel.get(root.activeIndex)
            if (position + 60 < Number(row.endSeconds) * 1000)
                return
            const next = root.findSelected(root.activeIndex, 1)
            if (next >= 0)
                root.selectRow(next, true)
            else
                assemblyPlayer.pause()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 42
            spacing: 12

            ColumnLayout {
                spacing: 1
                Text {
                    text: qsTr("回合拼接")
                    color: Theme.text
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
                Text {
                    text: qsTr("选择、排序并微调最终成片中的回合")
                    color: Theme.textMuted
                    font.pixelSize: 10
                }
            }
            Item { Layout.fillWidth: true }

            Rectangle {
                Layout.preferredWidth: 206
                Layout.preferredHeight: 40
                radius: 20
                color: Theme.accentSoft
                border.color: root.exportDirty ? Theme.warning : Theme.accent

                Column {
                    anchors.centerIn: parent
                    spacing: 1
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("已选 %1 个  ·  总时长 %2")
                              .arg(root.selectedCount()).arg(root.formatSeconds(root.selectedDuration()))
                        color: root.exportDirty ? Theme.warning : Theme.accent
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: root.exportDirty
                        text: qsTr("方案已修改，需要重新推送")
                        color: Theme.warning
                        font.pixelSize: 8
                    }
                }
            }

            Button {
                Layout.preferredWidth: 152
                Layout.preferredHeight: 36
                enabled: root.selectedCount() > 0
                text: root.exportDirty
                      ? qsTr("重新推送到导出")
                      : (root.hasPushed ? qsTr("再次推送到导出") : qsTr("推送到视频导出"))
                onClicked: root.pushToExport()
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
                    color: parent.enabled ? Theme.accent : Theme.panelRaised
                    border.color: parent.enabled ? Theme.accent : Theme.border
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
                    id: assemblyVideo
                    anchors.fill: parent
                    fillMode: VideoOutput.PreserveAspectFit
                }

                Column {
                    anchors.centerIn: parent
                    spacing: 8
                    visible: root.activeIndex < 0 || !root.importer.ready
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("请选择一个回合")
                        color: Theme.text
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("选中的回合会按右侧顺序连续预览")
                        color: Theme.textMuted
                        font.pixelSize: 10
                    }
                }

                Rectangle {
                    anchors.centerIn: parent
                    width: 60
                    height: 60
                    radius: 30
                    visible: root.activeIndex >= 0 && root.importer.ready
                    color: assemblyPlayArea.containsMouse ? Theme.accent : "#d9364b5d"
                    border.color: assemblyPlayArea.containsMouse ? Theme.accent : "#718298"
                    Text {
                        anchors.centerIn: parent
                        text: assemblyPlayer.playbackState === MediaPlayer.PlayingState ? "Ⅱ" : "▶"
                        color: Theme.text
                        font.pixelSize: 18
                    }
                    MouseArea {
                        id: assemblyPlayArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: assemblyPlayer.playbackState === MediaPlayer.PlayingState
                                   ? assemblyPlayer.pause() : root.selectRow(root.activeIndex, true)
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

                        CompactButton {
                            Layout.preferredWidth: 58
                            Layout.preferredHeight: 30
                            enabled: root.findSelected(root.activeIndex, -1) >= 0
                            text: qsTr("上一段")
                            onClicked: root.selectRow(root.findSelected(root.activeIndex, -1), false)
                        }
                        Slider {
                            id: assemblySlider
                            Layout.fillWidth: true
                            from: 0
                            to: Math.max(1, assemblyPlayer.duration)
                            onMoved: assemblyPlayer.position = value
                            Binding on value {
                                value: assemblyPlayer.position
                                when: !assemblySlider.pressed
                            }
                        }
                        Text {
                            text: root.formatSeconds(assemblyPlayer.position / 1000)
                            color: Theme.textMuted
                            font.pixelSize: 10
                        }
                        CompactButton {
                            Layout.preferredWidth: 62
                            Layout.preferredHeight: 30
                            text: root.muted ? qsTr("开启声音") : qsTr("静音")
                            onClicked: root.muted = !root.muted
                        }
                        CompactButton {
                            Layout.preferredWidth: 58
                            Layout.preferredHeight: 30
                            enabled: root.findSelected(root.activeIndex, 1) >= 0
                            text: qsTr("下一段")
                            onClicked: root.selectRow(root.findSelected(root.activeIndex, 1), false)
                        }
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 470
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
                            text: qsTr("回合顺序")
                            color: Theme.text
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: qsTr("%1 / %2 个回合").arg(root.selectedCount()).arg(assemblyModel.count)
                            color: Theme.textMuted
                            font.pixelSize: 10
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        CompactButton {
                            Layout.preferredWidth: 66
                            Layout.preferredHeight: 28
                            text: qsTr("全部显示")
                            enabled: assemblyModel.count > 0 && root.selectedCount() < assemblyModel.count
                            onClicked: root.setAllSelected(true)
                        }
                        CompactButton {
                            Layout.preferredWidth: 66
                            Layout.preferredHeight: 28
                            text: qsTr("全部隐藏")
                            enabled: root.selectedCount() > 0
                            onClicked: root.setAllSelected(false)
                        }
                        CompactButton {
                            Layout.preferredWidth: 82
                            Layout.preferredHeight: 28
                            checkable: true
                            checked: root.showSelectedOnly
                            text: checked ? qsTr("显示全部") : qsTr("仅看已选")
                            enabled: assemblyModel.count > 0
                            onClicked: root.showSelectedOnly = !root.showSelectedOnly
                        }
                        Item { Layout.fillWidth: true }
                        CompactButton {
                            Layout.preferredWidth: 96
                            Layout.preferredHeight: 28
                            text: qsTr("恢复原始顺序")
                            enabled: assemblyModel.count > 1
                            onClicked: restoreOrderDialog.open()
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("拖动 ≡ 调整顺序；三击回合卡片添加或修改备注")
                        color: Theme.textDim
                        font.pixelSize: 9
                    }

                    ListView {
                        id: assemblyList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        model: assemblyModel
                        spacing: root.showSelectedOnly ? 0 : 6
                        clip: true
                        ScrollBar.vertical: ScrollBar { }

                        delegate: ItemDelegate {
                            id: assemblyDelegate
                            required property int index
                            required property var modelData
                            property int clickStreak: 0
                            readonly property bool filteredOut: root.showSelectedOnly
                                                                && !assemblyDelegate.modelData.selected
                            width: assemblyList.width
                            height: filteredOut
                                    ? 0
                                    : ((assemblyDelegate.modelData.remark || "").length > 0 ? 68 : 58)
                            visible: !filteredOut
                            hoverEnabled: true
                            onClicked: {
                                root.selectRow(index, false)
                                clickStreak += 1
                                tripleClickReset.restart()
                                if (clickStreak >= 3) {
                                    clickStreak = 0
                                    tripleClickReset.stop()
                                    root.openRemark(index)
                                }
                            }

                            Timer {
                                id: tripleClickReset
                                interval: 560
                                repeat: false
                                onTriggered: assemblyDelegate.clickStreak = 0
                            }

                            background: Rectangle {
                                radius: Theme.radiusSmall
                                color: root.activeIndex === assemblyDelegate.index
                                       ? Theme.panelHover
                                       : (assemblyDelegate.hovered ? Theme.panelRaised : Theme.panel)
                                border.color: root.activeIndex === assemblyDelegate.index ? Theme.accent : Theme.border
                            }
                            opacity: assemblyDelegate.modelData.selected ? 1.0 : 0.45

                            contentItem: RowLayout {
                                spacing: 5

                                Item {
                                    Layout.preferredWidth: 18
                                    Layout.fillHeight: true

                                    Text {
                                        anchors.centerIn: parent
                                        text: "≡"
                                        color: dragHandleArea.pressed ? Theme.accent : Theme.textDim
                                        font.pixelSize: 17
                                    }
                                    MouseArea {
                                        id: dragHandleArea
                                        anchors.fill: parent
                                        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                                        onPressed: root.selectRow(assemblyDelegate.index, false)
                                        onPositionChanged: function(mouse) {
                                            if (!pressed)
                                                return
                                            const point = mapToItem(assemblyList.contentItem, mouse.x, mouse.y)
                                            const targetIndex = assemblyList.indexAt(point.x, point.y)
                                            if (targetIndex >= 0 && targetIndex !== assemblyDelegate.index)
                                                root.moveRow(assemblyDelegate.index, targetIndex)
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.preferredWidth: 42
                                    Layout.preferredHeight: 38
                                    radius: 8
                                    color: root.activeIndex === assemblyDelegate.index
                                           ? Theme.accent : Theme.panelRaised

                                    Column {
                                        anchors.centerIn: parent
                                        spacing: 0
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: qsTr("成片")
                                            color: root.activeIndex === assemblyDelegate.index
                                                   ? Theme.accentText : Theme.textDim
                                            font.pixelSize: 7
                                        }
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: String(assemblyDelegate.index + 1).padStart(2, "0")
                                            color: root.activeIndex === assemblyDelegate.index
                                                   ? Theme.accentText : Theme.textMuted
                                            font.pixelSize: 11
                                            font.weight: Font.Bold
                                        }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 90
                                    spacing: 1

                                    RowLayout {
                                        spacing: 6
                                        Text {
                                            text: qsTr("原回合 %1").arg(assemblyDelegate.modelData.rally)
                                            color: Theme.text
                                            font.pixelSize: 11
                                            font.weight: Font.DemiBold
                                        }
                                        Text {
                                            text: qsTr("%1 拍").arg(assemblyDelegate.modelData.hitCount)
                                            color: Theme.textMuted
                                            font.pixelSize: 9
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        visible: (assemblyDelegate.modelData.remark || "").length > 0
                                        text: assemblyDelegate.modelData.remark || ""
                                        color: Theme.accent
                                        font.pixelSize: 9
                                        elide: Text.ElideRight
                                    }
                                }

                                ColumnLayout {
                                    Layout.preferredWidth: 96
                                    spacing: 1
                                    Text {
                                        Layout.alignment: Qt.AlignRight
                                        text: root.formatSeconds(assemblyDelegate.modelData.startSeconds)
                                              + "–" + root.formatSeconds(assemblyDelegate.modelData.endSeconds)
                                        color: Theme.textMuted
                                        font.pixelSize: 9
                                    }
                                    Text {
                                        Layout.alignment: Qt.AlignRight
                                        text: qsTr("时长 %1").arg(root.formatSeconds(
                                                  assemblyDelegate.modelData.endSeconds
                                                  - assemblyDelegate.modelData.startSeconds))
                                        color: Theme.textDim
                                        font.pixelSize: 8
                                    }
                                }

                                CompactButton {
                                    id: visibilityButton
                                    Layout.preferredWidth: 42
                                    Layout.preferredHeight: 28
                                    text: assemblyDelegate.modelData.selected ? qsTr("隐藏") : qsTr("显示")
                                    onClicked: root.setRowSelected(
                                                   assemblyDelegate.index,
                                                   !assemblyDelegate.modelData.selected)
                                    contentItem: Text {
                                        text: visibilityButton.text
                                        color: assemblyDelegate.modelData.selected
                                               ? Theme.text : Theme.textDim
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                        font.pixelSize: 9
                                        font.weight: assemblyDelegate.modelData.selected
                                                     ? Font.Medium : Font.Normal
                                    }
                                    background: Rectangle {
                                        radius: Theme.radiusSmall
                                        color: assemblyDelegate.modelData.selected
                                               ? Theme.accentSoft : Theme.panel
                                        border.color: assemblyDelegate.modelData.selected
                                                      ? "#2e7964" : Theme.border
                                    }
                                }
                                CompactButton {
                                    Layout.preferredWidth: 24
                                    Layout.preferredHeight: 26
                                    enabled: assemblyDelegate.index > 0
                                    text: "↑"
                                    onClicked: root.moveRow(assemblyDelegate.index, assemblyDelegate.index - 1)
                                }
                                CompactButton {
                                    Layout.preferredWidth: 24
                                    Layout.preferredHeight: 26
                                    enabled: assemblyDelegate.index + 1 < assemblyModel.count
                                    text: "↓"
                                    onClicked: root.moveRow(assemblyDelegate.index, assemblyDelegate.index + 1)
                                }
                            }
                        }

                        Text {
                            anchors.centerIn: parent
                            visible: assemblyList.count === 0
                            text: root.analyzer.running ? qsTr("正在等待回合结果…") : qsTr("尚无可拼接回合")
                            color: Theme.textMuted
                            font.pixelSize: 11
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 112
            radius: Theme.radiusMedium
            color: Theme.panel
            border.color: Theme.border

            RowLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 14

                ColumnLayout {
                    Layout.preferredWidth: 150
                    spacing: 2
                    Text {
                        text: root.activeIndex >= 0
                              ? qsTr("回合 %1 · %2")
                                .arg(root.activeRallyNumber)
                                .arg(root.formatSeconds(root.rowDuration(assemblyModel.get(root.activeIndex))))
                              : qsTr("未选择回合")
                        color: Theme.text
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: root.activeIndex >= 0 && !assemblyModel.get(root.activeIndex).selected
                              ? qsTr("该回合已隐藏，不会进入成片")
                              : qsTr("微调当前回合的前后边界")
                        color: root.activeIndex >= 0 && !assemblyModel.get(root.activeIndex).selected
                               ? Theme.warning : Theme.textMuted
                        font.pixelSize: 9
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.fillHeight: true
                    Layout.topMargin: 8
                    Layout.bottomMargin: 8
                    color: Theme.border
                }

                ColumnLayout {
                    spacing: 4
                    Text { text: qsTr("开始时间（秒）"); color: Theme.textMuted; font.pixelSize: 9 }
                    RowLayout {
                        spacing: 5
                        TextField {
                            id: startField
                            Layout.preferredWidth: 72
                            Layout.preferredHeight: 32
                            enabled: root.activeIndex >= 0
                            selectByMouse: true
                            color: Theme.text
                            horizontalAlignment: Text.AlignHCenter
                            font.pixelSize: 10
                            validator: DoubleValidator { bottom: 0; decimals: 2 }
                            onEditingFinished: root.applyBoundary("start", text)
                            background: Rectangle {
                                radius: Theme.radiusSmall
                                color: Theme.navigation
                                border.color: startField.activeFocus ? Theme.accent : Theme.border
                            }
                        }
                        CompactButton {
                            Layout.preferredWidth: 48
                            text: "−0.5"
                            enabled: root.activeIndex >= 0
                            onClicked: root.nudgeBoundary("start", -0.5)
                        }
                        CompactButton {
                            Layout.preferredWidth: 48
                            text: "+0.5"
                            enabled: root.activeIndex >= 0
                            onClicked: root.nudgeBoundary("start", 0.5)
                        }
                    }
                }

                ColumnLayout {
                    spacing: 4
                    Text { text: qsTr("结束时间（秒）"); color: Theme.textMuted; font.pixelSize: 9 }
                    RowLayout {
                        spacing: 5
                        TextField {
                            id: endField
                            Layout.preferredWidth: 72
                            Layout.preferredHeight: 32
                            enabled: root.activeIndex >= 0
                            selectByMouse: true
                            color: Theme.text
                            horizontalAlignment: Text.AlignHCenter
                            font.pixelSize: 10
                            validator: DoubleValidator { bottom: 0; decimals: 2 }
                            onEditingFinished: root.applyBoundary("end", text)
                            background: Rectangle {
                                radius: Theme.radiusSmall
                                color: Theme.navigation
                                border.color: endField.activeFocus ? Theme.accent : Theme.border
                            }
                        }
                        CompactButton {
                            Layout.preferredWidth: 48
                            text: "−0.5"
                            enabled: root.activeIndex >= 0
                            onClicked: root.nudgeBoundary("end", -0.5)
                        }
                        CompactButton {
                            Layout.preferredWidth: 48
                            text: "+0.5"
                            enabled: root.activeIndex >= 0
                            onClicked: root.nudgeBoundary("end", 0.5)
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                ColumnLayout {
                    Layout.preferredWidth: 150
                    spacing: 4
                    Text {
                        text: root.activeIndex >= 0
                              ? qsTr("原始边界  %1–%2")
                                .arg(root.formatSeconds(assemblyModel.get(root.activeIndex).originalStartSeconds))
                                .arg(root.formatSeconds(assemblyModel.get(root.activeIndex).originalEndSeconds))
                              : qsTr("原始边界  —")
                        color: Theme.textMuted
                        font.pixelSize: 9
                    }
                    CompactButton {
                        Layout.fillWidth: true
                        text: qsTr("恢复原始边界")
                        enabled: root.activeIndex >= 0
                        onClicked: root.restoreActiveBoundary()
                    }
                    Text {
                        text: qsTr("三击右侧回合卡片编辑备注")
                        color: Theme.textDim
                        font.pixelSize: 8
                    }
                }
            }
        }
    }
}
