#!/bin/sh
# SPDX-License-Identifier: MIT
# Registers AudioStudio with the desktop for this user: an entry in the
# application menu, and the icon in the dock (GNOME takes the dock icon from
# the desktop entry whose StartupWMClass matches the window, not from the
# window itself). Builds the Release configuration and points the entry at it.
#
#   tools/install_desktop.sh            install or update
#   tools/install_desktop.sh --remove   take the entry away
set -eu

entry="${XDG_DATA_HOME:-$HOME/.local/share}/applications/AudioStudio.desktop"

if [ "${1:-}" = "--remove" ]; then
    rm -f "$entry"
    echo "Removed $entry"
    exit 0
fi

root=$(cd "$(dirname "$0")/.." && pwd)
dotnet build "$root/src/AudioStudio.App" -c Release --nologo -v quiet
mkdir -p "$(dirname "$entry")"
cat > "$entry" <<DESKTOP
[Desktop Entry]
Type=Application
Name=AudioStudio
Comment=Show, record and classify the XIAO ESP32S3 board's audio stream
Exec=$root/src/AudioStudio.App/bin/Release/net10.0/AudioStudio.App
Icon=$root/src/AudioStudio.App/Assets/icon.png
StartupWMClass=AudioStudio.App
Categories=Development;
Terminal=false
DESKTOP
echo "Installed $entry"
