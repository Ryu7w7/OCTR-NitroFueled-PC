#!/usr/bin/env bash
# Builds dist/SacredFire-x86_64.AppImage: the port's current build (run build.sh first),
# its scripts and the launcher, bundled with their Nix closure (nix-appimage: the AppImage mounts
# its /nix/store with user namespaces, available on SteamOS and most desktops).
# Running it opens the launcher; `--play` starts the game with the launcher's saved settings.
# Data (generated files, saves, bbport.ini): ~/.local/share/sacredfire (BB_DATA_DIR).
# Needs Nix (nix bundle, flakes) and binaries built inside nix-shell, whose RUNPATHs name the store
# paths the bundle collects (a build against the system's libraries would not run elsewhere):
#   NIX_PATH=nixpkgs=$HOME/.nix-defexpr/channels/nixpkgs   # single-user Nix with a channel
#   mv out out.arch; nix-shell shell.nix --run 'BB_IN_NIX_SHELL=1 bash build.sh'
#   bash packaging/appimage.sh
#   mv out out.nix; mv out.arch out        # the everyday build goes back (out.* are ignored)
# nix bundle extracts nothing, but running the AppImage with --appimage-extract-and-run (no FUSE)
# unpacks 2.8 GB into $TMPDIR: point TMPDIR at a disk, not a small tmpfs. After editing files
# that go into the package, remove ~/.cache/nix/fetcher-cache-v*.sqlite* so the source is re-read.
# A release package must match a published commit (GPL: the source of the binary): the build's
# out/VERSION has to equal the checkout's revision and must not be -dirty. Tag the commit (vX.Y)
# before building so the version reads as the tag. BB_ALLOW_DIRTY=1 skips the check for test builds.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
[[ -f out/sacredfire && -f out/gpu/libbbgpu.so && -f out/VERSION ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
built_from=$(cat out/VERSION)
checkout=$(git describe --tags --match 'v*' --always 2>/dev/null || echo unknown)
git update-index -q --refresh 2>/dev/null || true
git diff-index --quiet --ignore-submodules=dirty HEAD -- 2>/dev/null || checkout="$checkout-dirty"
if [[ -z ${BB_ALLOW_DIRTY:-} ]]; then
    if [[ $checkout == *-dirty ]]; then
        echo "The checkout has uncommitted changes ($checkout): commit and tag first, so the package matches its source (BB_ALLOW_DIRTY=1 for a test build)." >&2; exit 1
    fi
    if [[ $built_from != "$checkout" ]]; then
        echo "out/ was built from $built_from, the checkout is at $checkout: run build.sh again (BB_ALLOW_DIRTY=1 to package anyway)." >&2; exit 1
    fi
fi
root=$PWD
# The libraries' store paths (RUNPATH entries and their closures come along).
{
    echo '['
    for elf in out/sacredfire out/gpu/libbbgpu.so; do
        readelf -d "$elf" | sed -n 's/.*\[\(.*\)\]/\1/p' | tr ':' '\n'
    done | grep -o '^/nix/store/[^/]*' | sort -u | grep -v -- '-nix-shell$' | sed 's/.*/  "&"/'
    echo ']'
} > packaging/runtime-paths.nix
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
(cd "$work" && nix bundle --impure --bundler github:ralismark/nix-appimage \
    --expr "import $root/packaging {}")
mkdir -p dist
# The bundler names its output after the package's main program (sacredfire.AppImage).
built=$(ls "$work"/*.AppImage | head -1)
install -m755 "$built" dist/SacredFire-x86_64.AppImage
(cd dist && sha256sum SacredFire-x86_64.AppImage > SacredFire-x86_64.AppImage.sha256)
# What the package was built from (for the release notes: the GPL source of the binary is the
# repository at this revision plus nixpkgs at this version for the bundled libraries).
nixpkgs=$(nix-instantiate --eval -E '(import <nixpkgs> {}).lib.version' 2>/dev/null | tr -d '"' || echo unknown)
printf 'SacredFire %s\ncommit %s\nnixpkgs %s\n' "$built_from" "$(git rev-parse HEAD)" "${nixpkgs:-unknown}" \
    > dist/SacredFire-x86_64.AppImage.build.txt
ls -lh dist/SacredFire-x86_64.AppImage
cat dist/SacredFire-x86_64.AppImage.build.txt
