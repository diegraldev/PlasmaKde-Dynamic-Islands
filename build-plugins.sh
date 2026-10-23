#!/bin/bash
# Build all QML C++ modules and install them into ~/.local/share/qml.
# No root is required. Restart plasmashell once after this finishes.
set -euo pipefail
cd "$(dirname "$0")/plugins"

build_one() {
    local dir="$1"
    (cd "$dir" && ./build.sh)
}

build_one micstate
build_one volumewatcher
build_one camera
build_one keystate
build_one msiec
build_one audiolevels

echo "All plugins installed. Restart plasmashell to load them."
