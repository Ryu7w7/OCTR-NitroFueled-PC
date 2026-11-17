#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tools/install_from_pkg.sh [--base BASE.pkg] [--update UPDATE.pkg] [--out DIR] [--extractor PATH]
#                           [--download-extractor] [--yes]
#
# Linux helper: turns YOUR OWN PS4 .pkg files (base game + 1.21 update) into the game folder the
# launcher runs. It extracts both with a PKG extractor into DIR (default ~/Games/CTR-NitroFueled):
#     DIR/CUSA13795  DIR/CUSA13795-patch
# merges them with tools/merge_update.py (hard links, no extra disk space) into DIR/CUSA13795-1.21,
# and checks the result with scripts/game_check.py. Choose that last folder in the launcher.
#
# Without --base/--update it looks for the two .pkg files in the current folder, Downloads,
# Desktop and removable drives. The extractor is not bundled: pass --extractor, put
# pkg_extractor.AppImage next to where you run this, or use --download-extractor (fetches the
# AppImage of the shadPS4Plus PKG_EXTRACTOR_1_0 release). No game file is included or downloaded.
set -euo pipefail

here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$here/.." && pwd)
merge_update=${MERGE_UPDATE:-$root/tools/merge_update.py}
game_check=${GAME_CHECK:-$root/scripts/game_check.py}
min_pkg_mb=${INSTALL_MIN_PKG_MB:-1000}
min_free_gb=${INSTALL_MIN_FREE_GB:-35}
api=${GITHUB_API:-https://api.github.com}
extractor_repo=AzaharPlus/shadPS4Plus
extractor_tag=PKG_EXTRACTOR_1_0
titles='CUSA13795 CUSA14876'  # USA (supported) and EU (accepted by the launcher since v0.5.3)

out=${GAME_ROOT:-$HOME/Games/CTR-NitroFueled}
base=; update=; extractor=${PKG_EXTRACTOR:-}; download=0; assume_yes=0
tmp=

say()  { printf '\n== %s\n' "$*"; }
note() { printf '   %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
cleanup() { if [[ -n $tmp ]]; then rm -rf -- "$tmp"; fi; }
trap cleanup EXIT

while (($#)); do
    case $1 in
        --base)     base=${2:?--base needs a path}; shift 2 ;;
        --update)   update=${2:?--update needs a path}; shift 2 ;;
        --out)      out=${2:?--out needs a folder}; shift 2 ;;
        --extractor) extractor=${2:?--extractor needs a path}; shift 2 ;;
        --download-extractor) download=1; shift ;;
        --yes|-y)   assume_yes=1; shift ;;
        -h|--help)  sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
done

magic4()  { head -c 4 -- "$1" | od -An -tx1 | tr -d ' \n'; }
pkg_cid() { dd if="$1" bs=1 skip=64 count=36 2>/dev/null | tr -d '\0' || true; }

# pkg_title FILE LABEL: checks that FILE looks like a whole PS4 .pkg of this game; prints its title id.
pkg_title() {
    local f=$1 label=$2 mb cid title=
    [[ -f $f ]] || die "$label not found: $f"
    mb=$(( $(stat -c %s -- "$f") / 1048576 ))
    (( mb >= min_pkg_mb )) || die "$label is only $mb MB: an incomplete download or one part of a split archive. Use the whole .pkg."
    [[ $(magic4 "$f") == 7f434e54 ]] || die "$label is not a PS4 .pkg (wrong header): $f"
    cid=$(pkg_cid "$f")
    if [[ $cid =~ CUSA[0-9]{5} ]]; then
        title=${BASH_REMATCH[0]}
        [[ " $titles " == *" $title "* ]] || die "$label is $title, not Crash Team Racing Nitro-Fueled (CUSA13795 or CUSA14876)."
        note "$label: $mb MB, Content ID $cid"
        [[ $title != CUSA14876 ]] || note "European edition: accepted by the launcher but less tested (no community patches)."
    else
        note "$label: $mb MB (could not read its Content ID; checked after extraction)."
    fi
    printf '%s' "$title"
}

