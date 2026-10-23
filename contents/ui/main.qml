import QtQuick
import QtQuick.Layouts
import AudioLevels 1.0
import CameraStatus 1.0
import MicState 1.0
import KeyState 1.0
import MsiEc 1.0
import VolumeWatcher 1.0
import org.kde.kirigami as Kirigami
import org.kde.notificationmanager as NotificationManager
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PlasmaComponents
import org.kde.plasma.plasmoid
import org.kde.plasma.private.mpris as Mpris
import org.kde.plasma.workspace.components as WorkspaceComponents
import org.kde.plasma.workspace.dbus as DBus

PlasmoidItem {
    id: root

    // MSI EC is read once at startup and refreshed only on popup open, after a
    // control action, or when the MSI WMI hotkey device emits an Fn key event.
    MsiEc {
        id: msi
    }

    // Five real spectrum bands from the default sink monitor. The stream is
    // opened only while media is playing and closed as soon as it pauses.
    AudioLevels {
        id: audioLevels
        running: root.enableMedia && root.mediaPlaying
    }

    Plasmoid.backgroundHints: PlasmaCore.Types.NoBackground

    // MSI EC controls are exposed through the applet context menu. They are
    // one-shot actions (no menu, daemon or polling): the write path uses the
    // sysfs attribute directly when permitted, otherwise a single pkexec call.
    Plasmoid.contextualActions: [
        PlasmaCore.Action {
            text: msi.coolerBoost ? "Disable Cooler Boost" : "Enable Cooler Boost"
            icon.name: "weather-windy"
            visible: msi.available
            enabled: msi.canControl
            onTriggered: root.toggleMsiCoolerBoost()
        },
        PlasmaCore.Action {
            text: "Fan mode: " + (msi.fanMode || "unknown")
            icon.name: "fan"
            visible: msi.available
            enabled: msi.canControl && msi.availableFanModes.length > 0
            onTriggered: root.cycleMsiFan()
        },
        PlasmaCore.Action {
            text: "Shift mode: " + (msi.shiftMode || "unknown")
            icon.name: "speedometer"
            visible: msi.available
            enabled: msi.canControl && msi.availableShiftModes.length > 0
            onTriggered: root.cycleMsiShift()
        },
        PlasmaCore.Action {
            text: msi.superBattery ? "Disable Super Battery" : "Enable Super Battery"
            icon.name: "battery-good"
            visible: msi.available
            enabled: msi.canControl
            onTriggered: root.toggleMsiSuperBattery()
        },
        PlasmaCore.Action {
            text: msi.webcamBlocked ? "Unblock Webcam" : "Block Webcam"
            icon.name: msi.webcamBlocked ? "camera-web" : "camera-off"
            visible: msi.available
            enabled: msi.canControl
            onTriggered: root.toggleMsiWebcam()
        }
    ]

    // Native hover tooltip showing the full text the compact capsule elides.
    toolTipMainText: {
        if (popupOpen) return ""
        if (showMedia) return mediaTitle || "Music"
        if (activeMode === 2) return notificationTitle || "Notification"
        if (compactTitle.length > 0) return compactTitle
        return timeText
    }
    toolTipSubText: {
        if (popupOpen) return ""
        if (showMedia) return mediaArtist || mediaIdentity || ""
        if (activeMode === 2) return notificationBody || notificationApp || ""
        if (activeMode === 15) return msi.model + (msi.cpuTemp > 0 ? " · CPU " + msi.cpuTemp + "°C" : "")
        if (battery.hasBattery && battery.percent >= 0) {
            return batteryCaption()
        }
        return ""
    }

    readonly property int compactMinWidth: 82
    readonly property int compactMaxWidth: 560
    readonly property int compactSidePadding: 1
    readonly property int compactLeadingWidth: {
        if (showGreenDot) return 10
        if (activeMode === 0) return 30 + 6 + 22
        if (activeMode === 14) return 136
        return 24
    }
    readonly property int compactTrailingWidth: (activeMode === 2 && unreadCount > 0) ? 22 : Math.ceil(compactTimeMetrics.width)
    readonly property int compactGapWidth: showGreenDot ? 8 : activeMode === 0 ? 24 : 16
    readonly property int compactSeparatorsWidth: moduleSeparators ? Math.max(0, compactVisibleList.length - 1) * 16 : 0
    readonly property int compactTextWidth: {
        const measured = Math.ceil(compactTitleMetrics.width)
        // When idle or empty text, don't reserve space
        if (!root.compactTitle || root.compactTitle.length === 0) {
            return showGreenDot ? 0 : 20
        }
        const textWidth = measured + 4  // 4px buffer for rounding
        const maxTextWidth = Math.min(300,
            compactMaxWidth - compactLeadingWidth - compactTrailingWidth
            - compactGapWidth - compactSeparatorsWidth - compactSidePadding * 2)
        return Math.max(0, Math.min(maxTextWidth, textWidth))
    }
    readonly property int compactContentWidth: compactLeadingWidth + compactTextWidth + compactTrailingWidth + compactGapWidth + compactSeparatorsWidth
    readonly property int compactWidth: Math.max(compactMinWidth, Math.min(compactMaxWidth, compactContentWidth + compactSidePadding * 2))
    readonly property int expandedWidth: activeMode === 0 ? Plasmoid.configuration.expandedWidthMusic
        : activeMode === 2 ? Plasmoid.configuration.expandedWidthNotification
        : activeMode === 3 && msi.available && msi.canControl
            ? Math.max(Plasmoid.configuration.expandedWidthStatus, 380)
            : Plasmoid.configuration.expandedWidthStatus
    readonly property int compactHeight: 32
    readonly property int expandedHeight: {
        const cfg = Plasmoid.configuration.expandedHeight
        if (activeMode === 3 && msi.available && msi.canControl) {
            // The idle popup also hosts the MSI control pills.
            return Math.max(cfg, 96)
        }
        if (activeMode !== 11 || !timerHasCount) return cfg
        // A running timer draws an extra row of icon controls along the bottom.
        // Keep the panel at the configured height whenever it already fits under
        // the status icon, only growing for very small panels.
        return Math.max(cfg, 100)
    }
    readonly property string accent: Plasmoid.configuration.accentColor
    readonly property int cornerRadius: Plasmoid.configuration.cornerRadius
    readonly property bool backgroundEnabled: Plasmoid.configuration.backgroundEnabled
    readonly property bool borderEnabled: Plasmoid.configuration.borderEnabled
    readonly property bool followTheme: Plasmoid.configuration.followSystemTheme
    readonly property color panelBackground: !backgroundEnabled ? "transparent"
        : withAlpha(followTheme ? Kirigami.Theme.backgroundColor : Plasmoid.configuration.backgroundColor, Plasmoid.configuration.backgroundOpacity)
    readonly property color borderColor: (borderEnabled && backgroundEnabled)
        ? (followTheme ? withAlpha(Kirigami.Theme.textColor, 16) : Qt.rgba(1, 1, 1, 0.16))
        : Qt.rgba(0, 0, 0, 0)
    // Primary/secondary text colors. On a transparent panel white reads best, but
    // when the user opts into the Plasma theme we follow its text color instead.
    readonly property color textPrimary: followTheme ? Kirigami.Theme.textColor : "white"
    readonly property color textSecondary: followTheme
        ? withAlpha(Kirigami.Theme.textColor, 75)
        : Qt.rgba(1, 1, 1, 0.74)
    readonly property string idleDotColor: Plasmoid.configuration.idleDotColor
    readonly property string sharingDotColor: Plasmoid.configuration.sharingDotColor
    readonly property string timeText: {
        let fmt = Plasmoid.configuration.use24HourClock ? "HH:mm" : "h:mm"
        if (Plasmoid.configuration.showSeconds) {
            fmt += ":ss"
        }
        if (!Plasmoid.configuration.use24HourClock) {
            fmt += " AP"
        }
        let out = Qt.formatTime(currentTime, fmt)
        if (Plasmoid.configuration.showDate) {
            out = Qt.formatDate(currentTime, "ddd d") + "  " + out
        }
        return out
    }
    readonly property bool enableMedia: Plasmoid.configuration.enableMedia
    readonly property bool enableNotifications: Plasmoid.configuration.enableNotifications
    readonly property bool enableKeyboard: Plasmoid.configuration.enableKeyboard
    readonly property bool enableDownloads: Plasmoid.configuration.enableDownloads
    readonly property bool enableScreenSharing: Plasmoid.configuration.enableScreenSharing
    readonly property bool enableTimer: Plasmoid.configuration.enableTimer
    readonly property bool moduleSeparators: Plasmoid.configuration.moduleSeparators
    readonly property var compactVisibleList: {
        let out = []
        let order = ["content", "time"]
        for (let i = 0; i < order.length; i++) {
            const m = order[i]
            if (m === "time" && activeMode === 2 && unreadCount > 0) {
                continue
            }
            out.push(m)
        }
        return out
    }

    readonly property int mediaCount: mediaRepeater.count
    readonly property bool hasMedia: mediaCount > 0
    readonly property bool mediaPlaying: mediaStatus === Mpris.PlaybackStatus.Playing
    readonly property bool showMedia: enableMedia && hasMedia && (mediaPlaying || modeIndex === 0)
    readonly property bool sharingScreen: enableScreenSharing && notificationSettings.notificationsInhibitedByApplication
    readonly property bool hasNotification: enableNotifications && (notificationPulse.running || unreadCount > 0)
    readonly property bool hasJobs: enableDownloads && jobsCount > 0
    readonly property bool sharingWithMedia: sharingScreen && showMedia
    readonly property bool idleMode: !sharingScreen && !eventTimer.running && !hasNotification && !buildTimer.running && !hasJobs && !showMedia && !timerRinging && !timerHasCount
    readonly property bool showGreenDot: idleMode && !root.batteryPersistent && !root.batteryWarnHold
    readonly property int activeMode: {
        if (buildTimer.running) {
            return 8
        }
        if (timerRinging) {
            return 11
        }
        if (root.batteryPersistent || root.batteryWarnHold) {
            return 12
        }
        if (root.volumeAnnounceActive) {
            return 14
        }
        if (sharingWithMedia) {
            return 0
        }
        if (hasNotification) {
            return 2
        }
        if (eventTimer.running && modeIndex !== 0) {
            return modeIndex
        }
        if (timerHasCount) {
            return 11
        }
        if (hasJobs) {
            return 5
        }
        if (showMedia) {
            return 0
        }
        if (sharingScreen) {
            return 7
        }
        return 3
    }
    readonly property string compactTitle: {
        if (showGreenDot) {
            return ""
        }
        if (activeMode === 0) {
            return mediaTitle || "Music"
        }
        if (activeMode === 2) {
            return notificationTitle || "Notification"
        }
        if (activeMode === 3) {
            return eventTimer.running ? (keyboardLongName || keyboardShortName || "Keyboard layout") : ""
        }
        if (activeMode === 5) {
            return jobsCount > 0 ? "Download active" : "Downloads"
        }
        if (activeMode === 7) {
            return "Sharing screen"
        }
        if (activeMode === 8) {
            return buildLabel || (buildSuccess ? "Build succeeded" : "Build failed")
        }
        if (activeMode === 9) {
            return eventTimer.running ? (cameraAvailable ? "Camera ON" : "Camera OFF") : ""
        }
        if (activeMode === 10) {
            return eventTimer.running ? (micMuted ? "Mic OFF" : "Mic ON") : ""
        }
        if (activeMode === 11) {
            if (timerRinging) {
                return timerLabel || "Timer complete"
            }
            return root.timerCountdownText
        }
        if (activeMode === 12) {
            return (eventTimer.running || root.batteryPersistent || root.batteryWarnHold) ? root.batteryMessage : ""
        }
        if (activeMode === 13) {
            return eventTimer.running ? root.lockTitle : ""
        }
        if (activeMode === 14) {
            return eventTimer.running ? (root.volWatcher.muted ? "0%" : root.volWatcher.volume + "%") : ""
        }
        if (activeMode === 15) {
            return eventTimer.running ? root.msiMessage : ""
        }
        return ""
    }

    property int modeIndex: 0
    property bool popupOpen: false
    property int mediaStatus: Mpris.PlaybackStatus.Stopped
    property int unreadCount: 0
    property int jobsCount: 0
    property int jobsPercent: 0
    property real mediaPosition: 0
    property real mediaLength: 0
    readonly property real mediaProgress: mediaLength > 0 ? Math.max(0, Math.min(1, mediaPosition / mediaLength)) : 0
    property date currentTime: new Date()
    property string mediaTitle: ""
    property string mediaArtist: ""
    property string mediaArtUrl: ""
    property string mediaIdentity: ""
    property string notificationTitle: ""
    property string notificationBody: ""
    property string notificationIcon: "notifications"
    property string notificationApp: ""
    property var notificationActionNames: []
    property var notificationActionLabels: []
    property bool notificationHasDefaultAction: false
    property string keyboardShortName: ""
    property string keyboardLongName: ""
    property bool keyboardReady: false
    readonly property bool enableCamera: Plasmoid.configuration.enableCamera
    property bool cameraAvailable: false
    property bool cameraReady: false
    readonly property bool cameraBlocked: msi.available && msi.webcamBlocked
    readonly property bool enableMic: Plasmoid.configuration.enableMic
    property bool micMuted: false
    property bool micReady: false
    readonly property bool enableBattery: Plasmoid.configuration.enableBattery
    readonly property bool enableVolume: Plasmoid.configuration.enableVolume
    property bool batteryLowShow: false
    property bool batteryWarnHold: false
    property string msiMessage: ""
    property string msiIcon: "weather-windy"
    readonly property string batteryMessage: {
        if (!battery.hasBattery || battery.percent < 0) {
            return ""
        }
        if (battery.critical) {
            return "Low battery " + battery.percent + "%"
        }
        if (batteryLowShow) {
            return "Low battery " + battery.percent + "%"
        }
        return root.batteryCaption()
    }
    readonly property bool batteryPersistent: battery.critical
    property var mediaContainer: null
    property bool buildSuccess: true
    property string buildLabel: ""
    property string buildApp: ""

    // Lock state announcement (caps/num/scroll). We track previous values so we
    // only announce on actual changes, and we build a combined message if both
    // toggled simultaneously (unlikely but handled).
    property string lockTitle: ""
    property bool prevCapsLock: false
    property bool prevNumLock: false
    property bool prevScrollLock: false
    property string lockIcon: ""

    // KClock timer state, driven purely by D-Bus signals (no polling). The
    // countdown derives from the stored end timestamp and the already-running
    // 1s clock update, so re-rendering is free (no extra timers / no polling).
    property bool timerRinging: false
    property bool timerHasCount: false
    property real timerEndMs: 0
    property bool timerFrozen: false
    property real timerFrozenMs: 0
    property string timerLabel: ""
    property string timerRingingUuid: ""
    property string timerCountUuid: ""
    property var timerMap: ({})
    property var timerWatchers: ({})

    // Live countdown text for the earliest-running timer. While running it counts
    // down from the stored end timestamp; while paused the remaining time is frozen
    // so the countdown stays visible (kclockd keeps no remaining-time property).
    readonly property string timerCountdownText: {
        if (!timerHasCount) {
            return ""
        }
        let remain
        if (timerFrozen) {
            remain = Math.max(0, Math.ceil(timerFrozenMs / 1000))
        } else {
            if (timerEndMs <= 0) {
                return ""
            }
            remain = Math.max(0, Math.ceil((timerEndMs - currentTime.getTime()) / 1000))
        }
        const h = Math.floor(remain / 3600)
        const m = Math.floor((remain % 3600) / 60)
        const s = remain % 60
        if (h > 0) {
            return h + ":" + String(m).padStart(2, "0") + ":" + String(s).padStart(2, "0")
        }
        return m + ":" + String(s).padStart(2, "0")
    }

    Layout.minimumWidth: compactWidth
    Layout.minimumHeight: compactHeight
    Layout.preferredWidth: compactWidth
    Layout.preferredHeight: compactHeight
    Layout.maximumWidth: compactWidth
    Layout.maximumHeight: compactHeight

    implicitWidth: Layout.preferredWidth
    implicitHeight: Layout.preferredHeight

    TextMetrics {
        id: compactTitleMetrics
        text: root.activeMode === 14 ? "100%" : (root.timerRinging ? "Timer c" : root.compactTitle)
        font.pointSize: 12
        font.weight: Font.Medium
    }

    TextMetrics {
        id: compactTimeMetrics
        text: root.timeText
        font.pointSize: 16
        font.weight: Font.Medium
    }

    // The clock is scheduled to the next minute unless seconds, a running
    // countdown or an open media popup actually need a one-second tick. This is
    // still a timer, not a polling loop: one wake-up per minute while idle.
    readonly property int clockInterval: {
        if (Plasmoid.configuration.showSeconds || root.timerHasCount
            || (root.mediaPlaying && root.popupOpen)) {
            return 1000
        }
        const now = root.currentTime.getTime()
        return Math.max(250, 60000 - (now % 60000) + 50)
    }

    Timer {
        id: clockTimer
        interval: root.clockInterval
        running: true
        repeat: true
        onTriggered: {
            root.currentTime = new Date()
            if (root.mediaContainer && root.mediaPlaying && root.popupOpen) {
                root.mediaContainer.updatePosition()
            }
        }
    }

    Timer {
        id: eventTimer
        interval: 3000
        onTriggered: {
            root.volumeAnnounceActive = false
        }
    }

    // Holds the battery warn capsule (15%) visible for the full 6s even though
    // eventTimer's shared 3s window would otherwise hide it first.
    Timer {
        id: batteryWarnHoldTimer
        interval: 6000
        onTriggered: {
            root.batteryWarnHold = false
        }
    }

    Timer {
        id: notificationPulse
        interval: 15000
    }

    // Blink the timer-complete message/icon while a timer is ringing to draw
    // attention. Toggled on a fixed interval; opacity is bound to the flag so
    // the element returns to full opacity cleanly when ringing stops.
    property bool timerBlinkOn: true
    Timer {
        id: timerBlink
        interval: 450
        repeat: true
        running: root.activeMode === 11 && root.timerRinging
        onTriggered: root.timerBlinkOn = !root.timerBlinkOn
    }

    Timer {
        id: buildTimer
        interval: 6000
    }

    // kclockd emits runningChanged/timeCompletedChanged at the same instant it
    // emits timerAdded, sometimes before our SignalWatcher exists. Re-read the
    // settled state once after a short delay instead of creating a Timer object
    // dynamically for every timer.
    Timer {
        id: timerSyncRetry
        interval: 1200
        property string timerUuid: ""
        onTriggered: {
            if (timerUuid.length > 0) {
                root.syncTimerState(timerUuid)
            }
        }
    }

    // Event-driven microphone mute state: the native MicState plugin connects
    // to PulseAudio and signals only when the default source mute actually
    // changes (subscribe-based). Zero polling, zero idle cost — same design as
    // the camera/keyboard watchers.
    MicState {
        id: micState
        running: root.enableMic
        onMutedChanged: (muted) => root.setMicState(muted)
    }

    // Event-driven battery state via UPower D-Bus signals (no polling). The
    // manager only emits on real transitions, so announcements never spam.
    BatteryManager {
        id: battery
        onAcStateChanged: root.updateBatteryAnnounce()
        onLowBattery: (percent) => root.updateBatteryLowAnnounce(percent)
        onWarnBattery: (percent) => root.updateBatteryWarnAnnounce(percent)
        onCriticalChanged: root.updateBatteryCritical()
    }

    // Event-driven Caps/Num/Scroll lock state via KWin's keystate protocol.
    // Zero polling, no sysfs/evdev permissions needed — subscribes to compositor.
    KeyStateWatcher {
        id: lockState
        running: root.enableKeyboard
        onCapsLockChanged: root.updateLockAnnounce()
        onNumLockChanged: root.updateLockAnnounce()
        onScrollLockChanged: root.updateLockAnnounce()
    }

    // Event-driven volume/mute state via `pactl subscribe`. Emits
    // volumeAnnounce on every real sink change (change/new) so the island can
    // pulse a transient volume capsule without polling.
    VolumeWatcher {
        id: volWatcher
        running: root.enableVolume
        onVolumeAnnounce: root.updateVolumeAnnounce()
        onMutedChanged: (muted) => {
            if (volWatcher.ready) {
                root.updateVolumeAnnounce()
            }
        }
    }

    // Expose volWatcher (declared above) as a property of root so it can be
    // referenced from nested QML scopes (compact layout, StatusIcon, etc.).
    readonly property alias volWatcher: volWatcher

    // Transient volume announcement state (mirrors the lock/battery patterns).
    property bool volumeAnnounceActive: false

    function updateVolumeAnnounce() {
        if (!enableVolume) {
            return
        }
        root.volumeAnnounceActive = true
        modeIndex = 14
        eventTimer.restart()
    }

    function withAlpha(hex, percent) {
        const c = Qt.lighter(hex, 1.0)
        return Qt.rgba(c.r, c.g, c.b, Math.max(0, Math.min(100, percent)) / 100)
    }

    function setMedia(roleModel) {
        mediaTitle = roleModel.track || ""
        mediaArtist = roleModel.artist || roleModel.identity || ""
        mediaArtUrl = roleModel.artUrl || ""
        mediaIdentity = roleModel.identity || ""
        mediaStatus = roleModel.playbackStatus
        mediaPosition = roleModel.position || 0
        mediaLength = roleModel.length || 0
        mediaContainer = roleModel.container
    }

    function setNotification(roleModel) {
        notificationTitle = roleModel.summary || roleModel.applicationName || "Notification"
        notificationBody = roleModel.body || roleModel.text || ""
        notificationApp = roleModel.applicationName || ""
        notificationIcon = roleModel.applicationIconName || roleModel.iconName || "notifications"
        if (notificationIcon.length === 0) {
            notificationIcon = "notifications"
        }
        notificationActionNames = roleModel.actionNames || []
        notificationActionLabels = roleModel.actionLabels || []
        notificationHasDefaultAction = roleModel.hasDefaultAction || false
    }

    function dismissNotification() {
        if (unreadCount > 0) {
            notificationsModel.close(notificationsModel.index(0, 0))
        }
        notificationPulse.stop()
    }

    // Activates the notification's default action — usually raising or opening the
    // source application. No-op when the notification doesn't declare one.
    function activateNotification() {
        if (notificationHasDefaultAction) {
            notificationsModel.invokeDefaultAction(notificationsModel.index(0, 0))
        }
        closePopup()
    }

    function invokeNotificationAction(i) {
        const names = notificationActionNames
        if (i < 0 || i >= names.length) {
            return
        }
        notificationsModel.invokeAction(notificationsModel.index(0, 0), names[i])
        closePopup()
    }

    // Scroll over the capsule controls the active media player: volume when the
    // player exposes it, otherwise skip to the next/previous track.
    function handleWheel(deltaY) {
        if (!showMedia || !mediaContainer || deltaY === 0) {
            return
        }
        const up = deltaY > 0
        if (typeof mediaContainer.volume === "number") {
            const step = 0.05
            mediaContainer.volume = Math.max(0, Math.min(1, mediaContainer.volume + (up ? step : -step)))
        } else if (up) {
            mediaContainer.Next()
        } else {
            mediaContainer.Previous()
        }
    }

    function handleBuildNotification(app, summary, body) {
        if (!Plasmoid.configuration.ideBuildEnabled) {
            return false
        }
        const hay = (app + " " + summary + " " + body).toLowerCase()
        const fromIde = hay.indexOf("intellij") !== -1 || hay.indexOf("idea") !== -1
            || hay.indexOf("jetbrains") !== -1 || hay.indexOf("android studio") !== -1
            || hay.indexOf("gradle") !== -1 || hay.indexOf("maven") !== -1
        const aboutBuild = hay.indexOf("build") !== -1 || hay.indexOf("jar") !== -1
            || hay.indexOf("compil") !== -1 || hay.indexOf("artifact") !== -1
        if (!fromIde || !aboutBuild) {
            return false
        }
        const failed = hay.indexOf("fail") !== -1 || hay.indexOf("error") !== -1
            || hay.indexOf("unsuccessful") !== -1
        buildSuccess = !failed
        buildApp = app || "IntelliJ IDEA"
        buildLabel = summary || (failed ? "Build failed" : "Build succeeded")
        buildTimer.restart()
        return true
    }

    // PlasmaCore.Dialog snaps the popup flush against the panel edge. On Wayland
    // the compositor also controls the window position, so the only reliable way
    // to push the panel down is to make the window taller and shift its content
    // down by this margin (pure QML layout, works on any compositor).
    readonly property int popupGapMargin: Plasmoid.configuration.popupGap
    // The gap belongs between the capsule and the panel: at the top when the
    // popup opens downwards, at the bottom when it opens upwards.
    readonly property int popupGapTop: popupLocation === PlasmaCore.Types.TopEdge ? popupGapMargin : 0
    readonly property int popupGapBottom: popupLocation === PlasmaCore.Types.BottomEdge ? popupGapMargin : 0

    // The direction is derived from the panel edge by PlasmaCore, so on a
    // floating/top island the popup would open upward, toward the top of the
    // screen. We pick the edge ourselves based on where the capsule sits:
    // top half of the screen -> expand downward, bottom half -> expand upward.
    readonly property var popupLocation: {
        const geo = Plasmoid.screenGeometry
        if (geo === null || geo === undefined) {
            return Plasmoid.location
        }
        const p = root.mapToGlobal(Qt.point(0, root.compactHeight))
        return p.y > geo.y + geo.height / 2 ? PlasmaCore.Types.BottomEdge : PlasmaCore.Types.TopEdge
    }

    Timer {
        id: popupCloseTimer
        interval: 130
        onTriggered: {
            popup.visible = false
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: popup.visible
        onActivated: root.closePopup()
    }

    function openPopup() {
        popupCloseTimer.stop()
        // One-shot refresh of the MSI snapshot (temperatures, fan mode, webcam
        // switch). No periodic polling; the plugin also refreshes on Fn keys.
        msi.refresh()
        popup.visible = true
        popupOpen = true
    }

    function closePopup() {
        popupOpen = false
        popupCloseTimer.restart()
    }

    function togglePopup() {
        if (popupOpen) {
            closePopup()
        } else {
            openPopup()
        }
    }

    WorkspaceComponents.KeyboardLayoutSwitcher {
        id: keyboardLayoutSwitcher

        width: 1
        height: 1
        visible: false
        acceptedButtons: Qt.NoButton

        readonly property string currentShortName: layoutNames.shortName || layoutNames.displayName || ""
        readonly property string currentLongName: layoutNames.longName || layoutNames.displayName || currentShortName

        Component.onCompleted: root.updateKeyboardLayout(false)
        onCurrentShortNameChanged: Qt.callLater(root.updateKeyboardLayout, root.keyboardReady)
    }

    function updateKeyboardLayout(announce) {
        keyboardShortName = normalizeLayoutName(keyboardLayoutSwitcher.currentShortName)
        keyboardLongName = keyboardLayoutSwitcher.currentLongName
        if (announce && enableKeyboard && keyboardShortName.length > 0) {
            modeIndex = 3
            eventTimer.restart()
        }
        keyboardReady = true
    }

    function normalizeLayoutName(name) {
        const lower = (name || "").toLowerCase()
        if (lower === "us" || lower === "en" || lower.indexOf("english") !== -1) {
            return "Eng"
        }
        if (lower === "ru" || lower.indexOf("russian") !== -1 || lower.indexOf("рус") !== -1) {
            return "Ru"
        }
        return name || ""
    }

    function updateCameraStatus(announce) {
        const available = CameraStatus.cameraAvailable && !root.cameraBlocked
        const changed = cameraReady && available !== cameraAvailable
        cameraAvailable = available
        if (announce && changed && enableCamera) {
            modeIndex = 9
            eventTimer.restart()
        }
        cameraReady = true
    }

    // Microphone mute state. The native MicState plugin pushes the current mute
    // state on every real change, so this only pulses the announcement when the
    // state differs from what we last announced.
    function setMicState(muted) {
        const changed = micReady && muted !== micMuted
        micMuted = muted
        if (changed && enableMic) {
            modeIndex = 10
            eventTimer.restart()
        }
        micReady = true
    }

    // Battery capsule text for the current UPower state.
    function batteryCaption() {
        if (!battery.hasBattery || battery.percent < 0) {
            return ""
        }
        if (battery.critical) {
            return "Low battery " + battery.percent + "%"
        }
        const time = root.formatRemaining(battery.remainingMsec)
        const suffix = time.length > 0 ? " · " + time : ""
        if (battery.chargeState === 4) {
            return "Battery full" + suffix
        }
        if (battery.chargeState === 1) {
            return "Charging " + battery.percent + "%" + suffix
        }
        if (battery.pluggedIn) {
            return "Plugged in " + battery.percent + "%" + suffix
        }
        return "On battery " + battery.percent + "%" + suffix
    }

    function formatRemaining(ms) {
        if (!ms || ms <= 0) {
            return ""
        }
        const totalMinutes = Math.round(ms / 60000)
        const hours = Math.floor(totalMinutes / 60)
        const minutes = totalMinutes % 60
        if (hours > 0) {
            return hours + "h " + (minutes < 10 ? "0" : "") + minutes + "m"
        }
        return minutes + "m"
    }

    function batteryIconName() {
        if (!battery.hasBattery) {
            return "battery-missing"
        }
        const p = battery.percent
        let level = "000"
        if (p >= 95) level = "100"
        else if (p >= 85) level = "090"
        else if (p >= 75) level = "080"
        else if (p >= 65) level = "070"
        else if (p >= 55) level = "060"
        else if (p >= 45) level = "050"
        else if (p >= 35) level = "040"
        else if (p >= 25) level = "030"
        else if (p >= 15) level = "020"
        else if (p > 5) level = "010"
        return "battery-" + level + (battery.charging ? "-charging" : "")
    }

    // MSI EC announcements. The action is applied by the plugin; the capsule
    // mirrors the requested state immediately and the plugin refreshes later.
    function announceMsi(message, icon) {
        root.msiMessage = message
        root.msiIcon = icon || "weather-windy"
        modeIndex = 15
        eventTimer.restart()
    }

    function toggleMsiCoolerBoost() {
        const next = !msi.coolerBoost
        msi.setCoolerBoost(next)
        root.announceMsi("Cooler Boost " + (next ? "on" : "off"), "weather-windy")
    }

    function cycleMsiFan() {
        const modes = msi.availableFanModes
        if (modes.length === 0) {
            return
        }
        const current = modes.indexOf(msi.fanMode)
        const next = modes[(current + 1) % modes.length]
        msi.setFanMode(next)
        root.announceMsi("Fan: " + next, "fan")
    }

    function cycleMsiShift() {
        const modes = msi.availableShiftModes
        if (modes.length === 0) {
            return
        }
        const current = modes.indexOf(msi.shiftMode)
        const next = modes[(current + 1) % modes.length]
        msi.setShiftMode(next)
        root.announceMsi("Shift: " + next, "speedometer")
    }

    function toggleMsiSuperBattery() {
        const next = !msi.superBattery
        msi.setSuperBattery(next)
        root.announceMsi("Super Battery " + (next ? "on" : "off"), "battery-good")
    }

    function toggleMsiWebcam() {
        const next = !msi.webcamBlocked
        msi.setWebcamBlocked(next)
        root.announceMsi("Webcam " + (next ? "blocked" : "enabled"), next ? "camera-off" : "camera-web")
    }

    function msiFanLabel() {
        return "Fan " + (msi.fanMode || "?")
    }

    function msiShiftLabel() {
        return "Shift " + (msi.shiftMode || "?")
    }

    // AC adapter plugged/unplugged. The capsule text is derived reactively from
    // the live state (batteryMessage), so it always shows the current values.
    function updateBatteryAnnounce() {
        root.batteryLowShow = false
        root.batteryWarnHold = false
        batteryWarnHoldTimer.stop()
        if (enableBattery) {
            modeIndex = 12
            eventTimer.restart()
        }
    }

    function updateBatteryLowAnnounce(percent) {
        root.batteryLowShow = true
        if (enableBattery) {
            modeIndex = 12
            eventTimer.restart()
        }
    }

    // Battery at warnThreshold (15%): a longer 6s capsule, still transient.
    // Only the 10% critical state persists until the adapter is plugged.
    function updateBatteryWarnAnnounce(percent) {
        root.batteryLowShow = true
        root.batteryWarnHold = true
        batteryWarnHoldTimer.restart()
        if (enableBattery) {
            modeIndex = 12
            eventTimer.restart()
        }
    }

    // Critical battery (<= criticalThreshold): the capsule stays visible even
    // after the event timer expires. It only goes away when the AC adapter is
    // plugged back in, which flips critical off and runs the normal 3s announce.
    function updateBatteryCritical() {
        root.batteryLowShow = false
        root.batteryWarnHold = false
        batteryWarnHoldTimer.stop()
        if (enableBattery) {
            modeIndex = 12
            eventTimer.restart()
        }
    }

    // Lock state change (Caps/Num/Scroll). Only announces on actual state change.
    // Uses enableKeyboard config gate (same as keyboard layout announcements).
    function updateLockAnnounce() {
        const caps = lockState.capsLock
        const num = lockState.numLock
        const scroll = lockState.scrollLock

        // Detect which lock(s) changed
        const capsChanged = caps !== root.prevCapsLock
        const numChanged = num !== root.prevNumLock
        const scrollChanged = scroll !== root.prevScrollLock

        if (!capsChanged && !numChanged && !scrollChanged) {
            return
        }

        // Build message and pick icon
        let parts = []
        let icon = ""
        if (capsChanged) {
            parts.push("Caps Lock " + (caps ? "On" : "Off"))
            icon = "keyboard-caps"
        }
        if (numChanged) {
            parts.push("Num Lock " + (num ? "On" : "Off"))
            icon = icon.length === 0 ? "keyboard-number" : icon
        }
        if (scrollChanged) {
            parts.push("Scroll Lock " + (scroll ? "On" : "Off"))
            icon = icon.length === 0 ? "keyboard-scroll" : icon
        }

        root.lockTitle = parts.join(" · ")
        root.lockIcon = icon

        // Update previous states
        root.prevCapsLock = caps
        root.prevNumLock = num
        root.prevScrollLock = scroll

        if (enableKeyboard) {
            modeIndex = 13
            eventTimer.restart()
        }
    }

    // Event-driven camera detection: the CameraStatus plugin watches /dev via
    // inotify and signals only when the set of video devices actually changes.
    // No polling, no QMediaDevices, no Solid.
    Connections {
        target: CameraStatus

        function onCameraAvailableChanged() {
            root.updateCameraStatus(true)
        }
        function onVideoCountChanged() {
            root.updateCameraStatus(true)
        }
    }

    // The MSI EC webcam kill switch does not create a udev event. The plugin
    // refreshes on Fn hotkey events (and on popup open), so react to its state
    // instead of scanning sysfs here.
    Connections {
        target: msi

        function onWebcamBlockedChanged() {
            root.updateCameraStatus(true)
        }
        function onAvailableChanged() {
            root.updateCameraStatus(true)
        }
        function onLastErrorChanged() {
            if (msi.lastError.length > 0) {
                root.announceMsi(msi.lastError, "dialog-error")
            }
        }
    }

    Component.onCompleted: {
        root.updateCameraStatus(false)
        if (root.enableTimer) {
            root.enumerateTimers()
        }
    }

    // --- KClock timer integration (event-driven, no polling) ---
    // kclockd (the KDE Clock daemon) keeps a D-Bus TimerModel at /Timers and one
    // Timer object per active timer at /Timers/<uuid>. A timer firing is signalled
    // by ringingChanged on its Timer object, so we attach one SignalWatcher per
    // known timer. Everything here fires only on real D-Bus events.

    // SignalWatcher for the timer list: signals are emitted at exactly /Timers.
    DBus.SignalWatcher {
        service: "org.kde.kclockd"
        path: "/Timers"
        iface: "org.kde.kclock.TimerModel"
        enabled: root.enableTimer
        function dbustimerAdded(uuid) {
            root.watchTimer(uuid)
        }
        function dbustimerRemoved(uuid) {
            root.unwatchTimer(uuid)
        }
    }

    // kclockd auto-exits when no alarm/timer is pending. When it appears (again)
    // re-enumerate existing timers once so the island stays in sync.
    DBus.DBusServiceWatcher {
        busType: DBus.BusType.Session
        watchedService: "org.kde.kclockd"
        onRegisteredChanged: {
            if (registered && root.enableTimer) {
                root.enumerateTimers()
            }
        }
    }

    // One lightweight watcher per timer object (Qt D-Bus path matching is exact).
    // kclockd emits these signals on every state transition (start/pause/reset/
    // ring/dismiss), so we catch the whole set — all event-driven, no polling.
    Component {
        id: timerWatcherComponent

        DBus.SignalWatcher {
            property string timerUuid: ""

            service: "org.kde.kclockd"
            path: "/Timers/" + timerUuid
            iface: "org.kde.kclock.Timer"
            enabled: root.enableTimer

            function dbusringingChanged() { root.syncTimerState(timerUuid) }
            function dbusrunningChanged() { root.syncTimerState(timerUuid) }
            function dbustimeCompletedChanged() { root.syncTimerState(timerUuid) }
            function dbuslengthChanged() { root.syncTimerState(timerUuid) }
            function dbuslabelChanged() { root.syncTimerState(timerUuid) }
        }
    }

    function enumerateTimers() {
        const reply = DBus.SessionBus.asyncCall({
            service: "org.kde.kclockd",
            path: "/Timers",
            iface: "org.kde.kclock.TimerModel",
            member: "timers",
            signature: "",
            arguments: []
        })
        reply.finished.connect(() => {
            if (!reply.isError) {
                const uuids = reply.value || []
                for (let i = 0; i < uuids.length; i++) {
                    root.watchTimer(uuids[i])
                }
            }
            reply.destroy()
        })
    }

    // kclockd signals newer timers with a formatted UUID like "{a8db-…}" but the
    // D-Bus object path is always /Timers/<hex>. Normalize so both match.
    function normalizeUuid(uuid) {
        return String(uuid).replace(/[{}]/g, "").replace(/-/g, "")
    }

    function watchTimer(uuid) {
        uuid = root.normalizeUuid(uuid)
        if (!uuid || root.timerWatchers[uuid] || !root.enableTimer) {
            return
        }
        const watcher = timerWatcherComponent.createObject(root, { timerUuid: uuid })
        root.timerWatchers[uuid] = watcher
        root.syncTimerState(uuid)
        // kclockd emits runningChanged/timeCompletedChanged at the same instant it
        // emits timerAdded — while this watcher is still being created, so those
        // signals can be lost and the first GetAll can catch running=false. Re-read
        // the settled state shortly after so a just-started timer never goes missing.
        timerSyncRetry.timerUuid = uuid
        timerSyncRetry.restart()
    }

    function unwatchTimer(uuid) {
        uuid = root.normalizeUuid(uuid)
        const watcher = root.timerWatchers[uuid]
        if (watcher) {
            watcher.enabled = false
            watcher.destroy()
            delete root.timerWatchers[uuid]
        }
        delete root.timerMap[uuid]
        root.recomputeTimers()
    }

    // One-shot read of the full timer state for a single uuid. Called only from
    // the timer's D-Bus signals (and once at startup to sync pre-existing timers).
    // Note: asyncCall needs a parenthesized signature "(s)" for scalar args —
    // a bare "s" makes the encoder treat the whole argument list as one scalar.
    function syncTimerState(uuid) {
        uuid = root.normalizeUuid(uuid)
        const reply = DBus.SessionBus.asyncCall({
            service: "org.kde.kclockd",
            path: "/Timers/" + uuid,
            iface: "org.freedesktop.DBus.Properties",
            member: "GetAll",
            signature: "(s)",
            arguments: ["org.kde.kclock.Timer"]
        })
        reply.finished.connect(() => {
            if (reply.isError) {
                reply.destroy()
                return
            }
            const props = reply.value || {}
            const rv = (k) => {
                const x = props[k]
                return (x && typeof x === "object" && "value" in x) ? x.value : x
            }
            const nowSec = Math.floor(Date.now() / 1000)
            const tEnd = rv("timeCompleted") || 0
            const lengthSec = rv("length") || 0
            const running = rv("running") === true
            const ringing = rv("ringing") === true
            // A paused timer reads running=false with timeCompleted = now + remaining.
            // A reset timer reads running=false with remaining == full length; a
            // finished one reads remaining ~0. Treat any stopped timer with a
            // positive remaining as a paused countdown (so a rewind-to-start that
            // stops stays visible instead of vanishing).
            let pausedMs = 0
            if (!running && !ringing) {
                const remainMs = (tEnd - nowSec) * 1000
                const lengthMs = lengthSec * 1000
                if (remainMs > 0) {
                    pausedMs = Math.min(remainMs, lengthMs)
                }
            }
            root.timerMap[uuid] = {
                running: running,
                ringing: ringing,
                label: String(rv("label") || ""),
                lengthMs: lengthSec * 1000,
                end: tEnd * 1000,
                pausedMs: pausedMs
            }
            reply.destroy()
            root.recomputeTimers()
        })
    }

    // Derive the aggregate island state from the tracked timers. Costs nothing
    // while idle and runs only when a D-Bus timer event actually arrived.
    function recomputeTimers() {
        const nowMs = Date.now()
        let ringingUuid = ""
        let ringingLabel = ""
        let hasCount = false
        let bestRem = -1
        let bestEnd = 0
        let bestFrozen = false
        let bestFrozenMs = 0
        let bestUuid = ""
        for (const uuid in root.timerMap) {
            const t = root.timerMap[uuid]
            if (t.ringing && ringingUuid.length === 0) {
                ringingUuid = uuid
                ringingLabel = t.label
                continue
            }
            let rem = -1
            let end = 0
            let frozen = false
            let frozenMs = 0
            if (t.running) {
                end = t.end
                rem = Math.max(0, end - nowMs)
            } else if (t.pausedMs > 0) {
                frozen = true
                frozenMs = t.pausedMs
                rem = frozenMs
            } else {
                continue
            }
            if (bestRem < 0 || rem < bestRem) {
                bestRem = rem
                bestEnd = end
                bestFrozen = frozen
                bestFrozenMs = frozenMs
                bestUuid = uuid
                hasCount = true
            }
        }
        root.timerRingingUuid = ringingUuid
        root.timerLabel = ringingLabel
        root.timerRinging = ringingUuid.length > 0
        root.timerHasCount = hasCount
        root.timerEndMs = bestEnd
        root.timerFrozen = bestFrozen
        root.timerFrozenMs = bestFrozenMs
        root.timerCountUuid = hasCount ? bestUuid : ""
    }

    // Toggle pause/resume or reset the active counting timer. No-arg D-Bus calls
    // work fine (they don't hit the argument-marshalling path), and kclockd emits
    // runningChanged afterwards so the island re-syncs itself.
    function timerControl(member) {
        const uuid = root.timerCountUuid
        if (!uuid) {
            return
        }
        DBus.SessionBus.asyncCall({
            service: "org.kde.kclockd",
            path: "/Timers/" + uuid,
            iface: "org.kde.kclock.Timer",
            member: member,
            signature: "",
            arguments: []
        })
    }

    // Rewind the active counting timer to its full length and leave it paused.
    // kclockd's reset() sets elapsed back to 0 and stops the timer; the island
    // then renders the rewinded timer as a (visible) paused countdown at full
    // length rather than a running one.
    function timerReset() {
        const uuid = root.timerCountUuid
        if (!uuid) {
            return
        }
        DBus.SessionBus.asyncCall({
            service: "org.kde.kclockd",
            path: "/Timers/" + uuid,
            iface: "org.kde.kclock.Timer",
            member: "reset",
            signature: "",
            arguments: []
        })
    }

    // Delete the active timer from kclockd entirely. removeTimer() matches the
    // uuid in its native {8-4-4-4-12} form, but the island stores uuids
    // dash/brace-stripped (the form used on D-Bus object paths), so reconstruct
    // the dashed form here. kclockd emits timerRemoved and the island drops it
    // via the watcher — no lingering entry. Works for counting or ringing timers.
    function timerDelete() {
        const uuid = root.timerCountUuid || root.timerRingingUuid
        if (!uuid) {
            return
        }
        const n = String(uuid)
        const dashed = n.replace(/^([0-9a-f]{8})([0-9a-f]{4})([0-9a-f]{4})([0-9a-f]{4})([0-9a-f]{12})$/i, "$1-$2-$3-$4-$5")
        const removeUuid = dashed.length === 36 ? "{" + dashed + "}" : n
        DBus.SessionBus.asyncCall({
            service: "org.kde.kclockd",
            path: "/Timers",
            iface: "org.kde.kclock.TimerModel",
            member: "removeTimer",
            signature: "(s)",
            arguments: [removeUuid]
        })
        // kclockd auto-exits once no timers remain, which can drop the
        // timerRemoved signal before the island sees it — drop our local
        // tracking immediately so the capsule hides without waiting.
        root.unwatchTimer(root.normalizeUuid(uuid))
    }

    // Clicking the capsule while a timer rings dismisses it in kclockd, which
    // also stops the alarm sound. The resulting ringing=false signal hides it.
    function dismissTimer() {
        if (!root.timerRingingUuid || !root.timerRinging) {
            return
        }
        const uuid = root.timerRingingUuid
        DBus.SessionBus.asyncCall({
            service: "org.kde.kclockd",
            path: "/Timers/" + uuid,
            iface: "org.kde.kclock.Timer",
            member: "dismiss",
            signature: "",
            arguments: []
        })
        root.timerRinging = false
        root.timerLabel = ""
        root.timerRingingUuid = ""
    }

    NotificationManager.Settings {
        id: notificationSettings
    }

    NotificationManager.Notifications {
        id: notificationsModel
        limit: 1
        showExpired: false
        showDismissed: false
        showNotifications: true
        showJobs: true
        sortMode: NotificationManager.Notifications.SortByDate
        sortOrder: Qt.DescendingOrder
        groupMode: NotificationManager.Notifications.GroupDisabled

        onUnreadNotificationsCountChanged: {
            root.unreadCount = unreadNotificationsCount
            if (unreadNotificationsCount > 0 && root.enableNotifications) {
                notificationPulse.restart()
            }
        }

        onActiveJobsCountChanged: {
            root.jobsCount = activeJobsCount
            if (activeJobsCount > 0 && root.enableDownloads) {
                root.modeIndex = 5
                eventTimer.restart()
            }
        }

        onJobsPercentageChanged: root.jobsPercent = jobsPercentage

        Component.onCompleted: {
            root.unreadCount = unreadNotificationsCount
            root.jobsCount = activeJobsCount
            root.jobsPercent = jobsPercentage
        }
    }

    Connections {
        target: NotificationManager.Server

        function onNotificationAdded(notification) {
            const app = notification.applicationName || ""
            const summary = notification.summary || ""
            const body = notification.body || notification.text || ""
            if (root.handleBuildNotification(app, summary, body)) {
                return
            }
            if (!root.enableNotifications) {
                return
            }
            root.notificationTitle = summary || app || "Notification"
            root.notificationBody = body
            root.notificationApp = app
            root.notificationIcon = notification.applicationIconName || notification.iconName || "notifications"
            if (root.notificationIcon.length === 0) {
                root.notificationIcon = "notifications"
            }
            root.notificationActionNames = notification.actionNames || []
            root.notificationActionLabels = notification.actionLabels || []
            root.notificationHasDefaultAction = notification.hasDefaultAction || false
            notificationPulse.restart()
        }
    }

    Repeater {
        model: notificationsModel

        Item {
            readonly property string summaryValue: model.summary || ""
            readonly property string bodyValue: model.body || ""
            readonly property string appValue: model.applicationName || ""
            readonly property string appIconValue: model.applicationIconName || ""
            readonly property string iconValue: model.iconName || ""
            readonly property var actionsValue: model.actionNames || []

            visible: false
            Component.onCompleted: root.setNotification(model)
            onSummaryValueChanged: root.setNotification(model)
            onBodyValueChanged: root.setNotification(model)
            onActionsValueChanged: root.setNotification(model)
        }
    }

    Repeater {
        id: mediaRepeater

        model: Mpris.MultiplexerModel {
        }

        Item {
            readonly property string trackValue: model.track || ""
            readonly property string artistValue: model.artist || ""
            readonly property string artValue: model.artUrl || ""
            readonly property int statusValue: model.playbackStatus
            readonly property real positionValue: model.position || 0
            readonly property real lengthValue: model.length || 0

            visible: false
            Component.onCompleted: {
                if (index === 0) {
                    root.setMedia(model)
                }
            }
            onTrackValueChanged: if (index === 0) root.setMedia(model)
            onArtistValueChanged: if (index === 0) root.setMedia(model)
            onArtValueChanged: if (index === 0) root.setMedia(model)
            onPositionValueChanged: if (index === 0) root.setMedia(model)
            onLengthValueChanged: if (index === 0) root.setMedia(model)
            onStatusValueChanged: {
                if (index === 0) {
                    root.setMedia(model)
                    if (model.playbackStatus === Mpris.PlaybackStatus.Playing && root.enableMedia) {
                        root.modeIndex = 0
                        eventTimer.restart()
                    }
                }
            }
        }
    }

    MouseArea {
        id: mouseArea

        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        onClicked: (mouse) => {
            if (mouse.button === Qt.MiddleButton) {
                if (root.activeMode === 2 && root.unreadCount > 0) {
                    root.dismissNotification()
                }
                return
            }
            if (root.timerRinging && root.activeMode === 11) {
                root.dismissTimer()
                return
            }
            root.togglePopup()
        }
        onWheel: (wheel) => root.handleWheel(wheel.angleDelta.y)
    }

    Rectangle {
        id: island

        anchors.centerIn: parent
        width: root.compactWidth
        height: root.compactHeight
        radius: height / 2
        color: "transparent"
        border.width: 0
        transformOrigin: Item.Center

        Loader {
            id: compactLoader

            anchors.fill: parent
            sourceComponent: compactContent
        }
    }

    PlasmaCore.Dialog {
        id: popup

        visualParent: root
        location: root.popupLocation
        visible: false
        hideOnWindowDeactivate: false
        backgroundHints: PlasmaCore.Dialog.NoBackground

        Component.onCompleted: flags = flags | Qt.WindowStaysOnTopHint

        // Visibility is driven imperatively (openPopup/closePopup). If the window
        // manager hides the dialog itself (e.g. losing focus while switching
        // monitors) reset the open state so it cannot linger on another screen.
        onVisibleChanged: {
            if (!visible) {
                popupCloseTimer.stop()
                root.popupOpen = false
            }
        }

        mainItem: Item {
            id: popupRootItem
            width: root.expandedWidth
            height: root.expandedHeight + root.popupGapTop + root.popupGapBottom
            opacity: root.popupOpen ? 1 : 0
            scale: root.popupOpen ? 1 : 0.92

            Item {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.topMargin: root.popupGapTop
                anchors.bottomMargin: root.popupGapBottom

                MouseArea {
                    id: popupMouseArea

                    z: 10
                    anchors.fill: parent
                    hoverEnabled: true
                    // Hover-track the whole popup to dismiss on leave, but let
                    // presses fall through to the content buttons below so that
                    // interacting with the expanded panel keeps it open instead
                    // of hiding on every click/hover.
                    onPressed: (mouse) => mouse.accepted = false
                    onEntered: {
                        popupCloseTimer.stop()
                        popup.visible = true
                        root.popupOpen = true
                    }
                    onExited: root.closePopup()
                }

                Rectangle {
                    anchors.fill: parent
                    radius: root.cornerRadius
                    color: root.panelBackground
                    border.width: root.borderEnabled && root.backgroundEnabled ? 1 : 0
                    border.color: root.borderColor

                    Loader {
                        id: expandedLoader

                        anchors.fill: parent
                        sourceComponent: expandedContent
                    }
                }
            }
        }
    }

    Component {
        id: compactContent

        RowLayout {
            id: compactLayout

            anchors.fill: parent
            anchors.leftMargin: root.compactSidePadding
            anchors.rightMargin: root.compactSidePadding
            spacing: root.moduleSeparators ? 6 : 8

            Repeater {
                model: root.compactVisibleList

                RowLayout {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.fillWidth: modelData === "content"
                    spacing: root.moduleSeparators ? 6 : 8

                    PlasmaComponents.Label {
                        visible: index > 0 && root.moduleSeparators
                        text: "/"
                        color: Qt.rgba(1, 1, 1, 0.4)
                        font.pointSize: 15
                        font.weight: Font.Light
                        Layout.alignment: Qt.AlignVCenter
                    }

                    Loader {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.fillWidth: modelData === "content"
                        sourceComponent: modelData === "time" ? compactTimeBlock : compactContentBlock
                    }
                }
            }
        }
    }

    Component {
        id: compactContentBlock

        RowLayout {
            spacing: 8

            Rectangle {
                id: sharingDot
                visible: root.sharingWithMedia
                Layout.preferredWidth: 10
                Layout.preferredHeight: 10
                radius: 5
                color: root.sharingDotColor
                transformOrigin: Item.Center
            }

            SoundBars {
                visible: root.activeMode === 0
                playing: root.mediaPlaying
                levels: audioLevels.levels
                reactive: audioLevels.ready
                Layout.preferredWidth: 30
                Layout.preferredHeight: 26
            }

            Item {
                visible: root.activeMode === 0
                Layout.preferredWidth: 6
                Layout.preferredHeight: 1
            }

            MediaCompactIcon {
                visible: root.activeMode === 0
                artUrl: root.mediaArtUrl
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
            }

            Rectangle {
                id: idleDot
                visible: root.showGreenDot
                Layout.preferredWidth: 10
                Layout.preferredHeight: 10
                radius: 5
                color: root.idleDotColor
                transformOrigin: Item.Center
            }

            StatusIcon {
                visible: root.activeMode !== 0 && !root.showGreenDot && root.activeMode !== 14
                mode: root.activeMode
                iconName: root.notificationIcon
                opacity: root.activeMode === 11 && root.timerRinging ? (root.timerBlinkOn ? 1 : 0.3) : 1
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
            }

            // Compact volume control: mute icon + horizontal slider, shown in
            // place of the StatusIcon when a volume change is announced (mode 14).
            Item {
                visible: root.activeMode === 14
                Layout.preferredWidth: 136
                Layout.preferredHeight: 24

                // Mute toggle icon
                MouseArea {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24
                    height: 24
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.volWatcher.toggleMute()

                    Kirigami.Icon {
                        anchors.centerIn: parent
                        width: parent.width - 4
                        height: parent.height - 4
                        source: root.volWatcher.muted ? "audio-volume-muted" : "audio-volume-high"
                        color: "white"
                    }
                }

                PlasmaComponents.Slider {
                    id: volSlider
                    anchors.left: parent.left
                    anchors.leftMargin: 28
                    anchors.right: parent.right
                    anchors.rightMargin: 2
                    anchors.verticalCenter: parent.verticalCenter
                    height: 16
                    from: 0
                    to: 100
                    stepSize: 1
                    value: root.volWatcher.volume
                    enabled: !root.volWatcher.muted
                    opacity: enabled ? 1 : 0.5
                    onMoved: root.volWatcher.setVolume(Math.round(value))
                }
            }

            PlasmaComponents.Label {
                text: root.compactTitle
                visible: !root.showGreenDot && text.length > 0
                color: root.textPrimary
                opacity: root.activeMode === 11 && root.timerRinging ? (root.timerBlinkOn ? 1 : 0.3) : 1
                font.pointSize: 12
                font.weight: Font.Medium
                elide: Text.ElideRight
                Layout.preferredWidth: root.compactTextWidth
                Layout.minimumWidth: 0
                Layout.maximumWidth: root.compactTextWidth
            }

            Rectangle {
                id: unreadBadge
                visible: root.activeMode === 2 && root.unreadCount > 0
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                radius: 6
                color: root.accent
                transformOrigin: Item.Center

                PlasmaComponents.Label {
                    anchors.centerIn: parent
                    text: root.unreadCount
                    color: "white"
                    font.bold: true
                }
            }
        }
    }

    Component {
        id: compactTimeBlock

        PlasmaComponents.Label {
            text: root.timeText
            color: root.textPrimary
            font.pointSize: 16
            font.weight: Font.Medium
        }
    }

    Component {
        id: expandedContent

        Item {
            anchors.fill: parent

            Loader {
                anchors.fill: parent
                sourceComponent: {
                    if (root.activeMode === 0) return musicExpanded
                    if (root.activeMode === 2) return notificationExpanded
                    return statusExpanded
                }
            }
        }
    }

    Component {
        id: musicExpanded

        Item {
            anchors.fill: parent

            Image {
                id: musicArt
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                width: 52
                height: 52
                visible: source !== ""
                source: root.mediaArtUrl
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                layer.enabled: true
            }

            Rectangle {
                anchors.fill: musicArt
                visible: root.mediaArtUrl === ""
                radius: 8
                color: Qt.rgba(0.92, 0.94, 0.96, 0.28)

                Kirigami.Icon {
                    anchors.centerIn: parent
                    source: "audio-x-generic"
                    width: 30
                    height: 30
                }
            }

            SoundBars {
                id: musicBars
                anchors.left: musicArt.right
                anchors.leftMargin: 12
                anchors.top: parent.top
                anchors.topMargin: 16
                width: 38
                height: 28
                playing: root.mediaPlaying
                levels: audioLevels.levels
                reactive: audioLevels.ready
            }

            Row {
                id: musicControls
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 14
                spacing: 14

                Kirigami.Icon {
                    source: "media-skip-backward"
                    width: 24
                    height: 24
                    anchors.verticalCenter: parent.verticalCenter
                    opacity: root.mediaContainer ? 1 : 0.35

                    MouseArea {
                        anchors.fill: parent
                        onClicked: if (root.mediaContainer) root.mediaContainer.Previous()
                    }
                }

                Kirigami.Icon {
                    source: root.mediaPlaying ? "media-playback-pause" : "media-playback-start"
                    width: 26
                    height: 26
                    anchors.verticalCenter: parent.verticalCenter
                    opacity: root.mediaContainer ? 1 : 0.35

                    MouseArea {
                        anchors.fill: parent
                        onClicked: if (root.mediaContainer) root.mediaContainer.PlayPause()
                    }
                }

                Kirigami.Icon {
                    source: "media-skip-forward"
                    width: 24
                    height: 24
                    anchors.verticalCenter: parent.verticalCenter
                    opacity: root.mediaContainer ? 1 : 0.35

                    MouseArea {
                        anchors.fill: parent
                        onClicked: if (root.mediaContainer) root.mediaContainer.Next()
                    }
                }
            }

            PlasmaComponents.Label {
                id: musicTitle
                anchors.left: musicBars.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.top: parent.top
                anchors.topMargin: 16
                text: root.mediaTitle || "No title"
                color: root.textPrimary
                font.pointSize: 14
                font.weight: Font.Medium
                elide: Text.ElideRight
            }

            PlasmaComponents.Label {
                anchors.left: musicBars.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.top: musicTitle.bottom
                anchors.topMargin: 2
                text: root.mediaArtist || root.mediaIdentity || "Media player"
                color: root.textSecondary
                font.pointSize: 10
                elide: Text.ElideRight
            }

            Rectangle {
                anchors.left: musicArt.right
                anchors.leftMargin: 14
                anchors.right: musicControls.left
                anchors.rightMargin: 16
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 22
                height: 5
                radius: 3
                color: Qt.rgba(1, 1, 1, 0.22)

                Rectangle {
                    width: parent.width * root.mediaProgress
                    height: parent.height
                    radius: parent.radius
                    color: root.accent
                }
            }
        }
    }

    Component {
        id: notificationExpanded

        Item {
            id: notifRoot
            anchors.fill: parent

            readonly property bool hasActions: root.notificationActionLabels.length > 0

            StatusIcon {
                id: notifIcon
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                width: 44
                height: 44
                mode: 2
                iconName: root.notificationIcon
            }

            SoundBars {
                id: notifBars
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.top: parent.top
                anchors.topMargin: 14
                width: 32
                height: 24
                playing: notificationPulse.running
            }

            Column {
                anchors.left: notifIcon.right
                anchors.leftMargin: 14
                anchors.right: notifBars.left
                anchors.rightMargin: 14
                anchors.top: notifRoot.hasActions ? parent.top : undefined
                anchors.topMargin: notifRoot.hasActions ? 12 : 0
                anchors.verticalCenter: notifRoot.hasActions ? undefined : parent.verticalCenter
                spacing: 3

                PlasmaComponents.Label {
                    width: parent.width
                    text: root.notificationTitle || "Notification"
                    color: root.textPrimary
                    font.pointSize: 13
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }

                PlasmaComponents.Label {
                    width: parent.width
                    text: root.notificationBody || root.notificationApp || (root.unreadCount > 0 ? "%1 unread".arg(root.unreadCount) : "New notification")
                    color: root.textSecondary
                    font.pointSize: 10
                    wrapMode: Text.WordWrap
                    maximumLineCount: notifRoot.hasActions ? 1 : Plasmoid.configuration.notificationBodyLines
                    elide: Text.ElideRight
                }
            }

            // Click the icon/text region to trigger the default action (open the app).
            MouseArea {
                anchors.left: parent.left
                anchors.right: notifBars.left
                anchors.top: parent.top
                anchors.bottom: notifRoot.hasActions ? notifActions.top : parent.bottom
                enabled: root.notificationHasDefaultAction
                onClicked: root.activateNotification()
            }

            Row {
                id: notifActions
                visible: notifRoot.hasActions
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 12
                spacing: 8

                Repeater {
                    model: Math.min(2, root.notificationActionLabels.length)

                    ActionPill {
                        label: root.notificationActionLabels[index] || ""
                        onTriggered: root.invokeNotificationAction(index)
                    }
                }
            }
        }
    }

    Component {
        id: statusExpanded

        Item {
            id: statusRoot
            anchors.fill: parent

            readonly property bool hasProgress: root.activeMode === 5 || root.activeMode === 8
            readonly property bool hasControls: root.activeMode === 11 && (root.timerHasCount || root.timerRinging)

            StatusIcon {
                id: statusIcon
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                width: 44
                height: 44
                mode: root.activeMode
                iconName: root.notificationIcon
                opacity: root.activeMode === 11 && root.timerRinging ? (root.timerBlinkOn ? 1 : 0.3) : 1
            }

            Column {
                anchors.left: statusIcon.right
                anchors.leftMargin: 14
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.top: statusRoot.hasControls ? parent.top : undefined
                anchors.topMargin: statusRoot.hasControls ? 14 : 0
                anchors.verticalCenter: statusRoot.hasControls ? undefined : parent.verticalCenter
                spacing: 5

                PlasmaComponents.Label {
                    width: parent.width
                    text: root.activeMode === 3 ? (root.keyboardLongName || root.keyboardShortName || "Status") : root.compactTitle
                    color: root.textPrimary
                    opacity: root.activeMode === 11 && root.timerRinging ? (root.timerBlinkOn ? 1 : 0.3) : 1
                    font.pointSize: 13
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }

                PlasmaComponents.Label {
                    width: parent.width
                    text: root.activeMode === 3 ? (msi.available ? "CPU " + msi.cpuTemp + "°C · GPU " + msi.gpuTemp + "°C" : "Status") :
                          root.activeMode === 5 ? (root.jobsCount > 0 ? "%1% complete".arg(root.jobsPercent) : "No active downloads") :
                          root.activeMode === 7 ? "Active capture or presentation mode" :
                          root.activeMode === 8 ? (root.buildApp + (root.buildSuccess ? " · " + "success" : " · " + "failed")) :
                          root.activeMode === 9 ? (root.cameraBlocked ? "Camera blocked by hardware switch" : "Camera") :
                          root.activeMode === 10 ? (root.micMuted ? "Microphone muted" : "Microphone active") :
                          root.activeMode === 11 ? (root.timerRinging ? "Timer complete" : "Timer running") :
                          root.activeMode === 12 ? "Battery" :
                          root.activeMode === 14 ? (root.volWatcher.muted ? "Muted" : "Volume") :
                          root.activeMode === 15 ? "MSI EC" : "Status"
                    color: root.textSecondary
                    font.pointSize: 10
                    elide: Text.ElideRight
                }

                // MSI controls are available in the idle popup. They are plain
                // one-shot writes (sysfs or pkexec); no background service.
                Row {
                    visible: root.activeMode === 3 && msi.available && msi.canControl
                    spacing: 6

                    ActionPill {
                        label: "Boost"
                        active: msi.coolerBoost
                        onTriggered: root.toggleMsiCoolerBoost()
                    }
                    ActionPill {
                        label: root.msiFanLabel()
                        onTriggered: root.cycleMsiFan()
                    }
                    ActionPill {
                        label: root.msiShiftLabel()
                        onTriggered: root.cycleMsiShift()
                    }
                }

                Rectangle {
                    visible: statusRoot.hasProgress
                    width: parent.width
                    height: 5
                    radius: 3
                    color: Qt.rgba(1, 1, 1, 0.18)

                    Rectangle {
                        width: parent.width * (root.activeMode === 8 ? (root.buildSuccess ? 1 : 0.4) :
                                               root.jobsPercent > 0 ? root.jobsPercent / 100 : 0.05)
                        height: parent.height
                        radius: parent.radius
                        color: root.activeMode === 8 ? (root.buildSuccess ? "#55e36a" : "#ff4f6f") : root.accent
                    }
                }
            }

            // Timer controls shown while a KClock timer is running/paused/ringing.
            Row {
                visible: statusRoot.hasControls
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 12
                spacing: 8

                IconPill {
                    visible: root.activeMode === 11 && root.timerHasCount
                    iconName: root.timerFrozen ? "chronometer-start" : "chronometer-pause"
                    onTriggered: root.timerControl("toggleRunning")
                }
                IconPill {
                    visible: root.activeMode === 11 && root.timerHasCount
                    iconName: "chronometer-reset"
                    onTriggered: root.timerReset()
                }
                IconPill {
                    visible: root.activeMode === 11 && root.timerRinging
                    iconName: "media-playback-stop"
                    onTriggered: root.dismissTimer()
                }
                IconPill {
                    visible: root.activeMode === 11 && (root.timerHasCount || root.timerRinging)
                    iconName: "dialog-close"
                    onTriggered: root.timerDelete()
                }
            }
        }
    }

    component StatusIcon: Item {
        property int mode: 0
        property string iconName: "notifications"

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: mode === 2 ? "#2da6e8" :
                   mode === 3 ? Qt.rgba(0.5, 0.56, 0.68, 0.38) :
                   mode === 5 ? Qt.rgba(0.12, 0.6, 0.95, 0.34) :
                   mode === 7 ? Qt.rgba(0.25, 1, 0.15, 0.28) :
                   mode === 8 ? (root.buildSuccess ? Qt.rgba(0.33, 0.89, 0.41, 0.30) : Qt.rgba(0.92, 0.30, 0.36, 0.32)) :
                   mode === 9 ? (root.cameraAvailable ? Qt.rgba(0.2, 0.9, 0.5, 0.30) : Qt.rgba(0.92, 0.30, 0.36, 0.32)) :
                   mode === 10 ? (root.micMuted ? Qt.rgba(0.92, 0.30, 0.36, 0.32) : Qt.rgba(0.2, 0.9, 0.5, 0.30)) :
                   mode === 11 ? Qt.rgba(1, 0.62, 0.15, 0.34) :
                   mode === 12 ? (root.battery.percent <= 10 ? Qt.rgba(0.92, 0.30, 0.36, 0.32) :
                                  root.battery.percent <= 20 ? Qt.rgba(1, 0.62, 0.15, 0.34) :
                                  Qt.rgba(0.2, 0.9, 0.5, 0.30)) :
                   mode === 13 ? Qt.rgba(0.5, 0.7, 1, 0.35) :
                   mode === 14 ? Qt.rgba(0.4, 0.8, 1, 0.35) :
                   mode === 15 ? Qt.rgba(1, 0.45, 0.2, 0.34) :
                   Qt.rgba(0.55, 0.4, 1, 0.3)

            Kirigami.Icon {
                anchors.centerIn: parent
                width: parent.width * 0.62
                height: width
                source: mode === 2 ? iconName :
                        mode === 3 ? "input-keyboard" :
                        mode === 5 ? "download" :
                        mode === 7 ? "krfb" :
                        mode === 8 ? (root.buildSuccess ? "emblem-success" : "emblem-error") :
                        mode === 9 ? "camera-web" :
                        mode === 10 ? (root.micMuted ? "microphone-sensitivity-muted" : "microphone-sensitivity-high") :
                        mode === 11 ? "chronometer" :
                        mode === 12 ? root.batteryIconName() :
                        mode === 13 ? root.lockIcon :
                        mode === 14 ? (root.volWatcher.muted ? "audio-volume-muted" : "audio-volume-high") :
                        mode === 15 ? root.msiIcon :
                        "utilities-terminal"
            }
        }
    }

    component ActionPill: Rectangle {
        property string label: ""
        property bool active: false
        signal triggered()

        height: 26
        width: pillLabel.implicitWidth + 24
        radius: height / 2
        color: pillMouse.pressed ? Qt.lighter(root.accent, 1.15)
                                 : active ? Qt.rgba(0.2, 0.9, 0.5, 0.75) : root.accent

        PlasmaComponents.Label {
            id: pillLabel
            anchors.centerIn: parent
            text: parent.label
            color: "white"
            font.pointSize: 10
            font.weight: Font.Medium
            elide: Text.ElideRight
        }

        MouseArea {
            id: pillMouse
            anchors.fill: parent
            onClicked: parent.triggered()
        }
    }

    component IconPill: Rectangle {
        property string iconName: ""
        signal triggered()

        height: 28
        width: 28
        radius: height / 2
        color: iconMouse.pressed ? Qt.lighter(root.accent, 1.15) : root.accent

        Kirigami.Icon {
            id: iconGlyph
            anchors.centerIn: parent
            width: 15
            height: 15
            source: parent.iconName
            color: "white"
        }

        MouseArea {
            id: iconMouse
            anchors.fill: parent
            onClicked: parent.triggered()
        }
    }

    component MediaCompactIcon: Item {
        property string artUrl: ""

        Rectangle {
            anchors.fill: parent
            radius: 6
            color: Qt.rgba(1, 1, 1, 0.12)
            visible: artUrl.length === 0

            Kirigami.Icon {
                anchors.centerIn: parent
                width: parent.width * 0.7
                height: width
                source: "audio-x-generic"
            }
        }

        Image {
            anchors.fill: parent
            visible: artUrl.length > 0
            source: artUrl
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
        }
    }

    component SoundBars: Row {
        id: soundBarsRoot

        property bool playing: false
        property bool reactive: false
        property var levels: []

        spacing: 3
        width: 27
        height: 26

        Repeater {
            model: [14, 22, 10, 18, 16]

            Rectangle {
                id: bar
                width: 3
                height: soundBarsRoot.reactive && soundBarsRoot.levels.length > index
                    ? 4 + soundBarsRoot.levels[index] * 22
                    : modelData * fakeLevel
                anchors.verticalCenter: parent.verticalCenter
                radius: 1.5
                color: root.textPrimary
                opacity: soundBarsRoot.playing ? 0.9 : 0.55

                property real fakeLevel: 0.45

                Behavior on height {
                    enabled: soundBarsRoot.reactive
                    NumberAnimation { duration: 70; easing.type: Easing.OutQuad }
                }

                // Fallback when no monitor stream is ready: a lightweight
                // animation-engine pulse. Once real levels arrive, the height
                // is driven by the audio callback instead.
                SequentialAnimation {
                    running: soundBarsRoot.playing && !soundBarsRoot.reactive
                    loops: Animation.Infinite
                    NumberAnimation {
                        target: bar
                        property: "fakeLevel"
                        from: 0.35
                        to: 1
                        duration: 240 + (index % 5) * 100
                        easing.type: Easing.InOutSine
                    }
                    NumberAnimation {
                        target: bar
                        property: "fakeLevel"
                        from: 1
                        to: 0.35
                        duration: 240 + (index % 5) * 100
                        easing.type: Easing.InOutSine
                    }
                }
            }
        }
    }
}
