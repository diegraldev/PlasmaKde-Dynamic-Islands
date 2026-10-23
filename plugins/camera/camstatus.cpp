#include <QObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QSocketNotifier>
#include <QSet>
#include <QString>
#include <QTimer>

#include <libudev.h>

#include <cstring>

// Event-driven webcam availability.
//
// udev is the source of truth: a monitor on the "video4linux" subsystem wakes
// us only when a camera is added/removed, and enumeration filters out metadata
// nodes such as /dev/video1 (ID_V4L_CAPABILITIES has no :capture:). The old
// implementation counted every /dev/video* inode, so a metadata node made a
// blocked/unplugged camera look present.
class CameraStatus : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool cameraAvailable READ cameraAvailable NOTIFY cameraAvailableChanged)
    Q_PROPERTY(int videoCount READ videoCount NOTIFY videoCountChanged)
    Q_PROPERTY(QString cameraName READ cameraName NOTIFY cameraNameChanged)

public:
    explicit CameraStatus(QObject *parent = nullptr)
        : QObject(parent)
    {
        m_udev = udev_new();
        if (m_udev) {
            m_monitor = udev_monitor_new_from_netlink(m_udev, "udev");
            if (m_monitor) {
                udev_monitor_filter_add_match_subsystem_devtype(m_monitor, "video4linux", nullptr);
                udev_monitor_enable_receiving(m_monitor);
                const int fd = udev_monitor_get_fd(m_monitor);
                if (fd >= 0) {
                    m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
                    connect(m_notifier, &QSocketNotifier::activated,
                            this, &CameraStatus::onUdevEvent);
                }
            }
        }
        rescan();
    }

    ~CameraStatus() override
    {
        delete m_notifier;
        m_notifier = nullptr;
        if (m_monitor) {
            udev_monitor_unref(m_monitor);
            m_monitor = nullptr;
        }
        if (m_udev) {
            udev_unref(m_udev);
            m_udev = nullptr;
        }
    }

    bool cameraAvailable() const { return m_count > 0; }
    int videoCount() const { return m_count; }
    QString cameraName() const { return m_name; }

signals:
    void cameraAvailableChanged();
    void videoCountChanged();
    void cameraNameChanged();

private slots:
    void onUdevEvent()
    {
        if (!m_monitor) {
            return;
        }
        // Drain every queued event; one rescan after the batch is enough.
        while (udev_device *device = udev_monitor_receive_device(m_monitor)) {
            udev_device_unref(device);
        }
        rescan();
    }

private:
    static bool isCaptureDevice(udev_device *device)
    {
        const char *caps = udev_device_get_property_value(device, "ID_V4L_CAPABILITIES");
        if (!caps) {
            return false;
        }
        return std::strstr(caps, ":capture:") != nullptr;
    }

    static QString deviceName(udev_device *device)
    {
        const char *product = udev_device_get_property_value(device, "ID_V4L_PRODUCT");
        if (product && *product) {
            return QString::fromUtf8(product);
        }
        const char *model = udev_device_get_property_value(device, "ID_MODEL");
        if (model && *model) {
            return QString::fromUtf8(model).replace(QLatin1Char('_'), QLatin1Char(' '));
        }
        return QString();
    }

    void rescan()
    {
        if (!m_udev) {
            return;
        }

        QSet<QString> devices;
        QString firstName;

        udev_enumerate *enumerate = udev_enumerate_new(m_udev);
        if (enumerate) {
            udev_enumerate_add_match_subsystem(enumerate, "video4linux");
            udev_enumerate_scan_devices(enumerate);

            udev_list_entry *entries = udev_enumerate_get_list_entry(enumerate);
            udev_list_entry *entry = nullptr;
            udev_list_entry_foreach(entry, entries) {
                const char *syspath = udev_list_entry_get_name(entry);
                if (!syspath) {
                    continue;
                }
                udev_device *device = udev_device_new_from_syspath(m_udev, syspath);
                if (!device) {
                    continue;
                }
                if (isCaptureDevice(device)) {
                    const char *devnode = udev_device_get_devnode(device);
                    if (devnode && *devnode) {
                        devices.insert(QString::fromUtf8(devnode));
                        if (firstName.isEmpty()) {
                            firstName = deviceName(device);
                        }
                    }
                }
                udev_device_unref(device);
            }
            udev_enumerate_unref(enumerate);
        }

        const int count = devices.size();
        const QString name = count > 0 ? firstName : QString();

        if (count != m_count) {
            m_count = count;
            Q_EMIT videoCountChanged();
            Q_EMIT cameraAvailableChanged();
        }
        if (name != m_name) {
            m_name = name;
            Q_EMIT cameraNameChanged();
        }
    }

    udev *m_udev = nullptr;
    udev_monitor *m_monitor = nullptr;
    QSocketNotifier *m_notifier = nullptr;
    int m_count = 0;
    QString m_name;
};

class CameraStatusPlugin : public QQmlExtensionPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void registerTypes(const char *uri) override
    {
        qmlRegisterSingletonType<CameraStatus>(uri, 1, 0, "CameraStatus",
            [](QQmlEngine *, QJSEngine *) -> QObject * {
                return new CameraStatus;
            });
    }
};

#include "camstatus.moc"