find_candidates() {   # .pkg files of the game found in the usual places, biggest first
    local dirs=() d f cid
    if [[ -n ${PKG_SEARCH_DIRS:-} ]]; then IFS=: read -r -a dirs <<< "$PKG_SEARCH_DIRS"
    else
        dirs=("$PWD" "/run/media/${USER:-x}" "/media/${USER:-x}")
        if command -v xdg-user-dir >/dev/null 2>&1; then
            dirs+=("$(xdg-user-dir DOWNLOAD)" "$(xdg-user-dir DESKTOP)")
        else dirs+=("$HOME/Downloads" "$HOME/Desktop"); fi
    fi
    for d in "${dirs[@]}"; do
        [[ -d $d ]] || continue
        find "$d" -maxdepth 4 -type f -iname '*.pkg' -size +"${min_pkg_mb}"M 2>/dev/null
    done | sort -u | while IFS= read -r f; do
        [[ $(magic4 "$f") == 7f434e54 ]] || continue
        cid=$(pkg_cid "$f")
        [[ $cid =~ CUSA(13795|14876) ]] && printf '%s\t%s\n' "$(stat -c %s -- "$f")" "$f"
    done | sort -rn | cut -f2- || true
}

pick_pkgs() {
    local found=() ans
    mapfile -t found < <(find_candidates)
    if [[ ${#found[@]} -eq 2 ]]; then
        printf 'Found two game packages (the bigger one is normally the base game):\n  base  : %s\n  update: %s\n' "${found[0]}" "${found[1]}"
        ans=S
        if [[ $assume_yes == 0 ]] && ( : </dev/tty ) 2>/dev/null; then read -r -p 'Use them? [Y/n] ' ans </dev/tty || ans=; fi
        if [[ ${ans:-Y} != [nN]* ]]; then base=${found[0]}; update=${found[1]}; return 0; fi
    fi
    die "Could not pick the .pkg files automatically. Pass them: tools/install_from_pkg.sh --base BASE.pkg --update UPDATE.pkg"
}

find_extractor() {
    local c
    if [[ -z $extractor ]]; then
        for c in "$PWD/pkg_extractor.AppImage" "$out/pkg_extractor.AppImage" "$here/pkg_extractor.AppImage"; do
            if [[ -f $c ]]; then extractor=$c; break; fi
        done
    fi
    if [[ -z $extractor ]] && command -v pkg_extractor >/dev/null 2>&1; then extractor=$(command -v pkg_extractor); fi
    if [[ -z $extractor && $download == 1 ]]; then download_extractor; fi
    [[ -n $extractor ]] || die "No PKG extractor found. Pass --extractor PATH, put pkg_extractor.AppImage in the current folder, or use --download-extractor
(it fetches the AppImage from https://github.com/$extractor_repo/releases/tag/$extractor_tag)."
    [[ -f $extractor ]] || die "extractor not found: $extractor"
    chmod +x -- "$extractor" 2>/dev/null || true
}

download_extractor() {
    local urls url t dest=$out/pkg_extractor.AppImage
    command -v curl >/dev/null 2>&1 || die "--download-extractor needs curl"
    urls=$(curl -fsSL "$api/repos/$extractor_repo/releases/tags/$extractor_tag" \
        | grep -o '"browser_download_url"[[:space:]]*:[[:space:]]*"[^"]*"' | sed 's/.*:[[:space:]]*"\(.*\)"/\1/' || true)
    [[ -n $urls ]] || die "Could not read the $extractor_tag release of $extractor_repo. Download the extractor by hand."
    mkdir -p "$out"
    url=$(grep -iE 'extract.*\.AppImage$' <<< "$urls" | head -1 || true)
    if [[ -n $url ]]; then
        curl -fL --retry 3 -o "$dest" "$url"
    else
        url=$(grep -iE '\.zip$' <<< "$urls" | head -1 || true)
        [[ -n $url ]] || die "No AppImage or zip among the assets of $extractor_tag. Download the extractor by hand."
        t=$(mktemp -d "$out/.tmp-zip.XXXXXX")
        curl -fL --retry 3 -o "$t/pkg.zip" "$url"
        python3 -m zipfile -e "$t/pkg.zip" "$t/x"
        url=$(find "$t/x" -type f -iname '*extract*.AppImage' | head -1 || true)
        [[ -n $url ]] || { rm -rf "$t"; die "No extractor AppImage inside the downloaded zip."; }
        cp -f "$url" "$dest"; rm -rf "$t"
    fi
    chmod +x "$dest"
    extractor=$dest
    note "downloaded $dest (sha256 $(sha256sum "$dest" | cut -c1-16)...)"
}

extract() {   # extract PKG DIR: runs the extractor into DIR and prints the folder it created
    local d
    mkdir -p "$2"
    "$extractor" "$1" "$2" >&2
    d=$(find "$2" -mindepth 1 -maxdepth 1 -type d | head -1)
    [[ -n $d ]] || die "the extractor created no folder from $1"
    printf '%s' "$d"
}

check_game() {   # check_game DIR: scripts/game_check.py, the same check the launcher makes
    local res
    if [[ ! -f $game_check ]]; then note "scripts/game_check.py not found: skipping the check."; return 0; fi
    if res=$(python3 "$game_check" "$1" 2>&1); then note "game check passed (profile: $res)"; return 0; fi
    printf '%s\n' "$res" >&2
    return 1
}

say "1/4  Files"
[[ -n $base && -n $update ]] || pick_pkgs
base_title=$(pkg_title "$base" "base game")
update_title=$(pkg_title "$update" "update")
if [[ -n $base_title && -n $update_title && $base_title != "$update_title" ]]; then
    die "The base game is $base_title but the update is $update_title: editions cannot be mixed."
fi
title=${base_title:-$update_title}
find_extractor
mkdir -p "$out"
out=$(cd -- "$out" && pwd)
if [[ -n $title && -d $out/$title-1.21 ]] && check_game "$out/$title-1.21"; then
    say "Already installed: $out/$title-1.21"
    exit 0
fi
free_gb=$(( $(df --output=avail -B1 "$out" | tail -1) / 1073741824 ))
(( free_gb >= min_free_gb )) || die "Only $free_gb GB free in $out; about $min_free_gb GB are needed."
note "output folder: $out ($free_gb GB free)"

tmp=$out/.tmp-install
rm -rf -- "$tmp"; mkdir -p "$tmp"

say "2/4  Extracting"
if [[ -n $title && -d $out/$title && -d $out/$title-patch ]]; then
    note "$title and $title-patch are already extracted: skipping."
else
    base_dir=$(extract "$base" "$tmp/base")
    name=$(basename "$base_dir")
    [[ $name != *-patch ]] || die "$base is an update, not the base game: swap --base and --update."
    update_dir=$(extract "$update" "$tmp/update")
    uname=$(basename "$update_dir")
    [[ $uname != "$name" ]] || die "$update is a base game, not the 1.21 update: swap --base and --update."
    [[ $uname == "$name-patch" ]] || note "unexpected update folder name $uname (expected $name-patch); continuing."
    rm -rf -- "${out:?}/$name" "${out:?}/$uname"
    mv -- "$base_dir" "$out/$name"
    mv -- "$update_dir" "$out/$uname"
    title=$name
fi
merged_name=$title-1.21

say "3/4  Merging the update into the base game"
if [[ -f $merge_update ]]; then
    python3 "$merge_update" "$out/$title" "$out/$title-patch" "$tmp/merged"
else
    note "tools/merge_update.py not found: copying instead (needs extra disk space)."
    mkdir -p "$tmp/merged"
    cp -r -- "$out/$title"/. "$tmp/merged"/
    cp -rf -- "$out/$title-patch"/. "$tmp/merged"/
fi
rm -rf -- "${out:?}/$merged_name"
mv -- "$tmp/merged" "$out/$merged_name"

say "4/4  Checking"
check_game "$out/$merged_name" || die "The check failed; do not use $out/$merged_name yet. See docs/LINUX_PKG_INSTALL.md."

say "Done. In the launcher, choose this game folder:"
printf '\n    %s\n\n' "$out/$merged_name"
echo "It should say 1.21. $title and $title-patch can stay where they are: the merged folder is hard links, so they take no extra space."
