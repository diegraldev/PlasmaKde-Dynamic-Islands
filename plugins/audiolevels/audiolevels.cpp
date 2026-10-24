#include <QByteArray>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QTimer>
#include <QString>
#include <QVariantList>

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <string>
#include <vector>

// Event-driven audio spectrum for the compact media capsule.
//
// It records the active MPRIS player's sink input through libpulse when the
// player identity can be matched, and falls back to the default sink monitor.
// The five bands are normalized against the broadband level, so the display
// follows the spectral shape rather than the player's volume. The stream
// exists only while `running` is true (QML ties it to media playback), so idle
// cost is zero and there is no subprocess, no polling timer and no recording.
class AudioLevels : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(QVariantList levels READ levels NOTIFY levelsChanged)
    Q_PROPERTY(QVariantList peaks READ peaks NOTIFY levelsChanged)
    Q_PROPERTY(QString mediaIdentity READ mediaIdentity WRITE setMediaIdentity NOTIFY mediaIdentityChanged)

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
    QVariantList peaks() const { return m_peaksList; }
    QString mediaIdentity() const { return m_mediaIdentity; }

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

    void setMediaIdentity(const QString &identity)
    {
        if (m_mediaIdentity == identity) {
            return;
        }
        // The identity is read from the PulseAudio thread while it resolves
        // sink inputs, so update it under the threaded-mainloop lock when the
        // connection is alive.
        if (m_loop) {
            pa_threaded_mainloop_lock(m_loop);
            m_mediaIdentity = identity;
            pa_threaded_mainloop_unlock(m_loop);
        } else {
            m_mediaIdentity = identity;
        }
        Q_EMIT mediaIdentityChanged();
        requestRefresh();
    }

Q_SIGNALS:
    void runningChanged();
    void readyChanged();
    void levelsChanged();
    void mediaIdentityChanged();

