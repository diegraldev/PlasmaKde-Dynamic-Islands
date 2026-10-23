#!/bin/bash
set -e
cd "$(dirname "$0")"
/usr/lib64/qt6/libexec/moc volumewatcher.cpp -Muri=VolumeWatcher -o volumewatcher.moc
g++ -fPIC -c volumewatcher.cpp -o volumewatcher.o -I/usr/include/qt6 -I/usr/include/qt6/QtCore -I/usr/include/qt6/QtQml
g++ -shared -o libvolumewatcherplugin.so volumewatcher.o -lQt6Qml -lQt6Core
cp libvolumewatcherplugin.so "$HOME/.local/share/qml/VolumeWatcher/libvolumewatcher.so"
echo "Installed. Restart plasmashell to pick it up."
