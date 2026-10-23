#pragma once

#include <QObject>

#include <pulse/pulseaudio.h>

#include <atomic>
#include <string>

// Event-driven microphone mute watcher.
//
// Uses a PulseAudio threaded mainloop and subscribes to SOURCE changes. The
// default source is resolved via server info, then its mute flag is read from
// the source info list. State changes are marshalled to the main (QML) thread
// with queued invocations, so the widget does zero polling and sleeps idle.
class MicState : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)

public:
    explicit MicState(QObject *parent = nullptr);
    ~MicState() override;

    bool muted() const { return m_muted.load(); }
    bool ready() const { return m_ready.load(); }
    bool running() const { return m_running.load(); }
    void setRunning(bool running);

public Q_SLOTS:
    void refresh();

Q_SIGNALS:
    void mutedChanged(bool muted);
    void readyChanged(bool ready);
    void runningChanged(bool running);

private:
    void startPulse();
    void stopPulse();
    void scheduleReconnect();
    void publishMuted(bool muted);
    void publishReady(bool ready);

    // ---- libpulse callbacks (run on the PulseAudio thread) ----
    static void contextStateCb(pa_context *c, void *userdata);
    static void serverInfoCb(pa_context *c, const pa_server_info *info, void *userdata);
    static void sourceInfoCb(pa_context *c, const pa_source_info *info, int eol, void *userdata);
    static void subscribeCb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata);

    void onContextReady();
    void requestStatus();
    void finishRead(int muteValue);

    pa_threaded_mainloop *m_loop = nullptr;
    pa_context *m_context = nullptr;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_muted{false};
    std::atomic<bool> m_reconnectScheduled{false};
    std::string m_defaultSource;
    bool m_hasMute = false;
    bool m_hasPublishedMute = false;
    int m_muteValue = 0;
};
