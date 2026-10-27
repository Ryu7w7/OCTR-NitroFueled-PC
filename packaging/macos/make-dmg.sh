#!/bin/bash
# Builds dist/SacredFire-macOS.dmg: SacredFire.app with this checkout's build (run
# packaging/macos/play.sh or build.sh first), its scripts, the launcher, and the Vulkan loader and
# MoltenVK from the Vulkan SDK. Whoever installs it needs no Xcode, Vulkan SDK or build: only
# Rosetta 2 and Homebrew, which the app uses for the launcher's GTK 4, libadwaita and Python, and
# for Bash 4 (run.sh); it installs them itself on the first start (in a Terminal window).
# Data (generated files, saves, bbport.ini): ~/Library/Application Support/SacredFire.
# The app is not notarized: the first time, right-click it and choose Open (or
# `xattr -dr com.apple.quarantine /Applications/SacredFire.app`).
# A release must match a published commit (GPL: the source of the binaries), as appimage.sh says.
set -euo pipefail
cd "$(dirname "$0")/../.."
[[ -f out/sacredfire && -f out/bb-gpu-capabilities && -f out/VERSION ]] ||
    { echo 'Build first: bash packaging/macos/play.sh (or bash build.sh)' >&2; exit 1; }
sdk=${VULKAN_SDK:-$(cat out/VULKAN_SDK 2>/dev/null || true)}
[[ -f $sdk/lib/libvulkan.1.dylib && -f $sdk/lib/libMoltenVK.dylib ]] ||
    { echo "No Vulkan SDK at '$sdk' (set VULKAN_SDK=<sdk>/macOS)" >&2; exit 1; }

stage=out/dmg
rm -rf "$stage"
app=$stage/SacredFire.app
share=$app/Contents/Resources/sacredfire
mkdir -p "$app/Contents/MacOS" "$share/bin/vulkan" "$share/tools"
cp run.sh run.py LICENSE "$share/"
cp out/VERSION "$share/VERSION"
cp -R scripts patches launcher "$share/"
[[ -d shaders ]] && cp -R shaders "$share/"
cp tools/gen_import_names.py "$share/tools/"
find "$share" -name '__pycache__' -type d -prune -exec rm -rf {} +

# The game and its GPU check (x86-64, Rosetta 2), loading the bundled Vulkan loader.
for exe in sacredfire bb-gpu-capabilities; do
    cp "out/$exe" "$share/bin/$exe"
    install_name_tool -delete_rpath "$sdk/lib" "$share/bin/$exe" 2>/dev/null || true
    install_name_tool -add_rpath @executable_path/vulkan "$share/bin/$exe"
done
cp "$sdk/lib/libvulkan.1.dylib" "$sdk/lib/libMoltenVK.dylib" "$share/bin/vulkan/"
cat > "$share/bin/vulkan/MoltenVK_icd.json" <<'EOF'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "./libMoltenVK.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": true
    }
}
EOF
cp "$sdk/../LICENSE.txt" "$share/bin/vulkan/VULKAN-SDK-LICENSE.txt" 2>/dev/null || true

cat > "$app/Contents/Info.plist" <<'EOF'
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
    <key>LSMinimumSystemVersion</key><string>15.0</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF

cat > "$app/Contents/MacOS/SacredFire" <<'EOF'
#!/bin/bash
# SacredFire.app: the launcher with this app's prebuilt game (BB_PREBUILT, as the AppImage).
share=$(cd "$(dirname "$0")/../Resources/sacredfire" && pwd)
log="$HOME/Library/Logs/SacredFire.log"
mkdir -p "$HOME/Library/Logs"
exec >> "$log" 2>&1
echo "--- $(date) SacredFire.app"
notice() { osascript -e "display dialog \"$1\" with title \"SacredFire\" buttons {\"OK\"} default button \"OK\"" >/dev/null; }
if ! /usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null; then
    notice "SacredFire needs Rosetta 2. In Terminal: softwareupdate --install-rosetta"
    exit 1
