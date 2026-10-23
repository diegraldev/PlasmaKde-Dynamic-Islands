#!/bin/bash
set -e
cd "$(dirname "$0")"
/usr/lib64/qt6/libexec/moc -DQQmlExtensionInterface_iid='"org.qt-project.Qt.QQmlExtensionInterface"' audiolevels.cpp -o audiolevels.moc
g++ -std=c++17 -fPIC -c audiolevels.cpp -o audiolevels.o \
    -I/usr/include/qt6 -I/usr/include/qt6/QtCore -I/usr/include/qt6/QtQml \
    $(pkg-config --cflags libpulse)
g++ -shared -o libaudiolevelsplugin.so audiolevels.o \
    -lQt6Qml -lQt6Core $(pkg-config --libs libpulse)
mkdir -p "$HOME/.local/share/qml/AudioLevels"
cp libaudiolevelsplugin.so "$HOME/.local/share/qml/AudioLevels/libaudiolevels.so"
printf 'module AudioLevels\nplugin audiolevels\n' > "$HOME/.local/share/qml/AudioLevels/qmldir"
echo "Installed AudioLevels"
