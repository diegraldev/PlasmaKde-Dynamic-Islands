import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.FormLayout {
    id: page

    property alias cfg_backgroundEnabled: bgSwitch.checked
    property alias cfg_backgroundOpacity: opacitySlider.value
    property alias cfg_borderEnabled: borderSwitch.checked
    property alias cfg_followSystemTheme: themeSwitch.checked
    property string cfg_backgroundColor: "#0b1622"
    property string cfg_accentColor: "#8d5cff"
    property string cfg_idleDotColor: "#55e36a"
    property string cfg_sharingDotColor: "#ffaa33"
    property int cfg_audioBarsStyle: 0

    QQC2.Switch {
        id: themeSwitch
        Kirigami.FormData.label: "Plasma theme:"
        text: "Use the desktop theme's text and panel colors"
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        opacity: 0.7
        font: Kirigami.Theme.smallFont
        text: "When on, text follows your color scheme and the custom background hex is replaced by the theme background."
    }

    Item { Kirigami.FormData.isSection: true }

    QQC2.Switch {
        id: bgSwitch
        Kirigami.FormData.label: "Expanded background:"
        text: "Fill the big panel (the compact capsule stays transparent)"
    }

    RowLayout {
        Kirigami.FormData.label: "Background color:"
        enabled: bgSwitch.checked
        Repeater {
            model: ["#0b1622", "#000000", "#101418", "#1a1030", "#0e1f1a", "#241016"]
            ColorSwatch { swatch: modelData; selected: page.cfg_backgroundColor === modelData; onPicked: page.cfg_backgroundColor = modelData }
        }
    }

    QQC2.TextField {
        Kirigami.FormData.label: "Custom background (hex):"
        enabled: bgSwitch.checked
        text: page.cfg_backgroundColor
        inputMask: "\\#HHHHHH"
        onEditingFinished: if (text.length === 7) page.cfg_backgroundColor = text
    }

    RowLayout {
        Kirigami.FormData.label: "Background opacity:"
        enabled: bgSwitch.checked
        QQC2.Slider { id: opacitySlider; from: 20; to: 100; stepSize: 1; Layout.preferredWidth: Kirigami.Units.gridUnit * 10 }
        QQC2.Label { text: opacitySlider.value + "%" }
    }

    QQC2.Switch {
        id: borderSwitch
        Kirigami.FormData.label: "Border:"
        enabled: bgSwitch.checked
        text: "Show a subtle outline"
    }

    Item { Kirigami.FormData.isSection: true }

    RowLayout {
        Kirigami.FormData.label: "Accent color:"
        Repeater {
            model: ["#8d5cff", "#2da6e8", "#55e36a", "#ff4f6f", "#ffaa33", "#ffffff"]
            ColorSwatch { swatch: modelData; selected: page.cfg_accentColor === modelData; onPicked: page.cfg_accentColor = modelData }
        }
    }

    QQC2.TextField {
        Kirigami.FormData.label: "Custom accent (hex):"
        text: page.cfg_accentColor
        inputMask: "\\#HHHHHH"
        onEditingFinished: if (text.length === 7) page.cfg_accentColor = text
    }

    RowLayout {
        Kirigami.FormData.label: "Idle dot color:"
        Repeater {
            model: ["#55e36a", "#2da6e8", "#8d5cff", "#ffffff", "#ffaa33"]
            ColorSwatch { swatch: modelData; selected: page.cfg_idleDotColor === modelData; onPicked: page.cfg_idleDotColor = modelData }
        }
    }

    RowLayout {
        Kirigami.FormData.label: "Sharing dot color:"
        Repeater {
            model: ["#ffaa33", "#ff4f6f", "#ffd233", "#2da6e8", "#ffffff"]
            ColorSwatch { swatch: modelData; selected: page.cfg_sharingDotColor === modelData; onPicked: page.cfg_sharingDotColor = modelData }
        }
    }

    Item { Kirigami.FormData.isSection: true }

    QQC2.ComboBox {
        id: barsStyleCombo
        Kirigami.FormData.label: "Audio bars skin:"
        model: [
            "Classic — bars with peak caps",
            "Mirror — grows from the centre",
            "Dots — LED segments",
            "Wave — smooth waveform",
            "Neon — accent glowing tubes"
        ]
        currentIndex: page.cfg_audioBarsStyle
        onActivated: page.cfg_audioBarsStyle = currentIndex
    }

    QQC2.Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        opacity: 0.7
        font: Kirigami.Theme.smallFont
        text: {
            switch (barsStyleCombo.currentIndex) {
            case 0:
                return "Bars grow from the bottom and carry a peak cap that falls on its own."
            case 1:
                return "Bars grow symmetrically from the vertical centre, like the original capsule."
            case 2:
                return "Each band is a column of LED dots that light up with the level."
            case 3:
                return "A smooth filled waveform connects the five bands."
            default:
                return "Thin accent-coloured tubes with a soft glow and a bright tip."
            }
        }
    }

    component ColorSwatch: Rectangle {
        property string swatch: "#ffffff"
        property bool selected: false
        signal picked()

        width: Kirigami.Units.gridUnit * 1.6
        height: width
        radius: width / 2
        color: swatch
        border.width: selected ? 3 : 1
        border.color: selected ? Kirigami.Theme.highlightColor : Qt.rgba(1, 1, 1, 0.25)

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: parent.picked()
        }
    }
}