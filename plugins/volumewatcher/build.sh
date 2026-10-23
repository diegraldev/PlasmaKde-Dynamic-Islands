#!/bin/bash
set -e
cd "$(dirname "$0")"
/usr/lib64/qt6/libexec/moc -DQQmlExtensionInterface_iid='"org.qt-project.Qt.QQmlExtensionInterface"' volumewatcher.cpp -o volumewatcher.moc
g++ -fPIC -c volumewatcher.cpp -o volumewatcher.o \
    -I/usr/include/qt6 -I/usr/include/qt6/QtCore -I/usr/include/qt6/QtQml \
    $(pkg-config --cflags libpulse)
g++ -shared -o libvolumewatcherplugin.so volumewatcher.o -lQt6Qml -lQt6Core $(pkg-config --libs libpulse)
mkdir -p "$HOME/.local/share/qml/VolumeWatcher"
cp libvolumewatcherplugin.so "$HOME/.local/share/qml/VolumeWatcher/libvolumewatcher.so"
printf 'module VolumeWatcher\nplugin volumewatcher\n' > "$HOME/.local/share/qml/VolumeWatcher/qmldir"
echo "Installed VolumeWatcher"
