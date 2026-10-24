# Dynamic Island for KDE Plasma 6

A polished, event-driven **Dynamic Island** for the Plasma panel: a compact
capsule that turns into a media player, a system dashboard, and a hardware
control surface without getting in the way.

This is a personal fork of
[`ifny75/PlasmaKde-Dynamic-Islands`](https://github.com/ifny75/PlasmaKde-Dynamic-Islands),
extended and tuned for an **MSI Pulse GL66 11UGKV (MS-1581)** running Fedora
and Plasma 6.

<p align="center">
  <img src="docs/demo-collage.png" alt="Dynamic Island demo: capsules and popouts" width="720">
</p>

## Why this fork

The original applet is a great starting point. This fork adds the pieces I use
every day:

- **Real audio-reactive bars** in the compact capsule, driven by the active
  player's PulseAudio/PipeWire stream.
- **Volume-independent spectrum bars** with five musical bands, so turning the
  player up or down does not make every bar jump to the top.
- **A compact system dashboard** with keyboard layout, CPU/GPU usage and
  temperatures, RAM usage and battery state.
- **Media-aware click zones**: while music is playing, the left side opens the
  media panel and the right side opens the status panel.
- **MSI EC awareness** for Cooler Boost and the webcam kill switch: the island
  announces state changes made from anywhere (Fn keys, MControlCenter,
  scripts), driven by inotify on the EC sysfs attributes plus the MSI WMI
  hotkey device — zero polling.
- **Touchpad capsule**: the Fn key is remapped from the firmware's dead
  `Ctrl+Meta+F24` shortcut to a real toggle (self-healing keymap + KWin input
  DBus), so the island announces the actual state with a check/cross badge.
- **Right-pinned popup**: one parent window whose width never depends on the
  active mode; the visible panel hangs from a fixed right pivot and only grows
  leftward, so music, notification and status popouts share the same right edge.

## Features

| Area | What it does |
| --- | --- |
| Clock | Time, date and a compact calendar-friendly capsule. |
| Media | MPRIS cover, title, progress and transport controls. |
| Audio bars | Five real spectrum bands with five selectable skins (Classic, Mirror, Dots, Wave, Neon), shown only in the compact capsule. |
| Notifications | Unread count and a compact notification body. |
| Keyboard | Current layout name from KWin. |
| Battery | UPower percentage, charging state, remaining time and low/critical announcements. |
| Volume | PulseAudio/PipeWire volume and mute changes, with a compact slider while the change is announced. |
| Microphone | Default-source mute state from PulseAudio/PipeWire. |
| Camera | udev video4linux presence plus the MSI hardware-blocked state. |
| Touchpad | Fn-key toggle with the real KWin state (input DBus + scancode remap), announced with a black panel glyph and a green check / red cross badge. |
| Screen sharing | Active capture/presentation indicator. |
| Downloads | Progress and completion state for active jobs. |
| IDE builds | Build app, success/failure and progress. |
| Timers | KClock integration with a ringing state and controls. |
| System status | CPU, GPU, RAM and battery tiles, sampled only while the panel is open. |
| MSI EC | CPU/GPU temperatures in the status panel plus event-driven Cooler Boost and webcam announcements. |

## Media and audio bars

The bars are not a fake animation. The `AudioLevels` QML plugin opens a
PulseAudio stream while media is playing and closes it on pause.

- It tries to match the active MPRIS player to its own PulseAudio sink input, so
  other applications normally do not drive the bars.
- If no sink input can be matched, it falls back to the default sink monitor.
- Five log-spaced bands cover bass through treble:
  `40–160`, `180–550`, `600–1700`, `1800–5600` and `6500–15000 Hz`.
- Each band is normalized against the broadband level. That makes the heights
  independent of the player volume.
- A fixed spectral calibration and a deliberately wide **38 dB** range keep
  ordinary playback in the lower/middle part of each bar. A bar only reaches
  the top when its own frequency range is genuinely dominant.
- The bars use a fast attack and a slower release with a constant gravity term,
  so they follow beats and still fall smoothly instead of sticking near the top.
  A small peak-hold cap falls on its own for the Cava-style look.
- Bars grow from a shared baseline (not from their centre) and fade towards the
  bottom, which reads as a real spectrum meter rather than a pulsing icon.
- Five skins are available in **Settings → Appearance → Audio bars skin**:
  - **Classic** — bottom bars with a falling peak cap.
  - **Mirror** — bars grow symmetrically from the vertical centre (the original
    capsule look).
  - **Dots** — LED columns that light up segment by segment.
  - **Wave** — a smooth filled waveform through the five bands.
  - **Neon** — thin accent-coloured tubes with a soft glow and a bright tip.
- The expanded music panel shows cover, title, progress and controls; the bars
  stay in the compact capsule.

## System status panel

Click the right side of the capsule while media is playing (or the capsule
itself when nothing is playing) to open the read-only status panel.

- Keyboard layout as the title.
- CPU usage and package temperature.
- GPU usage and temperature through a minimal dynamically loaded **NVML**
  binding — no `nvidia-smi` subprocess.
- RAM usage with the used amount only.
- Battery percentage and charging state.
- The `SystemStats` sampler runs only while the popup is open and stops when it
  closes.

MSI controls are intentionally **not** in the status popup (or anywhere else —
the context menu is gone). The island is a passive observer: a dashboard stays
a dashboard.

## MSI EC awareness

The applet reads the MSI EC attributes under
`/sys/devices/platform/msi-ec`:

- `shift_mode`, `fan_mode`, `cooler_boost`, `super_battery`.
- `webcam_block` / webcam kill switch.
- CPU/GPU temperatures and fan speeds for the status popup.

Values are read on demand and refreshed when the popup opens, when the MSI WMI
Fn-hotkey device reports a key press, or — the interesting part — when an
**external tool writes an attribute**: kernfs emits inotify `MODIFY` events for
sysfs attribute writes, so the plugin watches `cooler_boost`, `webcam`,
`webcam_block`, `fan_mode`, `shift_mode` and `super_battery` directly. Toggle
Cooler Boost from MControlCenter or a shell and the capsule announces it
instantly. There is no polling daemon.

### Firmware Fn keys (Cooler Boost) — kernel fork

The Fn Cooler Boost combo is executed by the EC firmware before the OS sees
it. The WMI notification *does* reach the kernel, but stock `msi-wmi` drops
unmapped vendor events with just a `dmesg` line, so nothing ever reached
userspace. `kernel/msi-wmi/` in this repo is a DKMS fork that reports **every
raw vendor event** as `EV_MSC/MSC_RAW` on the `MSI WMI hotkeys` input device;
the plugin's hotkey watcher catches those, re-reads the EC once and announces
the change — still fully event-driven:

```sh
cd kernel/msi-wmi
sudo ./install.sh
systemctl --user restart plasma-plasmashell.service
```

Bonus: dmesg now logs the actual vendor scancode (`Unknown key pressed -
0x…`), so events can be promoted to proper keycodes later.

### Passwordless writes (optional)

By default the MSI EC attributes are owned by root and are read-only for the
desktop user. The applet can still write through a one-shot
`pkexec /usr/bin/tee` fallback, which prompts through polkit.

For direct, passwordless writes install the optional udev rule once:

```sh
sudo ./scripts/install-msi-ec-permissions.sh
```

The rule makes the control attributes group-writable for `wheel` (override with
`MSI_EC_GROUP=...`) and triggers immediately. It is a udev rule, not a daemon:
it only runs when the `msi-ec` platform device appears or changes.

## Installation

### Requirements

- KDE Plasma **6.0+**
- Qt 6 and the Qt QML runtime
- PulseAudio or PipeWire with the PulseAudio compatibility layer
- `libpulse` development headers
- A C++ compiler, `moc` and `pkg-config`
- Optional: NVIDIA driver for GPU metrics through NVML
- Optional: `msi-ec` kernel module for MSI hardware controls

On Fedora, the build dependencies are roughly:

```sh
sudo dnf install qt6-qtbase-devel qt6-qtdeclarative-devel \
    pulseaudio-libs-devel gcc-c++ pkgconf
```

### Install the applet

Clone the fork into the Plasma applet directory:

```sh
git clone https://github.com/diegraldev/PlasmaKde-Dynamic-Islands.git \
    ~/.local/share/plasma/plasmoids/com.ifny75.dynamicisland
```

If you already have the original applet installed, back it up or remove it
first so the package id does not collide.

### Build the QML plugins

```sh
cd ~/.local/share/plasma/plasmoids/com.ifny75.dynamicisland
./build-plugins.sh
systemctl --user restart plasma-plasmashell.service
```

`build-plugins.sh` builds and installs these QML modules under
`~/.local/share/qml/`:

- `MicState`
- `VolumeWatcher`
- `CameraStatus`
- `KeyState`
- `MsiEc`
- `AudioLevels`
- `SystemStats`

The build scripts currently use Fedora's `/usr/lib64/qt6/libexec/moc` path.
Adjust that path in the individual `plugins/*/build.sh` files on other
distributions.

### Add the widget

Right-click the panel → **Add Widgets…** → search for **Dynamic Island**.
The applet id is `com.ifny75.dynamicisland`.

## Configuration

Right-click the island and open **Configure Dynamic Island** to adjust:

- size and compact width;
- background colour, opacity, corner radius and accent colour;
- which modules are enabled;
- clock/date formatting;
- notification behaviour;
- popup width and height.

The **context menu** is intentionally empty: MSI EC controls were removed in
favour of passive, event-driven announcements.

## Architecture

```
contents/
  ui/                  QML UI, compact capsule, popups and configuration pages
  config/              KConfigXT schema
plugins/
  micstate/            PulseAudio default-source mute watcher
  volumewatcher/       PulseAudio default-sink volume/mute watcher
  camera/              udev video4linux capture-device watcher
  keystate/            KWin Wayland lock-key watcher
  msiec/               MSI EC sysfs reader/writer and WMI hotkeys
  audiolevels/         libpulse spectrum capture and per-band normalization
  systemstats/         /proc CPU/RAM and dynamically loaded NVML GPU metrics
scripts/
  99-msi-ec-permissions.rules
  install-msi-ec-permissions.sh
  msi-ec-permissions
```

Apart from the short status-popup sampler, everything is event driven. There is
no polling daemon, no persistent helper process and no `pactl` subprocess.
PulseAudio, udev, D-Bus and input events drive the state changes directly.

## Performance

The applet is designed to stay quiet when nothing is happening. It is
event-driven: PulseAudio, UPower, udev, D-Bus, KWin and MSI WMI signals drive
the state, not background polling loops.

The only periodic work is deliberately small and scoped:

| Work | When it runs | Cost |
| --- | --- | --- |
| Clock tick | One wake-up per minute while idle; 1 Hz only when seconds, a countdown or an open media popup needs it. | Negligible. |
| Status sampler | 1 Hz only while the system status popup is open. | Reads `/proc/stat`, `/proc/meminfo` and NVML, then stops and unloads NVML when the popup closes. |
| Timer blink | 450 ms only while a KClock timer is ringing. | Tiny; stops when ringing ends. |
| Bar fallback animation | Only while media is playing without real audio levels yet. | Replaced by real levels as soon as the monitor stream is ready. |
| One-shot timers | Popup close, MSI refresh, battery resync, build/notification expiry, reconnect retries. | Milliseconds, only after a real event. |

There is no polling daemon, no persistent helper process and no `pactl`
subprocess. The audio stream is opened only while media is playing and closed
on pause. If you want to trim the last bit of work, the status sampler can be
changed from 1 Hz to 1.5–2 Hz in `plugins/systemstats/systemstats.cpp`; the
difference is only noticeable in how quickly the dashboard reacts.

## Known limitations

- GPU metrics currently use NVML, so they are NVIDIA-only. AMD/Intel support
  would need an additional backend.
- MSI EC controls are specific to laptops with a compatible `msi-ec` driver.
- Per-application audio capture is best-effort: if the player identity cannot
  be matched to a sink input, the default sink monitor is used.
- MControlCenter does not always refresh after an external sysfs write; reopen
  it if its values look stale. The actual MSI EC values are correct.
- RGB and keyboard backlighting are intentionally out of scope for this fork. Check [rgb-backlight](https://github.com/diegraldev/rgb-backlight).

## Ideas / roadmap

Things that would fit naturally in future versions:

**More status information**

- Network throughput and latency
- Disk I/O and free space
- Top CPU/RAM process
- Fan RPM and GPU VRAM
- Battery health, cycles and time-to-full/empty
- Temperature history graph
- VPN, Bluetooth and KDE Connect state
- Next calendar event and weather
- Clipboard and screen-recording state
- CPU governor and power profile

**More interaction**

- Click-through actions (open System Monitor, KDE Connect, etc.)
- Notification actions directly from the capsule
- Quick toggles for Do Not Disturb, Night Light and media
- Pomodoro/focus mode
- Per-application volume
- Multiple island layouts and module ordering

**More integrations**

- AMD/Intel GPU backends
- NetworkManager, BlueZ and UPower signals
- KWin effects and workspace state
- Lyrics and richer media metadata

## Version history

| Tag | Highlights |
| --- | --- |
| `v1.6.0` | Five selectable audio-bar skins: Classic, Mirror, Dots, Wave and Neon. |
| `v1.5.8` | Sharing-dot width fix, Cava-style audio bars and the touchpad capsule. |
| `v1.5.7` | Right-pinned popup anchor shared by media and status. |
| `v1.5.6` | Responsive popup width capped by the available screen space. |
| `v1.5.5` | Status popup cleanup, narrower cards and the restored compact capsule. |
| `v1.5.4` | Compact status tiles, RAM used-only readout and a right gutter for the capsule. |
| `v1.5.3` | Status panel cleanup, popup edge protection, public fork README. |
| `v1.5.2` | CPU/GPU/RAM/battery status tiles and stricter bar calibration. |
| `v1.5.1` | Wider bar range, musical band split and tighter compact spacing. |
| `v1.5.0` | Per-app audio capture, compact bars and media/status click zones. |
| `v1.4.0` | Event-driven watchers and MSI EC controls. |
| `v1.3.0` | Initial audit and hardware integration work. |

## Credits

- Original applet: [ifny75](https://github.com/ifny75/PlasmaKde-Dynamic-Islands)
- Fork and hardware-specific work: [diegraldev](https://github.com/diegraldev)
- Built with KDE Plasma, Qt 6, PulseAudio/PipeWire and the MSI EC kernel driver.

## License

MIT. See [LICENSE](LICENSE); the original copyright and attribution are preserved.
