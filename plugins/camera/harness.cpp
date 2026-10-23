#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QDir>
#include <QDebug>
#include <QTimer>
#include <QFile>
#include <cstdio>

static void createFile(const char *name)
{
    QFile f(QString::fromUtf8(name));
    f.open(QIODevice::WriteOnly);
    f.close();
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    engine.addImportPath(QDir::home().filePath(QStringLiteral(".local/share/qml")));

    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral("/tmp/opencode/camstatus/test.qml")));
    if (component.isError()) {
        const auto errors = component.errors();
        for (const auto &e : errors) {
            fprintf(stderr, "QML-ERROR: %s\n", qPrintable(e.toString()));
        }
        return 1;
    }
    QObject *obj = component.create();
    if (!obj) {
        fprintf(stderr, "QML-ERROR: create failed\n");
        return 1;
    }
    fprintf(stderr, "HARNESS: created ok, camAvail=%s camCount=%s\n",
        qPrintable(obj->property("camAvail").toString()),
        qPrintable(obj->property("camCount").toString()));

    QTimer::singleShot(1000, [] {
        fprintf(stderr, "HARNESS: [t=1s] creating wtest/video0\n");
        createFile("/tmp/opencode/camstatus/wtest/video0");
    });
    QTimer::singleShot(2000, [] {
        fprintf(stderr, "HARNESS: [t=2s] creating wtest/video1\n");
        createFile("/tmp/opencode/camstatus/wtest/video1");
    });
    QTimer::singleShot(3000, [] {
        fprintf(stderr, "HARNESS: [t=3s] removing video0 video1\n");
        QFile::remove(QStringLiteral("/tmp/opencode/camstatus/wtest/video0"));
        QFile::remove(QStringLiteral("/tmp/opencode/camstatus/wtest/video1"));
    });
    QTimer::singleShot(1500, &app, [obj] {
        fprintf(stderr, "HARNESS: [t=1.5s] camAvail=%s camCount=%s\n",
            qPrintable(obj->property("camAvail").toString()),
            qPrintable(obj->property("camCount").toString()));
    });
    QTimer::singleShot(2500, &app, [obj] {
        fprintf(stderr, "HARNESS: [t=2.5s] camAvail=%s camCount=%s\n",
            qPrintable(obj->property("camAvail").toString()),
            qPrintable(obj->property("camCount").toString()));
    });
    QTimer::singleShot(3500, &app, [obj] {
        fprintf(stderr, "HARNESS: [t=3.5s] camAvail=%s camCount=%s\n",
            qPrintable(obj->property("camAvail").toString()),
            qPrintable(obj->property("camCount").toString()));
    });
    QTimer::singleShot(4000, &app, &QCoreApplication::quit);
    app.exec();
    fprintf(stderr, "HARNESS: done, camAvail=%s camCount=%s\n",
        qPrintable(obj->property("camAvail").toString()),
        qPrintable(obj->property("camCount").toString()));
    return 0;
}
