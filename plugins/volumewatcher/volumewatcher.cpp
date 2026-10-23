#include <QObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QProcess>
#include <QDebug>
#include <QTimer>

class VolumeWatcher : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(int volume READ volume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)

public:
    explicit VolumeWatcher(QObject *parent = nullptr)
        : QObject(parent), m_running(true)
    {
        startWatching();
    }

    ~VolumeWatcher()
    {
        stopWatching();
    }

    bool running() const { return m_running; }
    int volume() const { return m_volume; }
    bool muted() const { return m_muted; }

public slots:
    void setRunning(bool running)
    {
        if (m_running == running)
            return;
        m_running = running;
        if (m_running)
            startWatching();
        else
            stopWatching();
        emit runningChanged();
    }

    void setVolume(int volume)
    {
        if (volume < 0 || volume > 100)
            return;
        QProcess::startDetached("pactl", {"set-sink-volume", "@DEFAULT_SINK@", QString::number(volume) + "%"});
    }

    void toggleMute()
    {
        QProcess::startDetached("pactl", {"set-sink-mute", "@DEFAULT_SINK@", "toggle"});
    }

signals:
    void runningChanged();
    void volumeChanged();
    void mutedChanged();
    void volumeAnnounce(int volume, bool muted);

private:
    void startWatching()
    {
        if (m_subscribeProc)
            return;

        fetchState();

        m_subscribeProc = new QProcess(this);
        m_subscribeProc->setProgram("pactl");
        m_subscribeProc->setArguments({"subscribe", "change"});
        m_subscribeProc->setProcessChannelMode(QProcess::MergedChannels);
        m_subscribeProc->start();

        connect(m_subscribeProc, &QProcess::readyReadStandardOutput,
                this, &VolumeWatcher::onSubscribeOutput);
        connect(m_subscribeProc, &QProcess::readyReadStandardError,
                this, &VolumeWatcher::onSubscribeOutput);
        connect(m_subscribeProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &VolumeWatcher::onSubscribeFinished);

        // Handle case where pactl exits unexpectedly
        QTimer::singleShot(2000, this, [this]() {
            if (m_running && (!m_subscribeProc || !m_subscribeProc->isOpen())) {
                onSubscribeFinished(-1, QProcess::NormalExit);
            }
        });
    }

    void stopWatching()
    {
        if (m_subscribeProc) {
            m_subscribeProc->kill();
            m_subscribeProc->waitForFinished(1000);
            m_subscribeProc->deleteLater();
            m_subscribeProc = nullptr;
        }
    }

    void fetchState()
    {
        QProcess proc;
        proc.start("pactl", {"get-sink-volume", "@DEFAULT_SINK@"});
        if (proc.waitForFinished(3000)) {
            QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
            int percentIdx = out.indexOf("%");
            if (percentIdx > 0) {
                int start = out.lastIndexOf(' ', percentIdx);
                QString numStr = out.mid(start + 1, percentIdx - start - 1).trimmed();
                bool ok;
                int vol = numStr.toInt(&ok);
                if (ok) {
                    if (vol != m_volume) {
                        m_volume = vol;
                        emit volumeChanged();
                    }
                }
            }
        }

        QProcess proc2;
        proc2.start("pactl", {"get-sink-mute", "@DEFAULT_SINK@"});
        if (proc2.waitForFinished(3000)) {
            QString out = QString::fromLocal8Bit(proc2.readAllStandardOutput());
            bool muted = out.contains("yes");
            if (muted != m_muted) {
                m_muted = muted;
                emit mutedChanged();
            }
        }
    }

    void onSubscribeOutput()
    {
        if (!m_subscribeProc)
            return;
        QByteArray data = m_subscribeProc->readAll();
        if (data.contains("sink") && (data.contains("change") || data.contains("new"))) {
            int prevVol = m_volume;
            bool prevMuted = m_muted;
            fetchState();
            // Only announce when the volume percentage actually changed,
            // not on mute toggles or stream changes from media play/pause.
            if (m_volume != prevVol) {
                emit volumeAnnounce(m_volume, m_muted);
            }
        }
    }

    void onSubscribeFinished(int exitCode, QProcess::ExitStatus status)
    {
        Q_UNUSED(exitCode)
        Q_UNUSED(status)
        if (m_subscribeProc) {
            m_subscribeProc->deleteLater();
            m_subscribeProc = nullptr;
        }
        if (m_running) {
            QTimer::singleShot(5000, this, &VolumeWatcher::startWatching);
        }
    }

    QProcess *m_subscribeProc = nullptr;
    bool m_running = true;
    int m_volume = 0;
    bool m_muted = false;
};

class VolumeWatcherPlugin : public QQmlExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QQmlExtensionInterface")

public:
    void registerTypes(const char *uri) override
    {
        Q_UNUSED(uri)
        qmlRegisterType<VolumeWatcher>(uri, 1, 0, "VolumeWatcher");
    }
};

#include "volumewatcher.moc"

