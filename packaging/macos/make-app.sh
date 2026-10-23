#!/bin/bash
# Makes SacredFire.app (the launcher, launcher/sacredfire_launcher.py) in ~/Applications, so it
# opens from Launchpad, Spotlight or the Dock. The app runs this checkout's launcher with
# Homebrew's Python, GTK 4 and libadwaita (installed here when missing); the game itself starts
# from the launcher through packaging/macos/play.sh.
# Usage: bash packaging/macos/make-app.sh [destination folder, default ~/Applications]
set -e
cd "$(dirname "$0")/../.."
repo=$(pwd)
for brew in /opt/homebrew/bin/brew /usr/local/bin/brew; do [[ -x $brew ]] && break; done
[[ -x $brew ]] || { echo "Need Homebrew: see packaging/macos/README.md." >&2; exit 1; }
eval "$("$brew" shellenv)"
gi_python() {
    for python in "$("$brew" --prefix)"/bin/python3 "$("$brew" --prefix)"/bin/python3.*; do
        [[ -x $python ]] || continue
        if "$python" -c 'import gi; gi.require_version("Gtk", "4.0"); gi.require_version("Adw", "1")' \
            2>/dev/null; then
            echo "$python"
            return 0
        fi
    done
    return 1
}
if ! python=$(gi_python); then
    echo "Homebrew: installing gtk4, libadwaita, pygobject3"
    "$brew" install gtk4 libadwaita pygobject3 adwaita-icon-theme
    python=$(gi_python) || { echo "No Homebrew Python can load GTK 4 and libadwaita." >&2; exit 1; }
fi

dest=${1:-$HOME/Applications}
app="$dest/SacredFire.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>SacredFire</string>
    <key>CFBundleDisplayName</key><string>SacredFire</string>
    <key>CFBundleIdentifier</key><string>io.github.sacredfire.Launcher</string>
    <key>CFBundleExecutable</key><string>SacredFire</string>
    <key>CFBundleIconFile</key><string>SacredFire</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF
# The app runs the launcher of this checkout: moving the folder needs this script once more.
cat > "$app/Contents/MacOS/SacredFire" <<EOF
#!/bin/bash
# Opened from Finder the PATH is the system's: Homebrew's comes first for the launcher and the game.
eval "\$("$brew" shellenv)"
mkdir -p "\$HOME/Library/Logs"
exec "$python" "$repo/launcher/sacredfire_launcher.py" "\$@" >> "\$HOME/Library/Logs/SacredFire.log" 2>&1
EOF
chmod +x "$app/Contents/MacOS/SacredFire"

# The icon, from the launcher's PNG.
icon=launcher/io.github.sacredfire.Launcher.png
if [[ -f $icon ]] && command -v iconutil >/dev/null; then
    set_dir=$(mktemp -d)/SacredFire.iconset
    mkdir -p "$set_dir"
    for size in 16 32 128 256 512; do
        sips -z $size $size "$icon" --out "$set_dir/icon_${size}x${size}.png" >/dev/null
        double=$((size * 2))
        sips -z $double $double "$icon" --out "$set_dir/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$set_dir" -o "$app/Contents/Resources/SacredFire.icns" || true
    rm -rf "$(dirname "$set_dir")"
fi
touch "$app" # Finder picks up the new icon
echo "Made $app (launcher log: ~/Library/Logs/SacredFire.log)"
