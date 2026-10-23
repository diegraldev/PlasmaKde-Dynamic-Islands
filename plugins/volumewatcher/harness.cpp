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

    QQmlEngine engine;
    engine.addImportPath("/home/diegral/.local/share/qml");

    QQmlComponent component(&engine);
    component.setData(
        "import VolumeWatcher 1.0\n"
        "VolumeWatcher { id: vw; running: true }",
        QUrl());

    QObject *obj = component.create();
    if (component.isError()) {
        printf("=== QML Error: %s ===\n", qPrintable(component.errors().first().toString()));
        fflush(stdout);
        return 1;
    }

    QObject *vw = obj->findChild<QObject *>("vw");
    if (!vw) vw = obj;

    int lastVol = vw->property("volume").toInt();
    bool lastMuted = vw->property("muted").toBool();
    printf("Initial: volume=%d, muted=%s\n", lastVol, lastMuted ? "true" : "false");
    fflush(stdout);

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        int vol = vw->property("volume").toInt();
        bool muted = vw->property("muted").toBool();
        if (vol != lastVol || muted != lastMuted) {
            printf("CHANGED: volume=%d muted=%s\n", vol, muted ? "true" : "false");
            lastVol = vol;
            lastMuted = muted;
        }
    });
    timer.start(500);

    QTimer::singleShot(15000, &app, &QCoreApplication::quit);

    return app.exec();
}
