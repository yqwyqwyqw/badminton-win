import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BadmintonAnalyzer

Rectangle {
    id: root

    required property var model
    required property int selectedIndex
    signal rallySelected(int index)
    signal hitCountChanged(int index, int count)

    color: Theme.navigation
    border.color: Theme.border

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 18
        anchors.bottomMargin: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true

            ColumnLayout {
                spacing: 2

                Text {
                    text: qsTr("回合列表")
                    color: Theme.text
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                Text {
                    text: qsTr("分析完成后将逐个出现")
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
                    text: root.model.count
                    color: Theme.accent
                    font.pixelSize: 12
                    font.weight: Font.Bold
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            radius: Theme.radiusSmall
            color: Theme.panel
            border.color: Theme.border

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 11
                anchors.rightMargin: 11
                spacing: 8

                Text {
                    text: qsTr("当前仅显示演示数据")
                    color: Theme.textMuted
                    font.pixelSize: 10
                }

                Item { Layout.fillWidth: true }

                Text {
                    text: qsTr("可交互")
                    color: Theme.accent
                    font.pixelSize: 10
                }
            }
        }

        ListView {
            id: rallyList

            Layout.fillWidth: true
            Layout.fillHeight: true
            model: root.model
            spacing: 9
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }

            delegate: ItemDelegate {
                id: rallyDelegate

                required property int index
                required property int rallyNumber
                required property string timeRange
                required property int autoHits
                required property int effectiveHits
                required property string reviewState

                width: rallyList.width
                height: 112
                hoverEnabled: true
                onClicked: root.rallySelected(index)

                background: Rectangle {
                    radius: Theme.radiusMedium
                    color: root.selectedIndex === rallyDelegate.index
                           ? Theme.panelHover
                           : (rallyDelegate.hovered ? Theme.panelRaised : Theme.panel)
                    border.color: root.selectedIndex === rallyDelegate.index ? Theme.accent : Theme.border
                    border.width: root.selectedIndex === rallyDelegate.index ? 1 : 1
                }

                contentItem: ColumnLayout {
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true

                        Text {
                            text: qsTr("回合 %1").arg(rallyDelegate.rallyNumber)
                            color: Theme.text
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }

                        Text {
                            text: rallyDelegate.timeRange
                            color: Theme.textMuted
                            font.pixelSize: 10
                        }

                        Item { Layout.fillWidth: true }

                        Text {
                            text: rallyDelegate.reviewState
                            color: rallyDelegate.reviewState === "待复核" ? Theme.warning : Theme.accent
                            font.pixelSize: 9
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 7

                        Text {
                            text: qsTr("拍数")
                            color: Theme.textMuted
                            font.pixelSize: 10
                        }

                        Button {
                            Layout.preferredWidth: 26
                            Layout.preferredHeight: 26
                            text: "−"
                            onClicked: {
                                root.rallySelected(rallyDelegate.index)
                                root.hitCountChanged(rallyDelegate.index,
                                                     Math.max(1, rallyDelegate.effectiveHits - 1))
                            }
                            contentItem: Text {
                                text: parent.text
                                color: Theme.text
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                font.pixelSize: 15
                            }
                            background: Rectangle {
                                radius: 6
                                color: parent.hovered ? Theme.borderStrong : Theme.panelRaised
                                border.color: Theme.borderStrong
                            }
                        }

                        Text {
                            Layout.preferredWidth: 23
                            text: rallyDelegate.effectiveHits
                            color: Theme.text
                            horizontalAlignment: Text.AlignHCenter
                            font.pixelSize: 15
                            font.weight: Font.Bold
                        }

                        Button {
                            Layout.preferredWidth: 26
                            Layout.preferredHeight: 26
                            text: "+"
                            onClicked: {
                                root.rallySelected(rallyDelegate.index)
                                root.hitCountChanged(rallyDelegate.index,
                                                     rallyDelegate.effectiveHits + 1)
                            }
                            contentItem: Text {
                                text: parent.text
                                color: Theme.text
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                font.pixelSize: 14
                            }
                            background: Rectangle {
                                radius: 6
                                color: parent.hovered ? Theme.borderStrong : Theme.panelRaised
                                border.color: Theme.borderStrong
                            }
                        }

                        Text {
                            text: qsTr("自动 %1").arg(rallyDelegate.autoHits)
                            color: Theme.textDim
                            font.pixelSize: 9
                        }

                        Item { Layout.fillWidth: true }

                        Rectangle {
                            Layout.preferredWidth: 58
                            Layout.preferredHeight: 26
                            radius: 6
                            color: root.selectedIndex === rallyDelegate.index ? Theme.accent : Theme.panelRaised

                            Text {
                                anchors.centerIn: parent
                                text: qsTr("预览")
                                color: root.selectedIndex === rallyDelegate.index ? Theme.accentText : Theme.textMuted
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                            }
                        }
                    }
                }
            }
        }

        Button {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            text: qsTr("导出所选回合（占位）")

            contentItem: Text {
                text: parent.text
                color: Theme.accent
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                font.pixelSize: 11
                font.weight: Font.DemiBold
            }

            background: Rectangle {
                radius: Theme.radiusSmall
                color: parent.hovered ? Theme.accentSoft : Theme.panel
                border.color: "#2e7964"
            }
        }
    }
}
