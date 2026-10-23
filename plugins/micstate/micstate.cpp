#include "micstate.h"

#include <QMetaObject>
#include <QTimer>

#include <pulse/subscribe.h>

namespace {

constexpr pa_subscription_mask_t kSubscribeMask =
    static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SERVER);

} // namespace

MicState::MicState(QObject *parent)
    : QObject(parent)
{
}

MicState::~MicState()
{
    stopPulse();
}

void MicState::setRunning(bool running)
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
    Q_EMIT runningChanged(running);
}

void MicState::refresh()
{
    if (!m_loop) {
        return;
    }
    pa_threaded_mainloop_lock(m_loop);
    requestStatus();
    pa_threaded_mainloop_unlock(m_loop);
}

// ---- life cycle -----------------------------------------------------------

void MicState::startPulse()
{
    if (m_loop || !m_running.load()) {
        return;
    }
    m_hasMute = false;

    m_loop = pa_threaded_mainloop_new();
    if (!m_loop) {
        return;
    }
    pa_mainloop_api *api = pa_threaded_mainloop_get_api(m_loop);
    m_context = pa_context_new(api, "dynamicisland-mic");
    if (!m_context) {
        pa_threaded_mainloop_free(m_loop);
        m_loop = nullptr;
        return;
    }
    pa_context_set_state_callback(m_context, &MicState::contextStateCb, this);
    if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        pa_context_unref(m_context);
        m_context = nullptr;
        pa_threaded_mainloop_free(m_loop);
        m_loop = nullptr;
        return;
    }
    pa_threaded_mainloop_start(m_loop);
}

void MicState::stopPulse()
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
    m_hasMute = false;
    m_hasPublishedMute = false;
    m_defaultSource.clear();
    if (m_ready.exchange(false)) {
        Q_EMIT readyChanged(false);
    }
}

void MicState::scheduleReconnect()
{
    if (!m_running.load() || m_reconnectScheduled.exchange(true)) {
        return;
    }
    // Called from the PulseAudio thread. Tear down and retry on the GUI thread;
    // this is a single-shot retry after a real failure, not a polling loop.
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

void MicState::publishMuted(bool muted)
{
    if (m_muted.exchange(muted) == muted) {
        return;
    }
    QMetaObject::invokeMethod(this, [this, muted] {
        Q_EMIT mutedChanged(muted);
    }, Qt::QueuedConnection);
}

void MicState::publishReady(bool ready)
{
    if (m_ready.exchange(ready) == ready) {
        return;
    }
    QMetaObject::invokeMethod(this, [this, ready] {
        Q_EMIT readyChanged(ready);
    }, Qt::QueuedConnection);
}

// ---- libpulse callbacks ----------------------------------------------------

void MicState::contextStateCb(pa_context *c, void *userdata)
{
    MicState *self = static_cast<MicState *>(userdata);
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
        self->onContextReady();
        break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        self->scheduleReconnect();
        break;
    default:
        break;
    }
}

void MicState::onContextReady()
{
    if (!m_context) {
        return;
    }
    pa_context_set_subscribe_callback(m_context, &MicState::subscribeCb, this);
    pa_operation *op = pa_context_subscribe(m_context, kSubscribeMask, nullptr, nullptr);
    if (op) {
        pa_operation_unref(op);
    }
    publishReady(true);
    requestStatus();
}

void MicState::subscribeCb(pa_context *c, pa_subscription_event_type_t t, uint32_t, void *userdata)
{
    MicState *self = static_cast<MicState *>(userdata);
    const auto eventType = static_cast<pa_subscription_event_type_t>(
        t & PA_SUBSCRIPTION_EVENT_TYPE_MASK);
    const auto facility = static_cast<pa_subscription_event_type_t>(
        t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK);
    if ((facility == PA_SUBSCRIPTION_EVENT_SOURCE
         && (eventType == PA_SUBSCRIPTION_EVENT_CHANGE
             || eventType == PA_SUBSCRIPTION_EVENT_NEW
             || eventType == PA_SUBSCRIPTION_EVENT_REMOVE))
        || facility == PA_SUBSCRIPTION_EVENT_SERVER) {
        self->requestStatus();
    }
}

void MicState::requestStatus()
{
    if (!m_context) {
        return;
    }
    m_hasMute = false;
    pa_operation *o = pa_context_get_server_info(m_context, &MicState::serverInfoCb, this);
    if (o) {
        pa_operation_unref(o);
    }
}

void MicState::serverInfoCb(pa_context *c, const pa_server_info *info, void *userdata)
{
    MicState *self = static_cast<MicState *>(userdata);
    if (info && info->default_source_name) {
        self->m_defaultSource = info->default_source_name;
        pa_operation *o =
            pa_context_get_source_info_by_name(c, info->default_source_name,
                                               &MicState::sourceInfoCb, self);
        if (o) {
            pa_operation_unref(o);
        }
    }
}

void MicState::sourceInfoCb(pa_context *, const pa_source_info *info, int eol, void *userdata)
{
    MicState *self = static_cast<MicState *>(userdata);
    if (eol) {
        if (self->m_hasMute) {
            self->finishRead(self->m_muteValue);
        }
        return;
    }
    if (info) {
        self->m_hasMute = true;
        self->m_muteValue = info->mute;
    }
}

void MicState::finishRead(int muteValue)
{
    const bool muted = muteValue == 1;
    if (!m_hasPublishedMute) {
        // Publish the first known value even when it equals the default
        // (unmuted), otherwise the QML side never learns the initial state and
        // the first real mute would look like a no-op.
        m_hasPublishedMute = true;
        m_muted.store(muted);
        QMetaObject::invokeMethod(this, [this, muted] {
            Q_EMIT mutedChanged(muted);
        }, Qt::QueuedConnection);
        return;
    }
    publishMuted(muted);
}
