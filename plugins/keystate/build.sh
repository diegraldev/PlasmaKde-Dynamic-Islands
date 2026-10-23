#!/bin/bash
set -e
cd "$(dirname "$0")"
/usr/lib64/qt6/libexec/moc -DQQmlExtensionInterface_iid='"org.qt-project.Qt.QQmlExtensionInterface"' keystatewatcher.cpp -o keystatewatcher.moc
g++ -fPIC -c keystatewatcher.cpp -o keystatewatcher.o -I/usr/include/qt6 -I/usr/include/qt6/QtCore -I/usr/include/qt6/QtQml
g++ -shared -o libkeystateplugin.so keystatewatcher.o -lQt6Qml -lQt6Core -lwayland-client
cp libkeystateplugin.so "$HOME/.local/share/qml/KeyState/libkeystate.so"
echo "Installed. Restart plasmashell to pick it up."