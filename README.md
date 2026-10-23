# Dynamic Island (installed fork)

This repository tracks the installed Plasma 6 applet at
`~/.local/share/plasma/plasmoids/com.ifny75.dynamicisland`.

The applet is the original `ifny75/PlasmaKde-Dynamic-Islands` fork with
hardware-specific integrations for an MSI Pulse GL66 11UGKV (MS-1581).

## What it does

- Clock, media, notifications, keyboard layout, downloads, screen sharing and
  IDE-build status in one compact capsule.
- Battery state from UPower D-Bus signals: percentage, charging/plugged state,
  remaining time and low/critical announcements.
- Volume and microphone mute changes from PulseAudio/PipeWire, including a
  compact volume slider that is shown only while the change is announced.
- Camera presence from udev (`video4linux` capture devices only), with a
  hardware-blocked state from MSI EC.
- MSI EC controls: Cooler Boost, fan mode, shift mode, Super Battery and the
  webcam kill switch. Values are read on demand and refreshed on popup open,
  after an action, or when an MSI WMI Fn hotkey is pressed.

Everything is event driven. There is no polling daemon, no persistent helper
process and no `pactl` subprocess; PulseAudio/udev/D-Bus/input events drive the
state changes directly.

## Layout

- `contents/` — the installed Plasma package (QML, config schema, metadata).
- `plugins/` — QML C++ extension sources used by the applet:
  - `micstate` — PulseAudio default-source mute watcher (event driven).
  - `volumewatcher` — PulseAudio default-sink volume/mute watcher (event driven).
  - `camera` — udev video4linux capture-device watcher (event driven).
  - `keystate` — KWin Wayland lock-key watcher (event driven).
  - `msiec` — MSI EC fan/performance/webcam controls (sysfs, event driven).
- `build-plugins.sh` — builds and installs the QML modules into
  `~/.local/share/qml/`. Restart `plasmashell` after installing.
- `scripts/` — optional one-time MSI EC permission setup.

## Build and install

```sh
./build-plugins.sh
systemctl --user restart plasma-plasmashell.service
```

The applet QML itself is read from this directory; no extra install step is
needed for `contents/`.

## MSI EC write permissions

By default the MSI EC sysfs attributes are `root:root 0644`, so the applet can
read them but cannot write them. The code has two write paths:

1. Direct sysfs write when the attribute is writable by the user.
2. A single `pkexec /usr/bin/tee <attribute>` call per action when it is not.
   This prompts through polkit and does not leave a process behind.

For passwordless writes, install the optional udev rule once:

```sh
sudo ./scripts/install-msi-ec-permissions.sh
```

It makes the control attributes group-writable for `wheel` (override with
`MSI_EC_GROUP=...`) and triggers the rule immediately. Log out and back in if
your session does not already include that group. This is a udev rule, not a
daemon; it only runs when the `msi-ec` platform device appears or changes.

## History

`git log` and `git tag` are the version history. The first commit is the
unmodified installed state. Later commits contain the audit fixes.
