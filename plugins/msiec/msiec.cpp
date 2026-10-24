#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

// MSI EC controls for the Pulse GL66 (MS-1581).
//
// The kernel's msi-ec driver exposes fan modes, Cooler Boost, Shift modes,
// Super Battery, the webcam kill switch and the Fn/Win key positions through
// sysfs. Reading is free; the plugin is fully passive now: values refresh
// when the popup opens, when an external tool writes one of the sysfs
// attributes (kernfs emits inotify MODIFY for attribute writes, so the files
// are watched directly), or when the MSI WMI hotkey input device emits a key
// event. No daemon, no polling.
class MsiEc : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool canControl READ canControl NOTIFY canControlChanged)
    Q_PROPERTY(bool coolerBoost READ coolerBoost NOTIFY coolerBoostChanged)
    Q_PROPERTY(bool superBattery READ superBattery NOTIFY superBatteryChanged)
    Q_PROPERTY(bool webcamBlocked READ webcamBlocked NOTIFY webcamBlockedChanged)
    Q_PROPERTY(bool webcamOn READ webcamOn NOTIFY webcamOnChanged)
    Q_PROPERTY(QString fanMode READ fanMode NOTIFY fanModeChanged)
    Q_PROPERTY(QStringList availableFanModes READ availableFanModes NOTIFY availableFanModesChanged)
    Q_PROPERTY(QString shiftMode READ shiftMode NOTIFY shiftModeChanged)
    Q_PROPERTY(QStringList availableShiftModes READ availableShiftModes NOTIFY availableShiftModesChanged)
    Q_PROPERTY(QString winKey READ winKey NOTIFY winKeyChanged)
    Q_PROPERTY(QString fnKey READ fnKey NOTIFY fnKeyChanged)
    Q_PROPERTY(int cpuTemp READ cpuTemp NOTIFY cpuTempChanged)
    Q_PROPERTY(int cpuFan READ cpuFan NOTIFY cpuFanChanged)
    Q_PROPERTY(int gpuTemp READ gpuTemp NOTIFY gpuTempChanged)
    Q_PROPERTY(int gpuFan READ gpuFan NOTIFY gpuFanChanged)
    Q_PROPERTY(QString model READ model NOTIFY modelChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit MsiEc(QObject *parent = nullptr)
        : QObject(parent)
    {
        m_root = QStringLiteral("/sys/devices/platform/msi-ec");
        m_available = QFileInfo::exists(m_root + QStringLiteral("/fw_version"));
        m_model = readText(QStringLiteral("/sys/devices/virtual/dmi/id/product_name"));
        if (m_model.isEmpty()) {
            m_model = QStringLiteral("MSI");
        }
        m_pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        m_canControl = m_available && (!m_pkexec.isEmpty() || QFileInfo(m_root + QStringLiteral("/cooler_boost")).isWritable());

        if (m_available) {
            m_fsWatcher = new QFileSystemWatcher(this);
            m_fsWatcher->addPath(m_root);
            // External tools (MControlCenter, shell scripts) write the
            // attributes directly. kernfs emits inotify MODIFY for sysfs
            // attribute writes, so watching the files themselves catches
            // those changes for free. Our own reads never produce MODIFY, so
            // this cannot loop.
            const QStringList watchedAttributes = {
                QStringLiteral("cooler_boost"),
                QStringLiteral("super_battery"),
                QStringLiteral("webcam"),
                QStringLiteral("webcam_block"),
                QStringLiteral("fan_mode"),
                QStringLiteral("shift_mode"),
            };
            for (const QString &attr : watchedAttributes) {
                const QString path = m_root + QLatin1Char('/') + attr;
                if (QFileInfo::exists(path)) {
                    m_fsWatcher->addPath(path);
                }
            }
            connect(m_fsWatcher, &QFileSystemWatcher::directoryChanged,
                    this, &MsiEc::scheduleFsRefresh);
            connect(m_fsWatcher, &QFileSystemWatcher::fileChanged,
                    this, [this](const QString &path) {
                // Watches can be dropped after a change event; sysfs
                // attributes persist, so re-arm defensively.
                if (!m_fsWatcher->files().contains(path) && QFileInfo::exists(path)) {
                    m_fsWatcher->addPath(path);
                }
                scheduleFsRefresh();
            });
            m_fsRefresh.setSingleShot(true);
            m_fsRefresh.setInterval(80);
            connect(&m_fsRefresh, &QTimer::timeout, this, &MsiEc::refresh);
            setupHotkeyWatcher();
            setupTouchpadWatcher();
            refresh();
        }
    }

    ~MsiEc() override
    {
        if (m_writeProc) {
            m_writeProc->kill();
            m_writeProc->waitForFinished(500);
        }
        if (m_hotkeyFd >= 0) {
            ::close(m_hotkeyFd);
        }
        if (m_touchpadFd >= 0) {
            ::close(m_touchpadFd);
        }
    }

    bool available() const { return m_available; }
    bool canControl() const { return m_canControl; }
    bool coolerBoost() const { return m_coolerBoost; }
    bool superBattery() const { return m_superBattery; }
    bool webcamBlocked() const { return m_webcamBlocked; }
    bool webcamOn() const { return m_webcamOn; }
    QString fanMode() const { return m_fanMode; }
    QStringList availableFanModes() const { return m_availableFanModes; }
    QString shiftMode() const { return m_shiftMode; }
    QStringList availableShiftModes() const { return m_availableShiftModes; }
    QString winKey() const { return m_winKey; }
    QString fnKey() const { return m_fnKey; }
    int cpuTemp() const { return m_cpuTemp; }
    int cpuFan() const { return m_cpuFan; }
    int gpuTemp() const { return m_gpuTemp; }
    int gpuFan() const { return m_gpuFan; }
    QString model() const { return m_model; }
    QString lastError() const { return m_lastError; }

public Q_SLOTS:
    void refresh()
    {
        if (!m_available) {
            return;
        }
        const bool oldAvailable = m_available;
        m_available = QFileInfo::exists(m_root + QStringLiteral("/fw_version"));
        if (oldAvailable != m_available) {
            Q_EMIT availableChanged();
        }
        if (!m_available) {
            return;
        }

        setBool(m_coolerBoost, readText(m_root + QStringLiteral("/cooler_boost")) == QStringLiteral("on"),
                &MsiEc::coolerBoostChanged);
        setBool(m_superBattery, readText(m_root + QStringLiteral("/super_battery")) == QStringLiteral("on"),
                &MsiEc::superBatteryChanged);
        setBool(m_webcamOn, readText(m_root + QStringLiteral("/webcam")) == QStringLiteral("on"),
                &MsiEc::webcamOnChanged);
        const bool blocked = readText(m_root + QStringLiteral("/webcam_block")) == QStringLiteral("on")
            || !m_webcamOn;
        setBool(m_webcamBlocked, blocked, &MsiEc::webcamBlockedChanged);

        const QStringList fanModes = readList(m_root + QStringLiteral("/available_fan_modes"));
        if (fanModes != m_availableFanModes) {
            m_availableFanModes = fanModes;
            Q_EMIT availableFanModesChanged();
        }
        const QStringList shiftModes = readList(m_root + QStringLiteral("/available_shift_modes"));
        if (shiftModes != m_availableShiftModes) {
            m_availableShiftModes = shiftModes;
            Q_EMIT availableShiftModesChanged();
        }
        setString(m_fanMode, readText(m_root + QStringLiteral("/fan_mode")), &MsiEc::fanModeChanged);
        setString(m_shiftMode, readText(m_root + QStringLiteral("/shift_mode")), &MsiEc::shiftModeChanged);
        setString(m_winKey, readText(m_root + QStringLiteral("/win_key")), &MsiEc::winKeyChanged);
        setString(m_fnKey, readText(m_root + QStringLiteral("/fn_key")), &MsiEc::fnKeyChanged);

        setInt(m_cpuTemp, readInt(m_root + QStringLiteral("/cpu/realtime_temperature")), &MsiEc::cpuTempChanged);
        setInt(m_cpuFan, readInt(m_root + QStringLiteral("/cpu/realtime_fan_speed")), &MsiEc::cpuFanChanged);
        setInt(m_gpuTemp, readInt(m_root + QStringLiteral("/gpu/realtime_temperature")), &MsiEc::gpuTempChanged);
        setInt(m_gpuFan, readInt(m_root + QStringLiteral("/gpu/realtime_fan_speed")), &MsiEc::gpuFanChanged);

        const bool oldCanControl = m_canControl;
        m_canControl = m_available && (!m_pkexec.isEmpty()
            || QFileInfo(m_root + QStringLiteral("/cooler_boost")).isWritable());
        if (oldCanControl != m_canControl) {
            Q_EMIT canControlChanged();
        }
    }

    void setCoolerBoost(bool on) { writeValue(QStringLiteral("cooler_boost"), on ? QStringLiteral("on") : QStringLiteral("off")); }
    void toggleCoolerBoost() { setCoolerBoost(!m_coolerBoost); }
    void setSuperBattery(bool on) { writeValue(QStringLiteral("super_battery"), on ? QStringLiteral("on") : QStringLiteral("off")); }
    void toggleSuperBattery() { setSuperBattery(!m_superBattery); }
    void setWebcamBlocked(bool blocked) { writeValue(QStringLiteral("webcam_block"), blocked ? QStringLiteral("on") : QStringLiteral("off")); }
    void toggleWebcamBlocked() { setWebcamBlocked(!m_webcamBlocked); }

    void setFanMode(const QString &mode)
    {
        if (m_availableFanModes.contains(mode)) {
            writeValue(QStringLiteral("fan_mode"), mode);
        }
    }

    void setShiftMode(const QString &mode)
    {
        if (m_availableShiftModes.contains(mode)) {
            writeValue(QStringLiteral("shift_mode"), mode);
        }
    }

    void setWinKey(const QString &position)
    {
        if (position == QStringLiteral("left") || position == QStringLiteral("right")) {
            writeValue(QStringLiteral("win_key"), position);
        }
    }

    void setFnKey(const QString &position)
    {
        if (position == QStringLiteral("left") || position == QStringLiteral("right")) {
            writeValue(QStringLiteral("fn_key"), position);
        }
    }

Q_SIGNALS:
    void availableChanged();
    void canControlChanged();
    void coolerBoostChanged();
    void superBatteryChanged();
    void webcamBlockedChanged();
    void webcamOnChanged();
    void fanModeChanged();
    void availableFanModesChanged();
    void shiftModeChanged();
    void availableShiftModesChanged();
    void winKeyChanged();
    void fnKeyChanged();
    void cpuTempChanged();
    void cpuFanChanged();
    void gpuTempChanged();
    void gpuFanChanged();
    void modelChanged();
    void lastErrorChanged();
    // Fired when the AT keyboard reports KEY_TOUCHPAD_TOGGLE (530): the Fn
    // touchpad hotkey (remapped from the EC's Ctrl+Meta+F24 by the hwdb
    // rule shipped in scripts/). KWin toggles the touchpad for this key but
    // does not emit PropertiesChanged on its input DBus objects, so this is
    // the only event-driven notification of the toggle.
    void touchpadToggle();

private:
    void scheduleFsRefresh()
    {
        // Coalesce a burst of attribute writes into a single re-read.
        if (!m_fsRefresh.isActive()) {
            m_fsRefresh.start();
        }
    }

    static QString readText(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString();
        }
        return QString::fromUtf8(file.readAll()).trimmed();
    }

    static QStringList readList(const QString &path)
    {
        QStringList out;
        const QStringList lines = readText(path).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QString value = line.trimmed();
            if (!value.isEmpty()) {
                out.append(value);
            }
        }
        return out;
    }

    static int readInt(const QString &path)
    {
        bool ok = false;
        const int value = readText(path).toInt(&ok);
        return ok ? value : 0;
    }

    template<typename T>
    void setBool(T &member, bool value, void (MsiEc::*signal)())
    {
        if (member != value) {
            member = value;
            (this->*signal)();
        }
    }

    void setString(QString &member, const QString &value, void (MsiEc::*signal)())
    {
        if (member != value) {
            member = value;
            (this->*signal)();
        }
    }

    void setInt(int &member, int value, void (MsiEc::*signal)())
    {
        if (member != value) {
            member = value;
            (this->*signal)();
        }
    }

    void setError(const QString &error)
    {
        if (m_lastError != error) {
            m_lastError = error;
            Q_EMIT lastErrorChanged();
        }
    }

    void writeValue(const QString &attribute, const QString &value)
    {
        if (!m_available) {
            setError(QStringLiteral("MSI EC is not available"));
            return;
        }
        const QString path = m_root + QLatin1Char('/') + attribute;
        QFileInfo info(path);
        if (!info.exists()) {
            setError(QStringLiteral("%1 is not exposed by this EC").arg(attribute));
            return;
        }

        if (info.isWritable()) {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                setError(QStringLiteral("Cannot write %1").arg(attribute));
                return;
            }
            file.write(value.toUtf8() + '\n');
            file.close();
            setError(QString());
            QTimer::singleShot(150, this, &MsiEc::refresh);
            return;
        }

        if (m_pkexec.isEmpty()) {
            setError(QStringLiteral("No permission for %1 (install the optional udev rule)").arg(attribute));
            return;
        }
        if (m_writeProc) {
            setError(QStringLiteral("Another MSI EC operation is still running"));
            return;
        }

        m_writeProc = new QProcess(this);
        m_writeProc->setProgram(m_pkexec);
        m_writeProc->setArguments({QStringLiteral("/usr/bin/tee"), path});
        connect(m_writeProc, &QProcess::started, this, [this, value] {
            if (!m_writeProc) {
                return;
            }
            m_writeProc->write(value.toUtf8() + '\n');
            m_writeProc->closeWriteChannel();
        });
        connect(m_writeProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            if (m_writeProc) {
                m_writeProc->deleteLater();
                m_writeProc = nullptr;
            }
            setError(QStringLiteral("MSI EC authorization failed"));
        });
        connect(m_writeProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int exitCode, QProcess::ExitStatus status) {
            if (m_writeProc) {
                m_writeProc->deleteLater();
                m_writeProc = nullptr;
            }
            if (exitCode == 0 && status == QProcess::NormalExit) {
                setError(QString());
                QTimer::singleShot(150, this, &MsiEc::refresh);
            } else {
                setError(QStringLiteral("MSI EC write was not authorized"));
            }
        });
        m_writeProc->start();
    }

    void setupHotkeyWatcher()
    {
        // msi_wmi creates an input device for Fn hotkeys. Listening to it makes
        // external Fn changes (webcam kill switch, fan profile) visible without
        // polling. The device is in the `input` group on this system.
        const QDir inputDir(QStringLiteral("/sys/class/input"));
        const QStringList entries = inputDir.entryList({QStringLiteral("event*")}, QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            const QString name = readText(inputDir.filePath(entry + QStringLiteral("/device/name")));
            if (!name.contains(QStringLiteral("MSI WMI hotkeys"), Qt::CaseInsensitive)) {
                continue;
            }
            const QString devPath = QStringLiteral("/dev/input/") + entry;
            m_hotkeyFd = ::open(devPath.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (m_hotkeyFd < 0) {
                return;
            }
            m_hotkeyNotifier = new QSocketNotifier(m_hotkeyFd, QSocketNotifier::Read, this);
            connect(m_hotkeyNotifier, &QSocketNotifier::activated, this, [this] {
                struct input_event events[32];
                while (true) {
                    const ssize_t bytes = ::read(m_hotkeyFd, events, sizeof(events));
                    if (bytes <= 0) {
                        break;
                    }
                    const int count = static_cast<int>(bytes / sizeof(struct input_event));
                    for (int i = 0; i < count; ++i) {
                        // Key presses, autorepeats and raw MSC scans (some WMI
                        // events are not mapped to keycodes) all count as
                        // "something happened on the island's EC hotkeys".
                        if ((events[i].type == EV_KEY && (events[i].value == 1 || events[i].value == 2))
                            || events[i].type == EV_MSC) {
                            // Coalesce the burst of key events into one refresh.
                            if (!m_hotkeyRefresh.isActive()) {
                                m_hotkeyRefresh.start();
                            }
                        }
                    }
                }
            });
            m_hotkeyRefresh.setSingleShot(true);
            m_hotkeyRefresh.setInterval(120);
            connect(&m_hotkeyRefresh, &QTimer::timeout, this, &MsiEc::refresh);
            return;
        }
    }

    void setupTouchpadWatcher()
    {
        // The Fn touchpad key arrives on the AT keyboard as
        // KEY_TOUCHPAD_TOGGLE after the hwdb remap. Watch the stable by-path
        // node and ignore everything else — no key is ever logged.
        static const char kPath[] = "/dev/input/by-path/platform-i8042-serio-0-event-kbd";
        m_touchpadFd = ::open(kPath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (m_touchpadFd < 0) {
            m_touchpadFd = ::open(kPath, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        }
        if (m_touchpadFd < 0) {
            return;
        }
        // Self-heal the scancode mapping: the EC sends the touchpad hotkey
        // as scancode 0x76 (LEFTCTRL+LEFTMETA+F24) and the hwdb rule in
        // scripts/ remaps it to KEY_TOUCHPAD_TOGGLE — but anything that
        // resets the kernel keymap (driver rebind, early-boot udev race)
        // leaves a dead F24 that nobody listens to. Re-applying it here
        // keeps the hotkey working regardless of udev timing.
        int keymap[2] = { 0x76, KEY_TOUCHPAD_TOGGLE };
        ::ioctl(m_touchpadFd, EVIOCSKEYCODE, keymap);
        m_touchpadNotifier = new QSocketNotifier(m_touchpadFd, QSocketNotifier::Read, this);
        connect(m_touchpadNotifier, &QSocketNotifier::activated, this, [this] {
            struct input_event events[32];
            bool toggled = false;
            while (true) {
                const ssize_t bytes = ::read(m_touchpadFd, events, sizeof(events));
                if (bytes <= 0) {
                    break;
                }
                const int count = static_cast<int>(bytes / sizeof(struct input_event));
                for (int i = 0; i < count; i++) {
                    if (events[i].type == EV_KEY && events[i].code == 530 && events[i].value == 1) {
                        toggled = true;
                    }
                }
            }
            if (toggled) {
                Q_EMIT touchpadToggle();
            }
        });
    }

    QString m_root;
    QString m_model;
    QString m_pkexec;
    QString m_lastError;
    bool m_available = false;
    bool m_canControl = false;
    bool m_coolerBoost = false;
    bool m_superBattery = false;
    bool m_webcamBlocked = false;
    bool m_webcamOn = true;
    QString m_fanMode;
    QStringList m_availableFanModes;
    QString m_shiftMode;
    QStringList m_availableShiftModes;
    QString m_winKey;
    QString m_fnKey;
    int m_cpuTemp = 0;
    int m_cpuFan = 0;
    int m_gpuTemp = 0;
    int m_gpuFan = 0;
    QFileSystemWatcher *m_fsWatcher = nullptr;
    QTimer m_fsRefresh;
    QProcess *m_writeProc = nullptr;
    QSocketNotifier *m_hotkeyNotifier = nullptr;
    int m_hotkeyFd = -1;
    QTimer m_hotkeyRefresh;
    QSocketNotifier *m_touchpadNotifier = nullptr;
    int m_touchpadFd = -1;
};

class MsiEcPlugin : public QQmlExtensionPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void registerTypes(const char *uri) override
    {
        qmlRegisterType<MsiEc>(uri, 1, 0, "MsiEc");
    }
};

#include "msiec.moc"
