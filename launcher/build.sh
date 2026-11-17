#!/usr/bin/env bash
# Build the native launcher and optional EOS bridge; the game and renderer are separate targets.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
export PKG_CONFIG_PATH="$root/out/deps/prefix/lib64/pkgconfig:$root/out/deps/prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
if ! pkg-config --exists sdl3 libpng freetype2 openssl ayatana-appindicator-glib dbusmenu-glib-0.4 2>/dev/null; then
    if [[ -z ${BB_LAUNCHER_NIX:-} ]] && command -v nix-shell >/dev/null; then
        exec nix-shell "$root/launcher/shell.nix" --run 'BB_LAUNCHER_NIX=1 bash "$BB_LAUNCHER_BUILD"' \
            --argstr launcherBuild "$root/launcher/build.sh"
    fi
    echo 'Need SDL3 >= 3.2, libpng, FreeType, OpenSSL, Ayatana AppIndicator GLib, libdbusmenu-glib, CMake, Ninja and a C++20 compiler (launcher/shell.nix).' >&2
    exit 1
fi
cmake -S "$root/launcher" -B "$root/out/launcher" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$root/out/launcher" --target ctr-launcher --parallel "${BB_BUILD_JOBS:-2}"
bash "$root/eosbridge/build.sh" --auto
