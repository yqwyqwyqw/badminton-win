import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import BadmintonAnalyzer

Rectangle {
    id: root

    required property QtObject analyzer
    required property QtObject importer
    required property int selectedIndex
    property bool fullMode: false
    property int exportIndex: -1
    property bool hasSelection: selectedIndex >= 0 && selectedIndex < analyzer.rallyCount
    property var qualityOptions: {
        const options = [ { label: qsTr("720p 快速"), code: "720p" } ]
        if (importer.sourceHeight > 1080)
            options.push({ label: qsTr("1080p 高清"), code: "1080p" })
        options.push({
            label: qsTr("原始画质 · %1").arg(importer.resolutionText),
            code: "source"
        })
        return options
    }
    signal rallySelected(int index)

    color: Theme.navigation
    border.color: Theme.border

    FolderDialog {
        id: exportFolder
        title: qsTr("选择回合短片导出目录")
        onAccepted: root.analyzer.exportRally(
                        root.exportIndex, selectedFolder, qualityBox.currentValue)
    }

    Dialog {
        id: exportConfirm
        anchors.centerIn: parent
        modal: true
        width: 360
        title: qsTr("确认导出")
        standardButtons: Dialog.Ok | Dialog.Cancel
        closePolicy: Popup.CloseOnEscape
        onAccepted: {
            root.exportIndex = root.selectedIndex
            exportFolder.open()
        }

        contentItem: ColumnLayout {
            spacing: 8
            Text {
                text: qsTr("导出回合 %1？").arg(root.selectedIndex + 1)
                color: Theme.text
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
            Text {
                text: qsTr("画质：%1").arg(qualityBox.currentText)
                color: Theme.textMuted
                font.pixelSize: 11
            }
        }

        background: Rectangle {
            radius: Theme.radiusMedium
            color: Theme.panelRaised
            border.color: Theme.borderStrong
        }
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
                        text: root.fullMode ? qsTr("全部回合") : qsTr("分析回合")
                    color: Theme.text
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                    Text {
                        text: root.fullMode ? qsTr("边分析边追加，点击即可播放") : qsTr("识别完成后逐个出现")
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
                height: 56
                hoverEnabled: true
                onClicked: root.rallySelected(index)

                background: Rectangle {
                    radius: Theme.radiusMedium
                    color: root.selectedIndex === rallyDelegate.index
                           ? Theme.panelHover
                           : (rallyDelegate.hovered ? Theme.panelRaised : Theme.panel)
                    border.color: root.selectedIndex === rallyDelegate.index ? Theme.accent : Theme.border
                }

                contentItem: RowLayout {
                    spacing: 10
                    Text {
                        text: qsTr("回合 %1").arg(rallyDelegate.modelData.rally)
                        color: Theme.text
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: qsTr("%1 拍").arg(rallyDelegate.modelData.hitCount)
                        color: Theme.textMuted
                        font.pixelSize: 10
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: rallyDelegate.modelData.timeText
                        color: Theme.textMuted
                        font.pixelSize: 10
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
            Layout.preferredHeight: 130
            radius: Theme.radiusMedium
            color: Theme.panel
            border.color: Theme.border

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: qsTr("单回合导出画质")
                        color: Theme.textMuted
                        font.pixelSize: 9
                    }
                    ComboBox {
                        id: qualityBox
                        Layout.fillWidth: true
                        model: root.qualityOptions
                        textRole: "label"
                        valueRole: "code"
                    }
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: root.analyzer.exporting
                          ? qsTr("正在导出 · %1%").arg(Math.round(root.analyzer.exportProgress * 100))
                          : qsTr("所有档位均不会放大超过原始素材")
                    color: Theme.text
                    font.pixelSize: 10
                }

                Button {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
                    enabled: root.hasSelection && !root.analyzer.exporting
                    text: root.analyzer.exporting
                          ? qsTr("正在导出…")
                          : (root.hasSelection
                             ? qsTr("导出已选回合 %1").arg(root.selectedIndex + 1)
                             : qsTr("请先选择回合"))
                    onClicked: exportConfirm.open()
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
        }
    }
}
