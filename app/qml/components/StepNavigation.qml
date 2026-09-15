import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BadmintonAnalyzer

Rectangle {
    id: root

    required property int currentStep
    signal stepSelected(int index)

    color: Theme.navigation
    border.color: Theme.border

    ListModel {
        id: steps
        ListElement { stepTitle: "视频导入"; stepCaption: "选择并检查源素材" }
        ListElement { stepTitle: "回合分析"; stepCaption: "增量识别并查看回合" }
        ListElement { stepTitle: "回合拼接"; stepCaption: "选择、排序与调整时间" }
        ListElement { stepTitle: "视频导出"; stepCaption: "预览并输出最终视频" }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        Text {
            text: qsTr("工作流程")
            color: Theme.textMuted
            font.pixelSize: 11
            font.weight: Font.DemiBold
            font.letterSpacing: 1.2
            Layout.leftMargin: 10
            Layout.topMargin: 8
            Layout.bottomMargin: 3
        }

        Repeater {
            model: steps

            delegate: ItemDelegate {
                id: stepDelegate

                required property int index
                required property string stepTitle
                required property string stepCaption

                Layout.fillWidth: true
                Layout.preferredHeight: 70
                hoverEnabled: true
                onClicked: root.stepSelected(index)

                background: Rectangle {
                    radius: Theme.radiusMedium
                    color: root.currentStep === stepDelegate.index
                           ? Theme.accentSoft
                           : (stepDelegate.hovered ? Theme.panelRaised : "transparent")
                    border.color: root.currentStep === stepDelegate.index ? "#2a745f" : "transparent"
                }

                contentItem: RowLayout {
                    spacing: 11

                    Rectangle {
                        Layout.preferredWidth: 30
                        Layout.preferredHeight: 30
                        radius: 15
                        color: root.currentStep === stepDelegate.index ? Theme.accent : Theme.panelRaised
                        border.color: root.currentStep === stepDelegate.index ? Theme.accent : Theme.borderStrong

                        Text {
                            anchors.centerIn: parent
                            text: stepDelegate.index + 1
                            color: root.currentStep === stepDelegate.index ? Theme.accentText : Theme.textMuted
                            font.pixelSize: 12
                            font.weight: Font.Bold
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Text {
                            text: stepDelegate.stepTitle
                            color: root.currentStep === stepDelegate.index ? Theme.text : Theme.textMuted
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }

                        Text {
                            Layout.fillWidth: true
                            text: stepDelegate.stepCaption
                            color: Theme.textDim
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 72
            radius: Theme.radiusMedium
            color: Theme.panel
            border.color: Theme.border

            Column {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 12
                spacing: 5

                Text {
                    text: qsTr("TrackNet 分析流程")
                    color: Theme.text
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                }

                Text {
                    width: parent.width
                    text: qsTr("回合结果可随时停止并继续")
                    color: Theme.textDim
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
