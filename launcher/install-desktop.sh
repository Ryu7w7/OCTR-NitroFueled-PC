#!/usr/bin/env bash
# Adds the launcher to the application menu (~/.local/share/applications) for this checkout, with
# the game's own icon (sce_sys/icon0.png from the game folder: the launcher's saved game folder,
# else orig/CUSA13795, else $1) copied into the user's icon theme. Game files never enter the repo.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
share=${XDG_DATA_HOME:-$HOME/.local/share}
dest=$share/applications
mkdir -p "$dest"
game=${1:-}
if [[ -z $game && -f ${XDG_CONFIG_HOME:-$HOME/.config}/sacredfire-launcher/settings.json ]]; then
    game=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("game_dir",""))' \
        "${XDG_CONFIG_HOME:-$HOME/.config}/sacredfire-launcher/settings.json" 2>/dev/null || true)
fi
[[ -n $game ]] || game=$here/../orig/CUSA13795
icon=applications-games
if [[ -f $game/sce_sys/icon0.png ]]; then
    mkdir -p "$share/icons/hicolor/512x512/apps"
    cp -f "$game/sce_sys/icon0.png" "$share/icons/hicolor/512x512/apps/io.github.sacredfire.Launcher.png"
    icon=io.github.sacredfire.Launcher
    echo "Icon: $game/sce_sys/icon0.png"
fi
sed -e "s|@LAUNCHER@|$here/bb-launcher.sh|" -e "s|@ICON@|$icon|" "$here/io.github.sacredfire.Launcher.desktop.in" \
    > "$dest/io.github.sacredfire.Launcher.desktop"
echo "Installed $dest/io.github.sacredfire.Launcher.desktop"
