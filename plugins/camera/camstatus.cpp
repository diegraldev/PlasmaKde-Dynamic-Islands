#include <QObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QSocketNotifier>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QDebug>

#include <sys/inotify.h>
#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

class CameraStatus : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool cameraAvailable READ cameraAvailable NOTIFY cameraAvailableChanged)
    Q_PROPERTY(int videoCount READ videoCount NOTIFY videoCountChanged)

public:
    explicit CameraStatus(QObject *parent = nullptr)
        : QObject(parent)
    {
        const QByteArray envDir = qgetenv("CAMSTATUS_WATCH_DIR");
        m_watchDir = envDir.isEmpty() ? QStringLiteral("/dev") : QString::fromLocal8Bit(envDir);
        m_count = countVideos();
        m_inotifyFd = inotify_init1(IN_NONBLOCK);
        if (m_inotifyFd >= 0) {
            inotify_add_watch(m_inotifyFd, m_watchDir.toLocal8Bit().constData(),
                              IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
            m_notifier = new QSocketNotifier(m_inotifyFd, QSocketNotifier::Read, this);
            connect(m_notifier, &QSocketNotifier::activated, this, &CameraStatus::onEvent);
        }
        reportState();
    }

    ~CameraStatus()
    {
        if (m_inotifyFd >= 0) {
            close(m_inotifyFd);
        }
    }

    bool cameraAvailable() const { return m_count > 0; }
    int videoCount() const { return m_count; }

signals:
    void cameraAvailableChanged();
    void videoCountChanged();

private slots:
    void onEvent()
    {
        char buf[4096];
        while (read(m_inotifyFd, buf, sizeof(buf)) > 0) {
            // drain all pending events
        }
        QTimer::singleShot(0, this, &CameraStatus::rescan);
    }

    void rescan()
    {
        const int n = countVideos();
        if (n != m_count) {
            m_count = n;
            emit cameraAvailableChanged();
            emit videoCountChanged();
            reportState();
        }
    }

private:
    int countVideos() const
    {
        int n = 0;
        DIR *d = opendir(m_watchDir.toLocal8Bit().constData());
        if (!d)
            return 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            if (strncmp(e->d_name, "video", 5) == 0 && e->d_type != DT_DIR && e->d_type != DT_LNK)
                ++n;
        }
        closedir(d);
        return n;
    }

    void reportState() const
    {
        qInfo() << "CameraStatus: watching" << m_watchDir << "videoCount=" << m_count
                << "available=" << (m_count > 0);
    }

    int m_count = 0;
    int m_inotifyFd = -1;
    QString m_watchDir;
    QSocketNotifier *m_notifier = nullptr;
};

class CameraStatusPlugin : public QQmlExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void registerTypes(const char *uri) override
    {
        Q_UNUSED(uri)
        qmlRegisterSingletonType<CameraStatus>(uri, 1, 0, "CameraStatus",
            [](QQmlEngine *, QJSEngine *) -> QObject * {
                return new CameraStatus;
            });
    }
};

#include "camstatus.moc"
