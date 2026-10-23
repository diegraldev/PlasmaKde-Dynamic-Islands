#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QTimer>
#include <QVariantList>

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

// Event-driven audio spectrum for the compact media capsule.
//
// It records the default sink's monitor source through libpulse and exposes
// five smoothed band levels. The stream exists only while `running` is true
// (QML ties it to media playback), so idle cost is zero and there is no
// subprocess, no polling timer and no file recording.
class AudioLevels : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(QVariantList levels READ levels NOTIFY levelsChanged)

public:
    explicit AudioLevels(QObject *parent = nullptr)
        : QObject(parent)
        , m_levels{0.0, 0.0, 0.0, 0.0, 0.0}
    {
        m_emitTimer.start();
    }

    ~AudioLevels() override
    {
        // QML may already be tearing down its bindings. Do not emit
        // levelsChanged/readyChanged from the destructor; stop the PulseAudio
        // thread first and leave the document alone.
        m_destroying = true;
        m_running.store(false);
        stopPulse();
    }

    bool running() const { return m_running.load(); }
    bool ready() const { return m_ready.load(); }
    QVariantList levels() const { return m_levels; }

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

Q_SIGNALS:
    void runningChanged();
    void readyChanged();
    void levelsChanged();

private:
    static constexpr int kSampleCount = 1024;
    static constexpr int kBandCount = 5;

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
        m_context = pa_context_new(api, "dynamicisland-levels");
        if (!m_context) {
            pa_threaded_mainloop_free(m_loop);
            m_loop = nullptr;
            return;
        }
        pa_context_set_state_callback(m_context, &AudioLevels::contextStateCb, this);
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
        if (m_stream) {
            pa_stream_disconnect(m_stream);
            pa_stream_unref(m_stream);
            m_stream = nullptr;
        }
        if (m_context) {
            pa_context_disconnect(m_context);
            pa_context_unref(m_context);
            m_context = nullptr;
        }
        pa_threaded_mainloop_free(m_loop);
        m_loop = nullptr;
        m_monitorSource.clear();
        m_samples.clear();
        m_smoothed.fill(0.0f);
        if (m_ready.exchange(false) && !m_destroying) {
            Q_EMIT readyChanged();
        }
        if (!m_destroying) {
            setLevels({0.0, 0.0, 0.0, 0.0, 0.0});
        }
    }

    void scheduleReconnect()
    {
        if (!m_running.load() || m_reconnectScheduled.exchange(true)) {
            return;
        }
        QMetaObject::invokeMethod(this, [this] {
            if (m_destroying || !m_running.load()) {
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
        AudioLevels *self = static_cast<AudioLevels *>(userdata);
        switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
            pa_context_set_subscribe_callback(c, &AudioLevels::subscribeCb, self);
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
        AudioLevels *self = static_cast<AudioLevels *>(userdata);
        const auto facility = static_cast<pa_subscription_event_type_t>(
            t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK);
        if (facility == PA_SUBSCRIPTION_EVENT_SINK || facility == PA_SUBSCRIPTION_EVENT_SERVER) {
            self->requestServerInfo();
        }
    }

    static void serverInfoCb(pa_context *c, const pa_server_info *info, void *userdata)
    {
        AudioLevels *self = static_cast<AudioLevels *>(userdata);
        if (!info || !info->default_sink_name) {
            self->publishReady(false);
            return;
        }
        pa_operation *op = pa_context_get_sink_info_by_name(
            c, info->default_sink_name, &AudioLevels::sinkInfoCb, self);
        if (op) {
            pa_operation_unref(op);
        }
    }

    static void sinkInfoCb(pa_context *, const pa_sink_info *info, int eol, void *userdata)
    {
        AudioLevels *self = static_cast<AudioLevels *>(userdata);
        if (eol) {
            return;
        }
        if (!info || !info->monitor_source_name) {
            self->publishReady(false);
            return;
        }
        self->ensureStream(info->monitor_source_name);
    }

    static void streamStateCb(pa_stream *stream, void *userdata)
    {
        AudioLevels *self = static_cast<AudioLevels *>(userdata);
        switch (pa_stream_get_state(stream)) {
        case PA_STREAM_READY:
            self->publishReady(true);
            break;
        case PA_STREAM_FAILED:
        case PA_STREAM_TERMINATED:
            self->publishReady(false);
            self->scheduleReconnect();
            break;
        default:
            break;
        }
    }

    static void readCb(pa_stream *stream, size_t, void *userdata)
    {
        AudioLevels *self = static_cast<AudioLevels *>(userdata);
        // Drain what is currently queued, but do not call
        // pa_stream_readable_size() from inside the read callback. libpulse
        // already tells us when data is ready; peeking once per queued chunk
        // keeps the callback simple and avoids a lazy-PLT issue observed while
        // the stream was being torn down.
        for (int guard = 0; guard < 32; ++guard) {
            const void *data = nullptr;
            size_t bytes = 0;
            if (pa_stream_peek(stream, &data, &bytes) < 0) {
                break;
            }
            if (!data && bytes == 0) {
                break;
            }
            if (data && bytes >= sizeof(float)) {
                self->processSamples(static_cast<const float *>(data), bytes / sizeof(float));
            }
            pa_stream_drop(stream);
        }
    }

    void requestServerInfo()
    {
        if (!m_context) {
            return;
        }
        pa_operation *op = pa_context_get_server_info(m_context, &AudioLevels::serverInfoCb, this);
        if (op) {
            pa_operation_unref(op);
        }
    }

    void ensureStream(const char *source)
    {
        if (!source || !*source) {
            publishReady(false);
            return;
        }
        if (m_stream && m_monitorSource == source) {
            return;
        }
        if (m_stream) {
            pa_stream_disconnect(m_stream);
            pa_stream_unref(m_stream);
            m_stream = nullptr;
        }
        m_monitorSource = source;
        m_samples.clear();
        m_smoothed.fill(0.0f);

        pa_sample_spec spec;
        spec.format = PA_SAMPLE_FLOAT32LE;
        spec.rate = 44100;
        spec.channels = 1;
        m_stream = pa_stream_new(m_context, "dynamicisland-levels", &spec, nullptr);
        if (!m_stream) {
            publishReady(false);
            return;
        }
        pa_stream_set_state_callback(m_stream, &AudioLevels::streamStateCb, this);
        pa_stream_set_read_callback(m_stream, &AudioLevels::readCb, this);
        // Keep the monitor latency close to one analysis window (~23 ms). The
        // default record fragsize can be seconds long, which makes the bars
        // look frozen even though they are reading real audio.
        pa_buffer_attr attr = {};
        attr.maxlength = 16384;
        attr.fragsize = 4096;
        attr.tlength = static_cast<uint32_t>(-1);
        attr.prebuf = static_cast<uint32_t>(-1);
        attr.minreq = static_cast<uint32_t>(-1);
        if (pa_stream_connect_record(m_stream, source, &attr, PA_STREAM_ADJUST_LATENCY) < 0) {
            pa_stream_unref(m_stream);
            m_stream = nullptr;
            publishReady(false);
        }
    }

    void processSamples(const float *data, size_t count)
    {
        m_samples.insert(m_samples.end(), data, data + count);
        while (m_samples.size() >= static_cast<size_t>(kSampleCount)) {
            std::array<float, kBandCount> bands;
            computeBands(m_samples.data(), bands);
            for (int i = 0; i < kBandCount; ++i) {
                const float target = bands[i];
                const float old = m_smoothed[i];
                // Fast attack, slow release: the bars jump on transients and
                // settle naturally instead of flickering at 60 Hz.
                m_smoothed[i] = target > old ? old + (target - old) * 0.65f
                                             : old + (target - old) * 0.16f;
            }
            publishLevels();
            m_samples.erase(m_samples.begin(), m_samples.begin() + kSampleCount);
        }
    }

    void computeBands(const float *samples, std::array<float, kBandCount> &out) const
    {
        // Three probes per visual band. A single probe per band makes a pure
        // note that falls between two probes nearly invisible; taking the
        // strongest probe keeps the display honest without a full FFT.
        static constexpr std::array<std::array<float, 3>, kBandCount> frequencies = {{
            {{60.0f, 90.0f, 120.0f}},
            {{150.0f, 220.0f, 300.0f}},
            {{350.0f, 440.0f, 550.0f}},
            {{700.0f, 880.0f, 1100.0f}},
            {{1400.0f, 1800.0f, 2400.0f}}
        }};
        static constexpr float pi = 3.14159265358979323846f;
        static const std::array<float, kSampleCount> window = [] {
            std::array<float, kSampleCount> values{};
            for (int i = 0; i < kSampleCount; ++i) {
                values[i] = 0.5f - 0.5f * std::cos(2.0f * pi * i / (kSampleCount - 1));
            }
            return values;
        }();

        for (int band = 0; band < kBandCount; ++band) {
            float best = 0.0f;
            for (float frequency : frequencies[band]) {
                const float omega = 2.0f * pi * frequency / 44100.0f;
                const float coeff = 2.0f * std::cos(omega);
                float s0 = 0.0f;
                float s1 = 0.0f;
                float s2 = 0.0f;
                for (int i = 0; i < kSampleCount; ++i) {
                    s0 = window[i] * samples[i] + coeff * s1 - s2;
                    s2 = s1;
                    s1 = s0;
                }
                const float power = std::max(0.0f, s1 * s1 + s2 * s2 - coeff * s1 * s2);
                best = std::max(best, std::sqrt(power) / kSampleCount);
            }
            const float db = 20.0f * std::log10(best + 1.0e-7f);
            out[band] = std::clamp((db + 58.0f) / 58.0f, 0.0f, 1.0f);
        }
    }

    void publishLevels()
    {
        if (m_emitTimer.elapsed() < 33) {
            return;
        }
        m_emitTimer.restart();
        std::array<float, kBandCount> levels = m_smoothed;
        QMetaObject::invokeMethod(this, [this, levels] {
            if (m_destroying || !m_running.load() || !m_loop) {
                return;
            }
            setLevels(levels);
        }, Qt::QueuedConnection);
    }

    void publishReady(bool ready)
    {
        QMetaObject::invokeMethod(this, [this, ready] {
            if (m_destroying || !m_running.load() || !m_loop) {
                return;
            }
            if (m_ready.load() != ready) {
                m_ready.store(ready);
                Q_EMIT readyChanged();
            }
        }, Qt::QueuedConnection);
    }

    void setLevels(const std::array<float, kBandCount> &levels)
    {
        QVariantList list;
        list.reserve(kBandCount);
        for (float level : levels) {
            list.append(level);
        }
        m_levels = list;
        Q_EMIT levelsChanged();
    }

    pa_threaded_mainloop *m_loop = nullptr;
    pa_context *m_context = nullptr;
    pa_stream *m_stream = nullptr;
    std::string m_monitorSource;
    std::vector<float> m_samples;
    std::array<float, kBandCount> m_smoothed{};
    QVariantList m_levels;
    QElapsedTimer m_emitTimer;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_reconnectScheduled{false};
    bool m_destroying = false;
};

class AudioLevelsPlugin : public QQmlExtensionPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QQmlExtensionInterface")

public:
    void registerTypes(const char *uri) override
    {
        qmlRegisterType<AudioLevels>(uri, 1, 0, "AudioLevels");
    }
};

#include "audiolevels.moc"
