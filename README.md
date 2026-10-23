# Dynamic Island (installed fork)

This repository tracks the installed Plasma 6 applet at
`~/.local/share/plasma/plasmoids/com.ifny75.dynamicisland`.

The applet is the original `ifny75/PlasmaKde-Dynamic-Islands` fork with
hardware-specific integrations for an MSI Pulse GL66 11UGKV (MS-1581).

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

## History

`git log` and `git tag` are the version history. The first commit is the
unmodified installed state. Later commits contain the audit fixes.
