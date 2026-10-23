#include <QObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QMetaObject>
#include <QTimer>

#include <pulse/pulseaudio.h>

#include <atomic>
#include <cmath>
#include <string>

// Event-driven default-sink volume/mute watcher.
//
// libpulse's threaded mainloop subscribes to SINK/SERVER events. There is no
// `pactl subscribe` child process and no `pactl` process per slider move; the
// widget opens one PulseAudio connection while enabled and closes it when the
// module is disabled. All QML signals are emitted on the GUI thread.
class VolumeWatcher : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(int volume READ volume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)

public:
    explicit VolumeWatcher(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~VolumeWatcher() override
    {
        stopPulse();
    }

    bool running() const { return m_running.load(); }
    int volume() const { return m_volume.load(); }
    bool muted() const { return m_muted.load(); }
    bool ready() const { return m_ready.load(); }

public Q_SLOTS:
    void setRunning(bool running)
    {
        if (m_running.load() == running) {
            return;
        }
        m_running.store(running);
        if (running) {
            startPulse();
        } else {
            stopPulse();
        }
        Q_EMIT runningChanged();
    }

    void setVolume(int volume)
    {
        if (volume < 0 || volume > 100 || !m_loop) {
            return;
        }
        pa_threaded_mainloop_lock(m_loop);
        if (m_context && !m_sinkName.empty()) {
            pa_cvolume cvol;
            const int channels = m_channels > 0 ? m_channels : 2;
            pa_cvolume_set(&cvol, channels,
                           static_cast<pa_volume_t>(qRound(PA_VOLUME_NORM * (volume / 100.0))));
            pa_operation *op = pa_context_set_sink_volume_by_name(
                m_context, m_sinkName.c_str(), &cvol, nullptr, nullptr);
            if (op) {
                pa_operation_unref(op);
            }
        }
        pa_threaded_mainloop_unlock(m_loop);
    }

    void toggleMute()
    {
        if (!m_loop) {
            return;
        }
        pa_threaded_mainloop_lock(m_loop);
        if (m_context && !m_sinkName.empty()) {
            pa_operation *op = pa_context_set_sink_mute_by_name(
                m_context, m_sinkName.c_str(), m_muted.load() ? 0 : 1, nullptr, nullptr);
            if (op) {
                pa_operation_unref(op);
            }
        }
        pa_threaded_mainloop_unlock(m_loop);
    }

    void refresh()
    {
        if (!m_loop) {
            return;
        }
        pa_threaded_mainloop_lock(m_loop);
        requestServerInfo();
        pa_threaded_mainloop_unlock(m_loop);
    }

Q_SIGNALS:
    void runningChanged();
    void volumeChanged();
    void mutedChanged();
    void readyChanged();
    void volumeAnnounce(int volume, bool muted);

private:
    void startPulse()
    {
        if (m_loop || !m_running.load()) {
            return;
        }

        m_loop = pa_threaded_mainloop_new();
        if (!m_loop) {
            return;
        }
        pa_mainloop_api *api = pa_threaded_mainloop_get_api(m_loop);
        m_context = pa_context_new(api, "dynamicisland-volume");
        if (!m_context) {
            pa_threaded_mainloop_free(m_loop);
            m_loop = nullptr;
            return;
        }
        pa_context_set_state_callback(m_context, &VolumeWatcher::contextStateCb, this);
        if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
            pa_context_unref(m_context);
            m_context = nullptr;
            pa_threaded_mainloop_free(m_loop);
            m_loop = nullptr;
            return;
        }
        pa_threaded_mainloop_start(m_loop);
    }

    void stopPulse()
    {
        if (!m_loop) {
            return;
        }
        pa_threaded_mainloop_stop(m_loop);
        if (m_context) {
            pa_context_disconnect(m_context);
            pa_context_unref(m_context);
            m_context = nullptr;
        }
        pa_threaded_mainloop_free(m_loop);
        m_loop = nullptr;
        m_sinkName.clear();
        m_channels = 2;
        if (m_ready.exchange(false)) {
            Q_EMIT readyChanged();
        }
    }

    void scheduleReconnect()
    {
        if (!m_running.load() || m_reconnectScheduled.exchange(true)) {
            return;
        }
        // This runs on the GUI thread after the PulseAudio callback returns.
        QMetaObject::invokeMethod(this, [this] {
            if (!m_running.load()) {
                m_reconnectScheduled.store(false);
                return;
            }
            stopPulse();
            QTimer::singleShot(3000, this, [this] {
                m_reconnectScheduled.store(false);
                if (m_running.load()) {
                    startPulse();
                }
            });
        }, Qt::QueuedConnection);
    }

    // ---- libpulse callbacks (PulseAudio thread, mainloop locked) ----
    static void contextStateCb(pa_context *c, void *userdata)
    {
        VolumeWatcher *self = static_cast<VolumeWatcher *>(userdata);
        switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
            pa_context_set_subscribe_callback(c, &VolumeWatcher::subscribeCb, self);
            {
                pa_operation *op = pa_context_subscribe(
                    c, static_cast<pa_subscription_mask_t>(
                           PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER),
                    nullptr, nullptr);
                if (op) {
                    pa_operation_unref(op);
                }
            }
            self->requestServerInfo();
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            self->scheduleReconnect();
            break;
        default:
            break;
        }
    }

    static void subscribeCb(pa_context *, pa_subscription_event_type_t t, uint32_t, void *userdata)
    {
        VolumeWatcher *self = static_cast<VolumeWatcher *>(userdata);
        const auto facility = static_cast<pa_subscription_event_type_t>(
            t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK);
        if (facility == PA_SUBSCRIPTION_EVENT_SINK || facility == PA_SUBSCRIPTION_EVENT_SERVER) {
            self->requestServerInfo();
        }
    }

    static void serverInfoCb(pa_context *c, const pa_server_info *info, void *userdata)
    {
        VolumeWatcher *self = static_cast<VolumeWatcher *>(userdata);
        if (!info || !info->default_sink_name) {
            self->publishState(0, false, false);
            return;
        }
        self->m_sinkName = info->default_sink_name;
        pa_operation *op = pa_context_get_sink_info_by_name(
            c, info->default_sink_name, &VolumeWatcher::sinkInfoCb, self);
        if (op) {
            pa_operation_unref(op);
        }
    }

    static void sinkInfoCb(pa_context *, const pa_sink_info *info, int eol, void *userdata)
    {
        VolumeWatcher *self = static_cast<VolumeWatcher *>(userdata);
        if (eol) {
            return;
        }
        if (!info) {
            self->publishState(0, false, false);
            return;
        }
        self->m_channels = info->volume.channels > 0 ? info->volume.channels : 2;
        const pa_volume_t avg = pa_cvolume_avg(&info->volume);
        int percent = static_cast<int>(qRound((avg * 100.0) / PA_VOLUME_NORM));
        percent = qBound(0, percent, 100);
        self->publishState(percent, info->mute != 0, true);
    }

    void requestServerInfo()
    {
        if (!m_context) {
            return;
        }
        pa_operation *op = pa_context_get_server_info(m_context, &VolumeWatcher::serverInfoCb, this);
        if (op) {
            pa_operation_unref(op);
        }
    }

    // Called from the PulseAudio thread. Hand the values to the GUI thread so
    // QML property notifications never cross threads.
    void publishState(int volume, bool muted, bool ready)
    {
        QMetaObject::invokeMethod(this, [this, volume, muted, ready] {
            // A queued update can arrive after stopPulse(); ignore it instead of
            // resurrecting state for a module the user just disabled.
            if (!m_running.load() || !m_loop) {
                return;
            }
            const bool wasReady = m_ready.load();
            const bool volumeChanged = m_volume.load() != volume;
            const bool mutedChanged = m_muted.load() != muted;

            m_volume.store(volume);
            m_muted.store(muted);

            if (!wasReady && ready) {
                // Publish the initial state while `ready` is still false, then
                // flip ready last. QML can therefore distinguish the initial
                // mute state from a real user change and does not announce it.
                if (volumeChanged) {
                    Q_EMIT this->volumeChanged();
                }
                if (mutedChanged) {
                    Q_EMIT this->mutedChanged();
                }
                m_ready.store(true);
                Q_EMIT this->readyChanged();
                return;
            }

            m_ready.store(ready);
            if (volumeChanged) {
                Q_EMIT this->volumeChanged();
            }
            if (mutedChanged) {
                Q_EMIT this->mutedChanged();
            }
            if (wasReady != ready) {
                Q_EMIT this->readyChanged();
            }
            if (wasReady && ready && volumeChanged) {
                Q_EMIT this->volumeAnnounce(volume, muted);
            }
        }, Qt::QueuedConnection);
    }

    pa_threaded_mainloop *m_loop = nullptr;
    pa_context *m_context = nullptr;
    std::string m_sinkName;
    int m_channels = 2;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_reconnectScheduled{false};
    std::atomic<int> m_volume{0};
    std::atomic<bool> m_muted{false};
    std::atomic<bool> m_ready{false};
};

class VolumeWatcherPlugin : public QQmlExtensionPlugin
{
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
