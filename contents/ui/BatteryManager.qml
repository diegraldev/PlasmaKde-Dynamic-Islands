import QtQuick
import org.kde.plasma.workspace.dbus as DBus

// Event-driven battery state from UPower over D-Bus (no polling).
//
// One DBus.Properties watcher per UPower device (battery + AC adapter). That
// type fetches all properties once and refreshes automatically whenever UPower
// emits PropertiesChanged for the device, so percent/online/state stay live
// purely from signals. A SignalWatcher on the UPower object handles devices
// appearing/disappearing.
QtObject {
    id: root

    // --- public state (read-only for consumers) ---
    property int percent: -1
    property bool charging: false
    property bool hasBattery: false
    property int chargeState: 0        // UPower State: 1 charging, 2 discharging, 4 full
    property int lowThreshold: 20      // <= this % while discharging -> 3s capsule
    property int warnThreshold: 15     // <= this % while discharging -> 6s capsule
    property int criticalThreshold: 10 // <= this % while discharging -> persistent capsule until AC plugged
    property bool critical: false

    // --- public signals (UI reacts to these) ---
    signal acStateChanged(bool charging)
    signal lowBattery(int percent)
    signal warnBattery(int percent)

    // --- private ---
    property string batteryPath: ""
    property string acPath: ""
    property bool acSynced: false
    property bool lowArmed: false
    property bool warnArmed: false
    property var watchers: ({})        // device path -> DBus.Properties

    // Attached as a property (QtObject has no default property for children).
    property DBus.SignalWatcher upowerWatcher: DBus.SignalWatcher {
        service: "org.freedesktop.UPower"
        path: "/org/freedesktop/UPower"
        iface: "org.freedesktop.UPower"
        busType: DBus.BusType.System
        function dbusDeviceAdded(path) {
            root.watchDevice(path)
        }
        function dbusDeviceRemoved(path) {
            root.unwatchDevice(path)
        }
        function dbusDeviceChanged(path) {
            // DBus.Properties refreshes itself on PropertiesChanged; nothing to do.
        }
    }

    // One DBus.Properties per device; it re-reads on every property change.
    property Component devicePropsComponent: Component {
        DBus.Properties {
            property string devicePath: ""
            busType: DBus.BusType.System
            service: "org.freedesktop.UPower"
            path: devicePath
            iface: "org.freedesktop.UPower.Device"
            onRefreshed: root.applyDevice(devicePath, properties)
            onPropertiesChanged: root.applyDevice(devicePath, properties)
        }
    }

    function watchDevice(path) {
        if (path === "" || root.watchers[path]) {
            return
        }
        const props = devicePropsComponent.createObject(root, { devicePath: path })
        root.watchers[path] = props
    }

    function unwatchDevice(path) {
        const props = root.watchers[path]
        if (!props) {
            return
        }
        props.destroy()
        delete root.watchers[path]
        if (path === root.batteryPath) {
            root.batteryPath = ""
            root.hasBattery = false
        }
        if (path === root.acPath) {
            root.acPath = ""
        }
    }

    function enumerate() {
        const reply = DBus.SystemBus.asyncCall({
            service: "org.freedesktop.UPower",
            path: "/org/freedesktop/UPower",
            iface: "org.freedesktop.UPower",
            member: "EnumerateDevices",
            signature: "",
            arguments: []
        })
        reply.finished.connect(() => {
            if (reply.isError) {
                return
            }
            const paths = reply.value || []
            for (let i = 0; i < paths.length; i++) {
                root.watchDevice(String(paths[i]))
            }
        })
    }

    function applyDevice(path, props) {
        const type = Number((props["Type"] || {}).value || 0)
        if (type === 1) {
            if (props["Online"] !== undefined) {
                root.applyAc(path, Boolean(props["Online"]))
            }
        } else if (type === 2) {
            root.applyBattery(path, props)
        }
    }

    function applyAc(path, online) {
        if (root.acPath === "") {
            root.acPath = path
        }
        const prev = root.charging
        root.charging = online
        if (root.acSynced && online !== prev) {
            // Re-read authoritative values shortly after a plug/unplug: UPower
            // can emit a transient 0% (or a partial map without Percentage)
            // mid-transition, and the resync replaces any such value.
            root.resyncTimer.restart()
            root.acStateChanged(online)
        }
        root.acSynced = true
    }

    function applyBattery(path, props) {
        if (root.batteryPath === "") {
            root.batteryPath = path
        }
        root.hasBattery = Boolean(props["IsPresent"])

        // Only accept fields actually present in this update. Partial
        // PropertiesChanged maps and transient 0% reads during AC transitions
        // must not clobber a good reading; applyAc's resync re-reads them.
        if (props["State"] !== undefined && props["State"] !== null) {
            root.chargeState = Number((props["State"] || {}).value || 0)
        }
        if (props["Percentage"] !== undefined && props["Percentage"] !== null) {
            const pct = Math.round(Number((props["Percentage"] || {}).value || 0))
            if (!(pct === 0 && root.percent > 0)) {
                root.percent = pct
            }
        }

        // Low-battery guard: announce once per crossing while discharging.
        const discharging = root.chargeState === 2
        const pct = root.percent
        const lowNow = discharging && pct <= root.lowThreshold && pct > 0
        if (lowNow && root.lowArmed) {
            root.lowArmed = false
            root.lowBattery(pct)
        } else if (!lowNow) {
            root.lowArmed = true
        }

        // Warn guard: fires once when crossing <= warnThreshold, longer capsule.
        const warnNow = discharging && pct <= root.warnThreshold && pct > 0
        if (warnNow && root.warnArmed) {
            root.warnArmed = false
            root.warnBattery(pct)
        } else if (!warnNow) {
            root.warnArmed = true
        }

        // Critical guard: persistent until the AC adapter is plugged in again
        // (charging state clears it via the same applyBattery path).
        const criticalNow = discharging && pct <= root.criticalThreshold && pct > 0
        root.critical = criticalNow
    }

    property Timer resyncTimer: Timer {
        interval: 400
        running: false
        repeat: false
        onTriggered: {
            for (const path in root.watchers) {
                const w = root.watchers[path]
                if (w) {
                    w.updateAll()
                }
            }
        }
    }

    Component.onCompleted: root.enumerate()
}
