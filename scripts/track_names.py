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


def main():
    p = argparse.ArgumentParser(description="The zones' own names for the Discord presence.")
    p.add_argument('game')
    p.add_argument('--out', required=True)
    a = p.parse_args()
    print(prepare(a.game, a.out))


if __name__ == '__main__':
    main()
