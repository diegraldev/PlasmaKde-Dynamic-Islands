#include <QObject>
#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QSocketNotifier>
#include <QTimer>
#include <QDebug>

#include <wayland-client.h>
#include "keystate-client-protocol.h"

class KeyStateWatcher : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running WRITE setRunning NOTIFY runningChanged)
    Q_PROPERTY(bool capsLock READ capsLock NOTIFY capsLockChanged)
    Q_PROPERTY(bool numLock READ numLock NOTIFY numLockChanged)
    Q_PROPERTY(bool scrollLock READ scrollLock NOTIFY scrollLockChanged)

public:
    explicit KeyStateWatcher(QObject *parent = nullptr)
        : QObject(parent), m_running(true)
    {
        connectDisplay();
    }

    ~KeyStateWatcher()
    {
        disconnectDisplay();
    }

    bool running() const { return m_running; }
    bool capsLock() const { return m_capsLock; }
    bool numLock() const { return m_numLock; }
    bool scrollLock() const { return m_scrollLock; }

public slots:
    void setRunning(bool running)
    {
        if (m_running == running)
            return;
        m_running = running;
        if (m_running)
            connectDisplay();
        else
            disconnectDisplay();
        emit runningChanged();
    }

signals:
    void runningChanged();
    void capsLockChanged();
    void numLockChanged();
    void scrollLockChanged();

private:
    void connectDisplay()
    {
        if (m_display)
            return;

        m_display = wl_display_connect(nullptr);
        if (!m_display) {
            qInfo() << "KeyStateWatcher: failed to connect to Wayland display";
            return;
        }

        m_registry = wl_display_get_registry(m_display);
        if (!m_registry) {
            qInfo() << "KeyStateWatcher: failed to get registry";
            wl_display_disconnect(m_display);
            m_display = nullptr;
            return;
        }

        static const struct wl_registry_listener registryListener = {
            handleGlobal,
            handleGlobalRemove
        };
        wl_registry_add_listener(m_registry, &registryListener, this);

        // Wait for globals
        wl_display_roundtrip(m_display);
        wl_display_roundtrip(m_display);

        if (!m_keystate) {
            qInfo() << "KeyStateWatcher: org_kde_kwin_keystate global not found";
        } else {
            // Request initial states
            org_kde_kwin_keystate_fetchStates(m_keystate);
            wl_display_roundtrip(m_display);
        }

        if (m_display) {
            m_notifier = new QSocketNotifier(wl_display_get_fd(m_display), QSocketNotifier::Read, this);
            connect(m_notifier, &QSocketNotifier::activated, this, &KeyStateWatcher::dispatchEvents);
        }
    }

    void disconnectDisplay()
    {
        if (m_notifier) {
            m_notifier->deleteLater();
            m_notifier = nullptr;
        }
        if (m_keystate) {
            org_kde_kwin_keystate_destroy(m_keystate);
            m_keystate = nullptr;
        }
        if (m_registry) {
            wl_registry_destroy(m_registry);
            m_registry = nullptr;
        }
        if (m_display) {
            wl_display_disconnect(m_display);
            m_display = nullptr;
        }
    }

    void dispatchEvents()
    {
        if (m_display && wl_display_dispatch(m_display) < 0) {
            qInfo() << "KeyStateWatcher: Wayland display connection lost";
            disconnectDisplay();
            if (m_running)
                QTimer::singleShot(5000, this, &KeyStateWatcher::connectDisplay);
        }
    }

    static void handleGlobal(void *data, struct wl_registry *registry, uint32_t name,
                             const char *interface, uint32_t version)
    {
        KeyStateWatcher *self = static_cast<KeyStateWatcher *>(data);
        if (strcmp(interface, "org_kde_kwin_keystate") == 0) {
            self->m_keystate = static_cast<struct org_kde_kwin_keystate *>(
                wl_registry_bind(registry, name, &org_kde_kwin_keystate_interface, qMin<uint32_t>(version, 5)));
            org_kde_kwin_keystate_add_listener(self->m_keystate, &keystateListener, self);
        }
    }

    static void handleGlobalRemove(void *data, struct wl_registry *registry, uint32_t name)
    {
        KeyStateWatcher *self = static_cast<KeyStateWatcher *>(data);
        Q_UNUSED(registry)
        Q_UNUSED(name)
        if (self->m_keystate) {
            org_kde_kwin_keystate_destroy(self->m_keystate);
            self->m_keystate = nullptr;
        }
    }

    static void handleStateChanged(void *data, struct org_kde_kwin_keystate *,
                                   uint32_t key, uint32_t state)
    {
        KeyStateWatcher *self = static_cast<KeyStateWatcher *>(data);
        bool active = (state == ORG_KDE_KWIN_KEYSTATE_STATE_LOCKED);
        switch (key) {
        case ORG_KDE_KWIN_KEYSTATE_KEY_CAPSLOCK:
            if (self->m_capsLock != active) {
                self->m_capsLock = active;
                emit self->capsLockChanged();
            }
            break;
        case ORG_KDE_KWIN_KEYSTATE_KEY_NUMLOCK:
            if (self->m_numLock != active) {
                self->m_numLock = active;
                emit self->numLockChanged();
            }
            break;
        case ORG_KDE_KWIN_KEYSTATE_KEY_SCROLLLOCK:
            if (self->m_scrollLock != active) {
                self->m_scrollLock = active;
                emit self->scrollLockChanged();
            }
            break;
        default:
            break;
        }
    }

    static const struct org_kde_kwin_keystate_listener keystateListener;

    struct wl_display *m_display = nullptr;
    struct wl_registry *m_registry = nullptr;
    struct org_kde_kwin_keystate *m_keystate = nullptr;
    QSocketNotifier *m_notifier = nullptr;
    bool m_running = true;
    bool m_capsLock = false;
    bool m_numLock = false;
    bool m_scrollLock = false;
};

const struct org_kde_kwin_keystate_listener KeyStateWatcher::keystateListener = {
    handleStateChanged
};

#include "keystate-client-protocol.c"

class KeyStatePlugin : public QQmlExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QQmlExtensionInterface")

public:
    void registerTypes(const char *uri) override
    {
        Q_UNUSED(uri)
        qmlRegisterType<KeyStateWatcher>(uri, 1, 0, "KeyStateWatcher");
    }
};

#include "keystatewatcher.moc"