fi
for brew in /opt/homebrew/bin/brew /usr/local/bin/brew; do [[ -x $brew ]] && break; done
if [[ ! -x $brew ]]; then
    notice "SacredFire needs Homebrew (https://brew.sh) for its launcher. Install it, then open SacredFire again."
    open "https://brew.sh"
    exit 1
fi
eval "$("$brew" shellenv)"
gi_python() {
    for python in "$("$brew" --prefix)"/bin/python3 "$("$brew" --prefix)"/bin/python3.*; do
        [[ -x $python ]] || continue
        "$python" -c 'import gi; gi.require_version("Gtk", "4.0"); gi.require_version("Adw", "1")' \
            2>/dev/null && { echo "$python"; return 0; }
    done
    return 1
}
if ! launcher_python=$(gi_python) || [[ ! -x $("$brew" --prefix)/bin/bash ]]; then
    notice "SacredFire installs its launcher's parts with Homebrew (GTK 4, libadwaita, Python, Bash) in a Terminal window. Open SacredFire again when it has finished."
    osascript -e "tell application \"Terminal\" to do script \"$brew install gtk4 libadwaita pygobject3 adwaita-icon-theme bash && echo && echo 'Done: open SacredFire again.'\"" -e 'tell application "Terminal" to activate'
    exit 0
fi
# Game scripts: a Python whose XML parser loads (packaging/macos/play.sh has the story).
for candidate in "$launcher_python" "$(command -v python3 || true)" /usr/bin/python3; do
    [[ -x $candidate ]] || continue
    if "$candidate" -c 'import xml.etree.ElementTree as e; e.fromstring("<a/>")' 2>/dev/null; then
        export PYTHON=$candidate
        break
    fi
done
export BB_PREBUILT=1
export BB_DATA_DIR=${BB_DATA_DIR:-"$HOME/Library/Application Support/SacredFire"}
export VK_DRIVER_FILES=$share/bin/vulkan/MoltenVK_icd.json
mkdir -p "$BB_DATA_DIR"
cd "$share"
exec "$launcher_python" "$share/launcher/sacredfire_launcher.py" "$@"
EOF
chmod +x "$app/Contents/MacOS/SacredFire"

# Icon.
icon=launcher/io.github.sacredfire.Launcher.png
if [[ -f $icon ]]; then
    set_dir=$(mktemp -d)/SacredFire.iconset
    mkdir -p "$set_dir"
    for size in 16 32 128 256 512; do
        sips -z $size $size "$icon" --out "$set_dir/icon_${size}x${size}.png" >/dev/null
        sips -z $((size * 2)) $((size * 2)) "$icon" --out "$set_dir/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$set_dir" -o "$app/Contents/Resources/SacredFire.icns"
    rm -rf "$(dirname "$set_dir")"
fi

# Ad-hoc signatures (install_name_tool invalidated the executables' own).
codesign --force --sign - "$share/bin/vulkan/libvulkan.1.dylib" "$share/bin/vulkan/libMoltenVK.dylib" \
    "$share/bin/sacredfire" "$share/bin/bb-gpu-capabilities"
codesign --force --sign - "$app"

ln -s /Applications "$stage/Applications"
mkdir -p dist
rm -f dist/SacredFire-macOS.dmg
hdiutil create -volname SacredFire -srcfolder "$stage" -ov -format UDZO dist/SacredFire-macOS.dmg >/dev/null
(cd dist && shasum -a 256 SacredFire-macOS.dmg > SacredFire-macOS.dmg.sha256)
printf 'SacredFire %s (macOS)\ncommit %s\nVulkan SDK %s\n' "$(cat out/VERSION)" \
    "$(git rev-parse HEAD 2>/dev/null || echo unknown)" "$(basename "$(dirname "$sdk")")" \
    > dist/SacredFire-macOS.dmg.build.txt
ls -lh dist/SacredFire-macOS.dmg
cat dist/SacredFire-macOS.dmg.build.txt
