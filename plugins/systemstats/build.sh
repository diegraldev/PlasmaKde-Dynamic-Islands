#!/bin/bash
set -e
cd "$(dirname "$0")"
/usr/lib64/qt6/libexec/moc -DQQmlExtensionInterface_iid='"org.qt-project.Qt.QQmlExtensionInterface"' systemstats.cpp -o systemstats.moc
g++ -std=c++17 -fPIC -c systemstats.cpp -o systemstats.o \
    -I/usr/include/qt6 -I/usr/include/qt6/QtCore -I/usr/include/qt6/QtQml
g++ -shared -o libsystemstatsplugin.so systemstats.o \
    -lQt6Qml -lQt6Core
mkdir -p "$HOME/.local/share/qml/SystemStats"
cp libsystemstatsplugin.so "$HOME/.local/share/qml/SystemStats/libsystemstats.so"
printf 'module SystemStats\nplugin systemstats\n' > "$HOME/.local/share/qml/SystemStats/qmldir"
echo "Installed SystemStats"
