#!/bin/bash
set -e
cd "$(dirname "$0")"
/usr/lib64/qt6/libexec/moc micstate.h -o moc_micstate.cpp
/usr/lib64/qt6/libexec/moc -DQQmlExtensionInterface_iid='"org.qt-project.Qt.QQmlExtensionInterface"' micstate_plugin.cpp -o micstate_plugin.moc
g++ -fPIC -c micstate.cpp -o micstate.o \
    -I/usr/include/qt6 -I/usr/include/qt6/QtCore -I/usr/include/qt6/QtQml \
    $(pkg-config --cflags libpulse)
g++ -fPIC -c micstate_plugin.cpp -o micstate_plugin.o \
    -I/usr/include/qt6 -I/usr/include/qt6/QtCore -I/usr/include/qt6/QtQml \
    $(pkg-config --cflags libpulse)
g++ -fPIC -c moc_micstate.cpp -o moc_micstate.o \
    -I/usr/include/qt6 -I/usr/include/qt6/QtCore -I/usr/include/qt6/QtQml \
    $(pkg-config --cflags libpulse)
g++ -shared -o libmicstateplugin.so micstate.o micstate_plugin.o moc_micstate.o \
    -lQt6Qml -lQt6Core $(pkg-config --libs libpulse)
mkdir -p "$HOME/.local/share/qml/MicState"
cp libmicstateplugin.so "$HOME/.local/share/qml/MicState/libmicstate.so"
printf 'module MicState\nplugin micstate\n' > "$HOME/.local/share/qml/MicState/qmldir"
echo "Installed MicState"
