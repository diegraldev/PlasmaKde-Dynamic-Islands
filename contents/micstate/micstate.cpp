#include "micstate.h"

#include <pulse/subscribe.h>

namespace {

constexpr pa_subscription_mask_t kSubscribeMask =
    static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SOURCE);

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
    if (m_running == running) {
        return;
    }
    m_running = running;
    if (m_running) {
        startPulse();
    } else {
        stopPulse();
    }
    Q_EMIT runningChanged(m_running);
}

void MicState::refresh()
{
    if (m_context) {
        requestStatus();
    }
}

// ---- life cycle -----------------------------------------------------------

void MicState::startPulse()
{
    if (m_loop) {
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
    pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr);
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
    if (m_ready) {
        m_ready = false;
        Q_EMIT readyChanged(false);
    }
}

// ---- libpulse callbacks ----------------------------------------------------

void MicState::contextStateCb(pa_context *c, void *userdata)
{
    MicState *self = static_cast<MicState *>(userdata);
    pa_context_state_t state = pa_context_get_state(c);
    switch (state) {
    case PA_CONTEXT_READY:
        self->onContextReady();
        break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        // Server disappeared or was never there. Stay quiet; the running
        // property is unchanged so the watcher can come back later.
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
    pa_context_subscribe(m_context, kSubscribeMask, nullptr, nullptr);
    if (!m_ready) {
        m_ready = true;
        Q_EMIT readyChanged(true);
    }
    requestStatus();
}

void MicState::subscribeCb(pa_context *c, pa_subscription_event_type_t t, uint32_t, void *userdata)
{
    MicState *self = static_cast<MicState *>(userdata);
    pa_subscription_event_type_t event_type = static_cast<pa_subscription_event_type_t>(
        t & PA_SUBSCRIPTION_EVENT_TYPE_MASK);
    pa_subscription_event_type_t facility = static_cast<pa_subscription_event_type_t>(
        t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK);
    if (facility == PA_SUBSCRIPTION_EVENT_SOURCE
        && event_type == PA_SUBSCRIPTION_EVENT_CHANGE) {
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
            self->finishRead(self->m_defaultSource, self->m_muteValue, true);
        }
        return;
    }
    if (info) {
        self->m_hasMute = true;
        self->m_muteValue = info->mute;
    }
}

void MicState::finishRead(std::string, int muteValue, bool)
{
    bool muted = muteValue == 1;
    if (m_muted != muted) {
        m_muted = muted;
        Q_EMIT mutedChanged(m_muted);
    }
}
