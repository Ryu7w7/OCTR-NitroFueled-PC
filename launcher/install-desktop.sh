#!/usr/bin/env bash
# Adds this checkout to the application menu with the shared rounded launcher/game icon.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
share=${XDG_DATA_HOME:-$HOME/.local/share}
dest=$share/applications
mkdir -p "$dest"
mkdir -p "$share/icons/hicolor/256x256/apps"
cp -f "$here/io.github.sacredfire.Launcher.png" "$share/icons/hicolor/256x256/apps/io.github.sacredfire.Launcher.png"
# Remove an older installer-owned square icon that desktops may prefer at higher DPI.
rm -f "$share/icons/hicolor/512x512/apps/io.github.sacredfire.Launcher.png"
icon=io.github.sacredfire.Launcher
sed -e "s|@LAUNCHER@|$here/bb-launcher.sh|" -e "s|@ICON@|$icon|" "$here/io.github.sacredfire.Launcher.desktop.in" \
    > "$dest/io.github.sacredfire.Launcher.desktop"
echo "Installed $dest/io.github.sacredfire.Launcher.desktop"
