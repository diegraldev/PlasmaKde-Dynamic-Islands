#!/bin/bash
# Install the Dynamic Island msi-wmi fork as a DKMS module and swap it in.
#
#   sudo ./install.sh
#
# The fork reports every raw WMI vendor event as EV_MSC/MSC_RAW on the
# "MSI WMI hotkeys" input device (stock msi-wmi drops unmapped events with
# just a dmesg line). The Dynamic Island plugin listens for those events and
# re-reads the msi-ec sysfs state once per notification, which makes
# firmware Fn-key actions like Cooler Boost fully event-driven.
#
# After installing, restart plasmashell so the plugin re-opens the recreated
# input device:
#   systemctl --user restart plasma-plasmashell.service
set -euo pipefail

VERSION="0.1"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

if [[ $EUID -ne 0 ]]; then
    echo "Run with sudo: sudo $0" >&2
    exit 1
fi

echo "== Installing DKMS package msi-wmi/$VERSION =="
rm -rf "/usr/src/msi-wmi-$VERSION"
cp -r "$SRC_DIR" "/usr/src/msi-wmi-$VERSION"
rm -f "/usr/src/msi-wmi-$VERSION/install.sh"

dkms remove "msi-wmi/$VERSION" --all 2>/dev/null || true
dkms add "msi-wmi/$VERSION"
dkms build "msi-wmi/$VERSION"
dkms install --force "msi-wmi/$VERSION"

echo "== Swapping the live module =="
if lsmod | grep -q "^msi_wmi "; then
    rmmod msi_wmi
fi
modprobe msi_wmi

sleep 0.5
LOADED=$(modinfo -F filename msi_wmi)
echo "Active module: $LOADED"
if [[ "$LOADED" == *"updates"* || "$LOADED" == *"extra"* ]]; then
    echo "OK: the DKMS fork is active. Restart plasmashell, then press the"
    echo "Fn Cooler Boost key: the Dynamic Island capsule should announce it."
else
    echo "WARNING: the in-tree module still looks active. Reboot to pick up the fork."
fi