private:
    static constexpr int kFftSize = 2048;
    static constexpr int kHopSize = 1024;
    static constexpr int kBandCount = 5;

    struct BandRange {
        float low;
        float high;
    };

    // Contiguous, roughly equal-octave bands. Covering the whole range (instead
    // of six sparse probes) means no tone can fall between two probes, and the
    // FFT gives a stable spectral estimate for every band.
    static constexpr std::array<BandRange, kBandCount> kBandRanges = {{
        {40.0f, 160.0f},
        {160.0f, 500.0f},
        {500.0f, 1600.0f},
        {1600.0f, 5000.0f},
        {5000.0f, 16000.0f}
    }};
    // Fixed calibration measured on real music with the FFT analyser. The
    // centres follow the natural roll-off of a mix, and the per-band spans
    // compensate for the wider dynamic range of the treble bands, so every bar
    // moves across most of its height instead of the upper bands sitting at 0.
    static constexpr std::array<float, kBandCount> kBandCenters = {
        -17.0f, -21.5f, -33.0f, -43.5f, -56.5f
    };
    static constexpr std::array<float, kBandCount> kBandSpans = {
        20.0f, 22.0f, 32.0f, 36.0f, 44.0f
    };
    // Slow per-band reference (about 30 s) with a bounded correction, so a very
    // bass-heavy or very bright track still fills the bars without flattening
    // the difference between bands.
    static constexpr float kReferenceAlpha = 0.0008f;
    static constexpr float kReferenceClampDb = 8.0f;

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
        m_sinkInputIndex = PA_INVALID_INDEX;
        m_targetSinkInputIndex = PA_INVALID_INDEX;
        m_samples.clear();
        m_smoothed.fill(0.0f);
        m_peaks.fill(0.0f);
        m_reference = kBandCenters;
        m_referenceReady = false;
        if (m_ready.exchange(false) && !m_destroying) {
            Q_EMIT readyChanged();
        }
        if (!m_destroying) {
            setLevels({0.0, 0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0, 0.0});
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
                           PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER
                           | PA_SUBSCRIPTION_MASK_SINK_INPUT),
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
        if (facility == PA_SUBSCRIPTION_EVENT_SINK
            || facility == PA_SUBSCRIPTION_EVENT_SERVER
            || facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
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
        self->m_defaultSinkName = info->default_sink_name;
        self->requestSinkInputList(c);
    }

    static void sinkInputInfoCb(pa_context *c, const pa_sink_input_info *info, int eol, void *userdata)
    {
        AudioLevels *self = static_cast<AudioLevels *>(userdata);
        if (eol) {
            if (self->m_matchedSinkInputIndex != PA_INVALID_INDEX) {
                self->m_targetSinkInputIndex = self->m_matchedSinkInputIndex;
                pa_operation *op = pa_context_get_sink_info_by_index(
                    c, self->m_matchedSinkIndex, &AudioLevels::sinkInfoCb, self);
                if (op) {
                    pa_operation_unref(op);
                }
            } else {
                self->requestDefaultSinkInfo(c);
            }
            return;
        }
        if (!info || !self->matchesIdentity(info)) {
            return;
        }
        // Prefer a running stream over a corked one when an application has
        // more than one sink input.
        if (self->m_matchedSinkInputIndex == PA_INVALID_INDEX || (self->m_matchedCorked && !info->corked)) {
            self->m_matchedSinkInputIndex = info->index;
            self->m_matchedSinkIndex = info->sink;
            self->m_matchedCorked = info->corked != 0;
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
        self->ensureStream(info->monitor_source_name, self->m_targetSinkInputIndex);
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

    void requestRefresh()
    {
        if (!m_loop || !m_context) {
            return;
        }
        pa_threaded_mainloop_lock(m_loop);
        requestServerInfo();
        pa_threaded_mainloop_unlock(m_loop);
    }

    // Called with the threaded mainloop locked.
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

    void requestSinkInputList(pa_context *c)
    {
        m_matchedSinkInputIndex = PA_INVALID_INDEX;
        m_matchedSinkIndex = PA_INVALID_INDEX;
        m_matchedCorked = true;
        if (normalizedIdentity(m_mediaIdentity).isEmpty()) {
            requestDefaultSinkInfo(c);
            return;
        }
        pa_operation *op = pa_context_get_sink_input_info_list(
            c, &AudioLevels::sinkInputInfoCb, this);
        if (op) {
            pa_operation_unref(op);
        }
    }

    void requestDefaultSinkInfo(pa_context *c)
    {
        m_targetSinkInputIndex = PA_INVALID_INDEX;
        if (m_defaultSinkName.isEmpty()) {
            publishReady(false);
            return;
        }
        pa_operation *op = pa_context_get_sink_info_by_name(
            c, m_defaultSinkName.constData(), &AudioLevels::sinkInfoCb, this);
        if (op) {
            pa_operation_unref(op);
        }
    }

    static QString normalizedIdentity(const QString &value)
    {
        QString out;
        out.reserve(value.size());
        for (const QChar ch : value) {
            if (ch.isLetterOrNumber()) {
                out.append(ch.toLower());
            }
        }
        return out;
    }

    bool matchesIdentity(const pa_sink_input_info *info) const
    {
        if (!info || !info->proplist) {
            return false;
        }
        const QString wanted = normalizedIdentity(m_mediaIdentity);
        if (wanted.isEmpty()) {
            return false;
        }
        const auto matchesKey = [&wanted](const char *key, pa_proplist *proplist) {
            const char *value = pa_proplist_gets(proplist, key);
            if (!value || !*value) {
                return false;
            }
            const QString normalized = normalizedIdentity(QString::fromUtf8(value));
            return !normalized.isEmpty()
                && (normalized == wanted || normalized.contains(wanted)
                    || wanted.contains(normalized));
        };
        return matchesKey(PA_PROP_APPLICATION_NAME, info->proplist)
            || matchesKey(PA_PROP_APPLICATION_PROCESS_BINARY, info->proplist)
            || matchesKey(PA_PROP_MEDIA_NAME, info->proplist);
    }

    void ensureStream(const char *source, uint32_t sinkInputIndex)
    {
        if (!source || !*source) {
            publishReady(false);
            return;
        }
        if (m_stream && m_monitorSource == source && m_sinkInputIndex == sinkInputIndex) {
            return;
        }
        if (m_stream) {
            pa_stream_disconnect(m_stream);
            pa_stream_unref(m_stream);
            m_stream = nullptr;
        }
        m_monitorSource = source;
        m_sinkInputIndex = sinkInputIndex;
        m_samples.clear();
        m_smoothed.fill(0.0f);
        m_peaks.fill(0.0f);
        m_reference = kBandCenters;
        m_referenceReady = false;

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
        if (m_sinkInputIndex != PA_INVALID_INDEX) {
            // Capture only the active media player's sink input instead of the
            // whole default-sink mix. If the server rejects the monitor-stream
            // hint, keep the general monitor as a safe fallback.
            if (pa_stream_set_monitor_stream(m_stream, m_sinkInputIndex) < 0) {
                m_sinkInputIndex = PA_INVALID_INDEX;
            }
        }
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
        // 2048-sample FFT with a 1024-sample hop (50 % overlap): enough bass
        // resolution for the 40-160 Hz band while keeping the latency low.
        while (m_samples.size() >= static_cast<size_t>(kFftSize)) {
            std::array<float, kBandCount> bands;
            computeBands(m_samples.data(), bands);

            float sumSquares = 0.0f;
            for (int i = 0; i < kFftSize; ++i) {
                sumSquares += m_samples[i] * m_samples[i];
            }
            const float overallRms = std::sqrt(sumSquares / kFftSize);
            const float overallDb = 20.0f * std::log10(overallRms + 1.0e-7f);
            // Only the overall level decides whether the player is silent.
            // It must not scale the bars: the band values below are relative
            // to the overall level, so 10% and 100% volume look the same.
            // Keep the gate well below normal listening levels, so lowering
            // the player volume does not shrink the bars. It only mutes them
            // when the stream is effectively silent.
            const float activity = std::clamp((overallDb + 85.0f) / 20.0f, 0.0f, 1.0f);

            // Adapt the per-band reference only on normal-level audio, so a
            // quiet intro cannot pull the reference down and make the next
            // loud section saturate.
            const bool adaptReference = overallDb > -50.0f;
            if (adaptReference && !m_referenceReady) {
                m_reference = kBandCenters;
                m_referenceReady = true;
            }

            for (int i = 0; i < kBandCount; ++i) {
                const float bandDb = 20.0f * std::log10(bands[i] + 1.0e-7f);
                // Subtract the broadband level: this is the spectral shape,
                // independent of the player's volume. A very loud bass-heavy
                // track and the same track at 20% produce the same values.
                const float relativeDb = bandDb - overallDb;
                if (adaptReference) {
                    m_reference[i] += (relativeDb - m_reference[i]) * kReferenceAlpha;
                    m_reference[i] = std::clamp(m_reference[i],
                                                kBandCenters[i] - kReferenceClampDb,
                                                kBandCenters[i] + kReferenceClampDb);
                }
                const float reference = m_referenceReady ? m_reference[i] : kBandCenters[i];
                // Per-band spans keep every bar lively: the treble bands have a
                // much wider natural dynamic range than the bass, so a single
                // span would leave them near zero on ordinary music.
                const float normalized = std::clamp(
                    0.5f + (relativeDb - reference) / kBandSpans[i], 0.0f, 1.0f);
                const float target = normalized * activity;
                const float old = m_smoothed[i];
                // Fast attack, slower release with a constant gravity term.
                // The gravity guarantees that a bar keeps falling even when it
                // sits close to its target, which is what gives Cava-style
                // meters their natural, non-sticky decay.
                float next = old;
                if (target > old) {
                    next = old + (target - old) * 0.62f;
                } else {
                    next = old - (old - target) * 0.22f;
                    next = std::min(next, old - 0.008f);
                    next = std::max(next, target);
                }
                m_smoothed[i] = std::clamp(next, 0.0f, 1.0f);

                // Peak-hold cap: snap up with the bar, then fall on its own.
                if (m_smoothed[i] >= m_peaks[i]) {
                    m_peaks[i] = m_smoothed[i];
                } else {
                    m_peaks[i] = std::max(m_smoothed[i], m_peaks[i] - 0.008f);
                }
            }
            publishLevels();
            m_samples.erase(m_samples.begin(), m_samples.begin() + kHopSize);
        }
    }

    // In-place iterative radix-2 FFT.
    static void fft(std::vector<std::complex<float>> &a)
    {
        const int n = static_cast<int>(a.size());
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) {
                j ^= bit;
            }
            j ^= bit;
            if (i < j) {
                std::swap(a[i], a[j]);
            }
        }
        static const std::array<std::complex<float>, kFftSize / 2> twiddles = [] {
            std::array<std::complex<float>, kFftSize / 2> table{};
            constexpr float pi = 3.14159265358979323846f;
            for (int i = 0; i < kFftSize / 2; ++i) {
                const float angle = -2.0f * pi * i / kFftSize;
                table[i] = std::complex<float>(std::cos(angle), std::sin(angle));
            }
            return table;
        }();
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len / 2;
            const int step = n / len;
            for (int i = 0; i < n; i += len) {
                for (int j = 0; j < half; ++j) {
                    const std::complex<float> u = a[i + j];
                    const std::complex<float> v = a[i + j + half] * twiddles[j * step];
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
            }
        }
    }

    void computeBands(const float *samples, std::array<float, kBandCount> &out)
    {
        static const std::array<float, kFftSize> window = [] {
            std::array<float, kFftSize> values{};
            constexpr float pi = 3.14159265358979323846f;
            for (int i = 0; i < kFftSize; ++i) {
                values[i] = 0.5f - 0.5f * std::cos(2.0f * pi * i / (kFftSize - 1));
            }
            return values;
        }();

        for (int i = 0; i < kFftSize; ++i) {
            m_fft[i] = std::complex<float>(window[i] * samples[i], 0.0f);
        }
        fft(m_fft);

        // Hann window power gain: mean(w^2) = 3/8. The mean power per bin is
        // used instead of the total band power so a wider treble band does not
        // automatically outrank the narrow bass band.
        constexpr float windowPower = 0.375f;
        constexpr float sampleRate = 44100.0f;
        constexpr double fftScale = static_cast<double>(kFftSize) * kFftSize;

        for (int band = 0; band < kBandCount; ++band) {
            int low = static_cast<int>(std::ceil(kBandRanges[band].low * kFftSize / sampleRate));
            int high = static_cast<int>(std::floor(kBandRanges[band].high * kFftSize / sampleRate));
            low = std::max(1, low);
            high = std::min(kFftSize / 2 - 1, high);
            if (high < low) {
                high = low;
            }
            double power = 0.0;
            for (int k = low; k <= high; ++k) {
                power += std::norm(m_fft[k]);
            }
            const int bins = high - low + 1;
            const double meanPower = power / fftScale / windowPower / bins;
            out[band] = static_cast<float>(std::sqrt(meanPower));
        }
    }

    void publishLevels()
    {
        if (m_emitTimer.elapsed() < 33) {
            return;
        }
        m_emitTimer.restart();
        const std::array<float, kBandCount> levels = m_smoothed;
        const std::array<float, kBandCount> peaks = m_peaks;
        QMetaObject::invokeMethod(this, [this, levels, peaks] {
            if (m_destroying || !m_running.load() || !m_loop) {
                return;
            }
            setLevels(levels, peaks);
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

    void setLevels(const std::array<float, kBandCount> &levels,
                   const std::array<float, kBandCount> &peaks)
    {
        QVariantList levelList;
        QVariantList peakList;
        levelList.reserve(kBandCount);
        peakList.reserve(kBandCount);
        for (int i = 0; i < kBandCount; ++i) {
            levelList.append(levels[i]);
            peakList.append(peaks[i]);
        }
        m_levels = levelList;
        m_peaksList = peakList;
        Q_EMIT levelsChanged();
    }

    pa_threaded_mainloop *m_loop = nullptr;
    pa_context *m_context = nullptr;
    pa_stream *m_stream = nullptr;
    std::string m_monitorSource;
    uint32_t m_sinkInputIndex = PA_INVALID_INDEX;
    uint32_t m_targetSinkInputIndex = PA_INVALID_INDEX;
    uint32_t m_matchedSinkInputIndex = PA_INVALID_INDEX;
    uint32_t m_matchedSinkIndex = PA_INVALID_INDEX;
    bool m_matchedCorked = true;
    QByteArray m_defaultSinkName;
    QString m_mediaIdentity;
    std::vector<float> m_samples;
    std::vector<std::complex<float>> m_fft = std::vector<std::complex<float>>(kFftSize);
    std::array<float, kBandCount> m_smoothed{};
    std::array<float, kBandCount> m_peaks{};
    std::array<float, kBandCount> m_reference{};
    bool m_referenceReady = false;
    QVariantList m_levels;
    QVariantList m_peaksList;
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
