pragma Singleton

import QtQuick

QtObject {
    readonly property color window: "#0b0f16"
    readonly property color navigation: "#0e141e"
    readonly property color panel: "#111925"
    readonly property color panelRaised: "#172130"
    readonly property color panelHover: "#1c2838"
    readonly property color border: "#263446"
    readonly property color borderStrong: "#35465d"

    readonly property color text: "#edf3fa"
    readonly property color textMuted: "#8f9daf"
    readonly property color textDim: "#667487"

    readonly property color accent: "#42d6a4"
    readonly property color accentSoft: "#173e35"
    readonly property color accentText: "#071510"
    readonly property color warning: "#f1b55f"
    readonly property color danger: "#ef7182"
    readonly property color timeline: "#222e3d"

    readonly property int radiusSmall: 7
    readonly property int radiusMedium: 11
    readonly property int radiusLarge: 15
}
