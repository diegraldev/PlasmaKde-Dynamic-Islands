#!/bin/bash
# One-time, optional installation of a udev rule for passwordless MSI EC writes.
# Run with: sudo ./scripts/install-msi-ec-permissions.sh
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this script with sudo." >&2
    exit 1
fi

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
group="${MSI_EC_GROUP:-wheel}"

install -Dm755 "$root_dir/scripts/msi-ec-permissions" /usr/local/libexec/msi-ec-permissions
install -Dm644 "$root_dir/scripts/99-msi-ec-permissions.rules" /etc/udev/rules.d/99-msi-ec-permissions.rules
udevadm control --reload-rules
udevadm trigger --subsystem-match=platform --sysname-match=msi-ec --action=change
MSI_EC_GROUP="$group" /usr/local/libexec/msi-ec-permissions

echo "MSI EC attributes are now group-writable (group: $group)."
echo "Log out and back in if your session group list does not include it."
