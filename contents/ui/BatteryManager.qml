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
    property bool charging: false      // UPower State == Charging
    property bool pluggedIn: false     // AC adapter online
    property bool hasBattery: false
    property int chargeState: 0        // UPower State: 1 charging, 2 discharging, 4 full, 5 pending
    property int remainingMsec: 0      // time to empty/full, whichever applies
    property int lowThreshold: 20      // <= this % while discharging -> 3s capsule
    property int warnThreshold: 15     // <= this % while discharging -> 6s capsule
    property int criticalThreshold: 10 // <= this % while discharging -> persistent capsule until AC plugged
    property bool critical: false

    // --- public signals (UI reacts to these) ---
    signal acStateChanged(bool pluggedIn)
    signal lowBattery(int percent)
    signal warnBattery(int percent)

    // --- private ---
    property string batteryPath: ""
    property string acPath: ""
    property bool acSynced: false
    property bool lowArmed: false
    property bool warnArmed: false
    property var watchers: ({})        // device path -> DBus.Properties

    // DBus.Properties returns gadget-wrapped values on GetAll/refreshed, but
    // plain values on PropertiesChanged. Normalise both shapes here; without
    // this the battery froze at the first value after boot.
    function prop(props, key, fallback) {
        if (!props) {
            return fallback
        }
        const x = props[key]
        if (x === undefined || x === null) {
            return fallback
        }
        if (typeof x === "object" && "value" in x) {
            return x.value
        }
        return x
    }

    function numberOf(props, key, fallback) {
        const n = Number(root.prop(props, key, fallback))
        return Number.isFinite(n) ? n : fallback
    }

    function boolOf(props, key, fallback) {
        const x = root.prop(props, key, fallback)
        if (typeof x === "string") {
            return x === "true" || x === "1" || x === "yes"
        }
        return Boolean(x)
    }

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
        // Create first, then set devicePath. Setting it as an initial property
        // races with DBus.Properties' own setup and logs "Could not set initial
        // property devicePath" in plasmashell.
        const props = devicePropsComponent.createObject(root)
        if (!props) {
            return
        }
        root.watchers[path] = props
        props.devicePath = path
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
            root.percent = -1
            root.charging = false
            root.chargeState = 0
            root.remainingMsec = 0
            root.critical = false
        }
        if (path === root.acPath) {
            root.acPath = ""
            root.pluggedIn = false
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
            if (!reply.isError) {
                const paths = reply.value || []
                for (let i = 0; i < paths.length; i++) {
                    root.watchDevice(String(paths[i]))
                }
            }
            reply.destroy()
        })
    }

    function applyDevice(path, props) {
        const type = root.numberOf(props, "Type", 0)
        if (type === 1) {
            root.applyAc(path, root.boolOf(props, "Online", false))
        } else if (type === 2) {
            root.applyBattery(path, props)
        }
    }

    function applyAc(path, online) {
        if (root.acPath === "") {
            root.acPath = path
        }
        const changed = root.pluggedIn !== online
        root.pluggedIn = online
        if (online) {
            root.critical = false
        } else {
            root.charging = false
        }
        if (root.acSynced && changed) {
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
        root.hasBattery = root.boolOf(props, "IsPresent", root.hasBattery)

        if (props && props["State"] !== undefined && props["State"] !== null) {
            root.chargeState = root.numberOf(props, "State", root.chargeState)
        }

        if (props && props["Percentage"] !== undefined && props["Percentage"] !== null) {
            const pct = Math.round(root.numberOf(props, "Percentage", root.percent))
            // Ignore a transient 0% while we still have a good reading.
            if (!(pct === 0 && root.percent > 0)) {
                root.percent = Math.max(0, Math.min(100, pct))
            }
        }

        root.charging = root.chargeState === 1

        const timeToEmpty = root.numberOf(props, "TimeToEmpty", 0)
        const timeToFull = root.numberOf(props, "TimeToFull", 0)
        const seconds = root.charging || root.chargeState === 5 ? timeToFull : timeToEmpty
        root.remainingMsec = Math.max(0, Math.round(seconds * 1000))

        const discharging = !root.pluggedIn || root.chargeState === 2 || root.chargeState === 6
        const pct = root.percent

        // Low-battery guard: announce once per crossing while discharging.
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

        // Critical guard: persistent until the AC adapter is plugged in again.
        root.critical = discharging && pct <= root.criticalThreshold && pct > 0
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
