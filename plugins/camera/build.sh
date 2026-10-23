#!/bin/bash
set -e
cd "$(dirname "$0")"
/usr/lib64/qt6/libexec/moc -DQQmlExtensionInterface_iid='"org.qt-project.Qt.QQmlExtensionInterface"' camstatus.cpp -o camstatus.moc
g++ -fPIC -c camstatus.cpp -o camstatus.o \
    -I/usr/include/qt6 -I/usr/include/qt6/QtCore -I/usr/include/qt6/QtQml
g++ -shared -o libcamstatusplugin.so camstatus.o -lQt6Qml -lQt6Core -ludev
mkdir -p "$HOME/.local/share/qml/CameraStatus"
cp libcamstatusplugin.so "$HOME/.local/share/qml/CameraStatus/libcamstatus.so"
printf 'module CameraStatus\nplugin camstatus\n' > "$HOME/.local/share/qml/CameraStatus/qmldir"
echo "Installed CameraStatus"
