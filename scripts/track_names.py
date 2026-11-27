#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The tracks', arenas' and Adventure areas' own names, for the launchers' Discord presence.

Each zone has its name in the game's languages in maps/<...>/<Zone>_zoneInfo_<lang>.lng
(CZoneInfo::_displayName, the first string of the file's string table): chunkinfos.pak has the
base game's, update.pak and update2.pak the Grand Prix ones. The names are written once (and again
when those archives change) to OUT/track_names.txt, a line each: zone<TAB>language<TAB>name, the
zone in lower case as the game names its level (runtime_presence.c), the language as the launcher
names it (en, es, fr, pt_BR).

usage: track_names.py GAME --out OUT
"""
import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from archive_reader import Archive  # noqa: E402
from button_prompts import _sections_and_fixups, _strings  # noqa: E402

ARCHIVES = ('chunkinfos.pak', 'update.pak', 'update2.pak')
LANGUAGES = {'en': 'en', 'mx': 'es', 'fr': 'fr', 'pt': 'pt_BR'}
ZONE_INFO = re.compile(r'(?:^|/)([^/]+)_zoneinfo_(en|mx|fr|pt)\.lng$', re.I)


def display_name(blob, zone):
    """The zone's name in this file, '' when it has none (menus, test levels)."""
    try:
        _sections, fixups = _sections_and_fixups(blob)
        strings = _strings(*fixups['TSTR'])
    except (KeyError, ValueError, IndexError):
        return ''
    for s in strings[:2]:
        s = s.strip()
        if s and s != 'Default' and not s.lower().startswith(zone.lower()) and '\t' not in s:
            return s[:60]
    return ''


def collect(game):
    from mod_file_paths import resolve
    names = {}
    for name in ARCHIVES:
        path = resolve(Path(game) / 'archives' / name)
        if not path.is_file():
            continue
        archive = Archive(path)
        for f in archive.files:
            m = ZONE_INFO.search(f['path'].replace('\\', '/'))
            if not m:
                continue
            zone, lang = m[1].lower(), LANGUAGES[m[2].lower()]
            text = display_name(bytes(archive.extract(f)), m[1])
            if text:
                names[(zone, lang)] = text   # later archives (the updates) win
    # The port's new tracks (scripts/track_content.py): their names from their packages.
    try:
        import track_content
        for _folder, pkg in track_content.packages():
            for code, lang in LANGUAGES.items():
                text = pkg.get('names', {}).get(code) or pkg.get('names', {}).get('en')
                if text:
                    names[(track_content.zone(pkg), lang)] = text[:60]
    except (ImportError, OSError, ValueError):
        pass
    return names


def prepare(game, out):
    """OUT/track_names.txt, made again only when the archives changed: its path."""
    from mod_file_paths import resolve
    out = Path(out)
    target, stamp_file = out / 'track_names.txt', out / 'track_names.stamp'
    stamp = {}
    for name in ARCHIVES:
        path = resolve(Path(game) / 'archives' / name)
        if path.is_file():
            stamp[name] = [path.stat().st_size, path.stat().st_mtime_ns]
    stamp['script'] = Path(__file__).stat().st_mtime_ns
    try:
        import track_content
        stamp['tracks'] = track_content.digest()
    except (ImportError, OSError, ValueError):
        pass
    try:
        if target.is_file() and json.loads(stamp_file.read_text(encoding='utf-8')) == stamp:
            return target
    except (OSError, ValueError):
        pass
    names = collect(game)
    out.mkdir(parents=True, exist_ok=True)
    lines = [f'{zone}\t{lang}\t{text}' for (zone, lang), text in sorted(names.items())]
    target.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    stamp_file.write_text(json.dumps(stamp), encoding='utf-8')
    return target


# The tracks' pictures for the presence (tools/presence_images.py: the game's own from the online lobby's
# track vote, and the port's tracks' backgrounds), in the content bucket: <base><zone>.jpg, and
# <base>index.json naming the zones that have one, so pictures are added or changed without a build.
PRESENCE_IMAGES = 'https://dlctr.ryu7w7.xyz/presence/'
ZONE = re.compile(r'[a-z0-9_]{1,60}\Z')


def presence_images(out, base=None, timeout=3, fetch=None):
    """OUT/presence_images.txt, a line each: zone<TAB>picture URL, from the bucket's index (the last one
    fetched, kept as presence_images.json, when the bucket does not answer). BB_PRESENCE_IMAGES: another
    base URL, empty for none (the launcher then shows the logo). Its path, None without pictures."""
    import os
    out = Path(out)
    base = os.environ.get('BB_PRESENCE_IMAGES', PRESENCE_IMAGES) if base is None else base
    target, kept = out / 'presence_images.txt', out / 'presence_images.json'
    if not base or not re.fullmatch(r'https://[A-Za-z0-9.-]+(/[A-Za-z0-9._~%/-]*)?/', base):
        target.unlink(missing_ok=True)
        return None
    if fetch is None:
        import track_content

        def fetch(url):
            with track_content.open_url(url, timeout) as r:
                return r.read(1 << 20)
    index = None
    try:
        index = json.loads(fetch(base + 'index.json'))
        out.mkdir(parents=True, exist_ok=True)
        kept.write_text(json.dumps({'base': base, 'index': index}), encoding='utf-8')
    except (OSError, ValueError):
        try:
            saved = json.loads(kept.read_text(encoding='utf-8'))
            index = saved['index'] if saved.get('base') == base else None
        except (OSError, ValueError, KeyError, TypeError):
            index = None
    zones = index.get('zones') if isinstance(index, dict) and index.get('format') == 1 else None
    if not isinstance(zones, list):
        target.unlink(missing_ok=True)
        return None
    lines = [f'{z}\t{base}{z}.jpg' for z in zones if isinstance(z, str) and ZONE.fullmatch(z)]
    out.mkdir(parents=True, exist_ok=True)
    target.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return target


def main():
    p = argparse.ArgumentParser(description="The zones' own names for the Discord presence.")
    p.add_argument('game')
    p.add_argument('--out', required=True)
    a = p.parse_args()
    print(prepare(a.game, a.out))


if __name__ == '__main__':
    main()
