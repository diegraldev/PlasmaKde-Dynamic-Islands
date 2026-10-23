#!/bin/bash
set -e
cd "$(dirname "$0")"
/usr/lib64/qt6/libexec/moc -DQQmlExtensionInterface_iid='"org.qt-project.Qt.QQmlExtensionInterface"' msiec.cpp -o msiec.moc
g++ -fPIC -c msiec.cpp -o msiec.o \
    -I/usr/include/qt6 -I/usr/include/qt6/QtCore -I/usr/include/qt6/QtQml
g++ -shared -o libmsiecplugin.so msiec.o -lQt6Qml -lQt6Core
mkdir -p "$HOME/.local/share/qml/MsiEc"
cp libmsiecplugin.so "$HOME/.local/share/qml/MsiEc/libmsiec.so"
printf 'module MsiEc\nplugin msiec\n' > "$HOME/.local/share/qml/MsiEc/qmldir"
echo "Installed MsiEc"
