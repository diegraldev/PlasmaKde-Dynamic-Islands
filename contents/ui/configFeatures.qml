import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

Kirigami.FormLayout {
    property alias cfg_enableMedia: mediaSwitch.checked
    property alias cfg_enableKeyboard: keyboardSwitch.checked
    property alias cfg_enableCamera: cameraSwitch.checked
    property alias cfg_enableMic: micSwitch.checked
    property alias cfg_enableTimer: timerSwitch.checked
    property alias cfg_enableBattery: batterySwitch.checked
    property alias cfg_enableVolume: volumeSwitch.checked
    property alias cfg_enableDownloads: downloadsSwitch.checked
    property alias cfg_enableScreenSharing: sharingSwitch.checked
    property alias cfg_ideBuildEnabled: ideSwitch.checked

    QQC2.Switch { id: mediaSwitch;     Kirigami.FormData.label: "Media:";          text: "Now playing / MPRIS" }
    QQC2.Switch { id: keyboardSwitch;  Kirigami.FormData.label: "Keyboard layout:"; text: "Announce layout changes" }
    QQC2.Switch { id: cameraSwitch;    Kirigami.FormData.label: "Camera:";          text: "Announce camera availability changes" }
    QQC2.Switch { id: micSwitch;       Kirigami.FormData.label: "Microphone:";      text: "Announce when the microphone is muted or unmuted" }
    QQC2.Switch { id: downloadsSwitch; Kirigami.FormData.label: "Downloads:";      text: "Show active jobs / progress" }
    QQC2.Switch { id: sharingSwitch;   Kirigami.FormData.label: "Screen sharing:"; text: "Show capture / presentation state" }
    QQC2.Switch { id: ideSwitch;       Kirigami.FormData.label: "IntelliJ IDEA:";  text: "Show build results (e.g. JAR build succeeded)" }
    QQC2.Switch { id: timerSwitch;     Kirigami.FormData.label: "Clock timer:";    text: "Announce when a timer finishes (KClock)" }
    QQC2.Switch { id: batterySwitch;   Kirigami.FormData.label: "Battery:";        text: "Announce charging / unplugged and low battery" }
    QQC2.Switch { id: volumeSwitch;    Kirigami.FormData.label: "Volume:";        text: "Announce volume / mute changes" }
}