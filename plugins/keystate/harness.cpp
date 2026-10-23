#include <QCoreApplication>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QObject>
#include <QDebug>
#include <QTimer>
#include <cstdio>

int main(int argc, char *argv[])
{
    printf("=== MAIN START ===\n");
    fflush(stdout);

    QCoreApplication app(argc, argv);
    printf("=== QCoreApplication created ===\n");
    fflush(stdout);

    QQmlEngine engine;
    engine.addImportPath("/home/diegral/.local/share/qml");
    printf("=== Engine created, import paths: %s ===\n", 
           qPrintable(engine.importPathList().join(",")));
    fflush(stdout);

    QQmlComponent component(&engine);
    component.setData(
        "import KeyState 1.0\n"
        "KeyStateWatcher { id: watcher; running: true }",
        QUrl());

    printf("=== Component loading... ===\n");
    fflush(stdout);
    QObject *obj = component.create();
    if (component.isError()) {
        printf("=== QML Error: %s ===\n", qPrintable(component.errors().first().toString()));
        fflush(stdout);
        return 1;
    }
    printf("=== Component created ===\n");
    fflush(stdout);

    QObject *watcher = obj->findChild<QObject *>("watcher");
    if (!watcher) watcher = obj;
    printf("=== Watcher: %p class=%s ===\n", watcher, watcher->metaObject()->className());
    fflush(stdout);

    // Dump properties
    const QMetaObject *mo = watcher->metaObject();
    for (int i = mo->propertyOffset(); i < mo->propertyCount(); ++i) {
        QMetaProperty prop = mo->property(i);
        printf("  Property: %s = %s\n", prop.name(), qPrintable(watcher->property(prop.name()).toString()));
    }
    fflush(stdout);

    bool lastCaps = watcher->property("capsLock").toBool();
    bool lastNum = watcher->property("numLock").toBool();
    bool lastScroll = watcher->property("scrollLock").toBool();

    printf("Initial caps: %d num: %d scroll: %d\n", lastCaps, lastNum, lastScroll);
    fflush(stdout);

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        bool caps = watcher->property("capsLock").toBool();
        bool num = watcher->property("numLock").toBool();
        bool scroll = watcher->property("scrollLock").toBool();
        if (caps != lastCaps) {
            printf("CAPS LOCK CHANGED: %s\n", caps ? "ON" : "OFF");
            lastCaps = caps;
        }
        if (num != lastNum) {
            printf("NUM LOCK CHANGED: %s\n", num ? "ON" : "OFF");
            lastNum = num;
        }
        if (scroll != lastScroll) {
            printf("SCROLL LOCK CHANGED: %s\n", scroll ? "ON" : "OFF");
            lastScroll = scroll;
        }
        fflush(stdout);
    });
    timer.start(200);

    QTimer::singleShot(5000, &app, &QCoreApplication::quit);

    printf("=== Starting event loop ===\n");
    fflush(stdout);
    return app.exec();
}