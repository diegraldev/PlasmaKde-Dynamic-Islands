import QtQuick
import org.kde.plasma.configuration

ConfigModel {
    ConfigCategory {
        name: "Size & Shape"
        icon: "transform-scale"
        source: "configSize.qml"
    }
    ConfigCategory {
        name: "Layout"
        icon: "view-split-left-right"
        source: "configLayout.qml"
    }
    ConfigCategory {
        name: "Appearance"
        icon: "preferences-desktop-color"
        source: "configAppearance.qml"
    }
    ConfigCategory {
        name: "Clock"
        icon: "preferences-system-time"
        source: "configClock.qml"
    }
    ConfigCategory {
        name: "Notifications"
        icon: "preferences-desktop-notification-bell"
        source: "configNotifications.qml"
    }
    ConfigCategory {
        name: "Modules"
        icon: "view-visible"
        source: "configFeatures.qml"
    }
}