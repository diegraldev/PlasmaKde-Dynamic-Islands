import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.FormLayout {
    id: page

    property alias cfg_moduleSeparators: separatorsSwitch.checked
    property alias cfg_popupGap: gapSpin.value

    QQC2.Switch {
        id: separatorsSwitch
        Kirigami.FormData.label: "Separators:"
        text: "Show “/” dividers between modules"
    }

    QQC2.Label {
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 20
        wrapMode: Text.WordWrap
        opacity: 0.7
        font: Kirigami.Theme.smallFont
        text: "“Content” is the active item — music, language, notification, download or status."
    }

    Item { Kirigami.FormData.isSection: true }

    QQC2.SpinBox {
        id: gapSpin
        Kirigami.FormData.label: "Distance from panel:"
        from: 0; to: 120; stepSize: 2
    }

    QQC2.Label {
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 20
        wrapMode: Text.WordWrap
        opacity: 0.7
        font: Kirigami.Theme.smallFont
        text: "Gap between the small capsule and the big expanded panel. Increase it if the panel appears too close to or overlapping the capsule."
    }
}