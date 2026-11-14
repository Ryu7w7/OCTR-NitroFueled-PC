#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tracks made with CTR Track Editor (https://github.com/Boooing/CTR-Nitro-Fueled-Track-Editor, v0.6.5
read): what the launcher does for them.

Reference counts. Each object of a map file (maps/*.igz) keeps, in the u32 at +0x08, the number of
references to it, which must equal the pointers (ROFS fixups) to it in the file: the game's own
map files all do. Tracks saved before the editor's fix of 2026-11-08 (v0.5) have them too high or
too low (Backrooms 12 603 objects, Suzuka 12 618, Neo Bowser City 7 159). Each such track plays,
but the next level loaded after it crashes the game ("Fault ... guest offset 0x14a94e9",
igStreamingChunkManager thread, removing a script class from a list holding freed memory). The
editor's rule (igz.IgzFile.fix_refcounts) is applied here: an object with pointers gets their
count; one without (root lists) stays as it is; files that are not maps are never touched (script
files keep counts lower than their pointers on purpose).

The fixed map files are appended to the archive and its table entries point at them: the rest of
the archive, compressed, stays byte for byte.

Installs in the game folder. The editor's "Install in the game" writes into the game's own
archives (new t9xx_*.pak, update3.pak, update.pak and update2.pak changed in place, with
*.ctrmod.json / *.ctrmod.toc records next to them). A game changed that way is not the game the
launcher compares mods against: traces() finds those files so the launcher can say so.

    python scripts/track_editor.py counts ARCHIVE.pak [--fix]
    python scripts/track_editor.py traces GAME_FOLDER
"""
import argparse
import re
import struct
import sys
from collections import Counter
from pathlib import Path

from graphics_tab import SECTOR, Archive
from igz_merge import File


# --- reference counts --------------------------------------------------------------------------

def is_map(logical):
    p = logical.casefold().replace('\\', '/')
    return p.startswith('maps/') and p.endswith('.igz')


def fix_counts(blob):
    """(bytes, objects fixed) of one map file; the same bytes when nothing is wrong or it is not a
    standard IGZ."""
    try:
        z = File(blob)
    except Exception:  # noqa: BLE001 - not a standard IGZ (some maps/ files): left as it is
        return blob, 0
    refs = Counter(z.ref(site) for site in z.sites.get('ROFS', []))
    wrong = [(o, n) for o, n in refs.items() if o in z.typed and z.u32(o + 8) != n]
    if not wrong:
        return blob, 0
    for o, n in wrong:
        z.set_u32(o + 8, n)
    return z.build(), len(wrong)


def wrong_counts(path):
    """{logical path: objects with a wrong count} of an archive's map files."""
    archive = Archive(Path(path))
    try:
        out = {}
        for i in range(len(archive.names)):
            if is_map(archive.logical(i)):
                _, n = fix_counts(archive.read(i))
                if n:
                    out[archive.logical(i)] = n
        return out
    finally:
        archive.f.close()


def fix_archive(path):
    """The map files of an archive with wrong counts, fixed in place: each fixed file is appended
    (uncompressed, sector aligned) and its table entry points at it. Returns {logical: objects}."""
    path = Path(path)
    archive = Archive(path)
    try:
        count = len(archive.names)
        fixed = {}
        for i in range(count):
            if is_map(archive.logical(i)):
                data, n = fix_counts(archive.read(i))
                if n:
                    fixed[i] = (archive.logical(i), data, n)
        infos = list(archive.infos)
    finally:
        archive.f.close()
    if not fixed:
        return {}
    with open(path, 'r+b') as f:
        f.seek(0, 2)
        end = f.tell()
        for i, (_, data, _) in sorted(fixed.items()):
            end += -end % SECTOR
            f.seek(end)
            f.write(data)
            _, ordinal, _, _ = infos[i]
            ordinal = (ordinal & ~0xFF) | (end >> 32)  # the offset's high byte (archives past 4 GiB)
            f.seek(0x38 + 4 * count + 16 * i)
            f.write(struct.pack('<IiIi', end & 0xFFFFFFFF, ordinal, len(data), -1))
            end += len(data)
        f.seek(end)
        f.truncate()
    return {logical: n for logical, _, n in fixed.values()}


# --- installs in the game folder ---------------------------------------------------------------

# What the editor (and CTR Track Installer, the same code) leaves in the game folder.
TRACE_FILES = re.compile(r'.*\.ctrmod(\.json|\.toc|-off)?\Z|ctr_installed_tracks\.json\Z|'
                         r'ctr_custom_category\.json\Z|ctr_update3\.json\Z', re.I)
# The game has no t9xx tracks and only update.pak and update2.pak.
ADDED_ARCHIVES = re.compile(r't9\d\d_.*\.pak\Z|update([3-9]|\d\d+)\.pak\Z', re.I)


def traces(game):
    """Sorted names (relative to the game folder) of the files a track editor install left there."""
    game = Path(game)
    found = []
    for folder in (game, game / 'archives'):
        if not folder.is_dir():
            continue
        for p in folder.iterdir():
            if p.is_file() and (TRACE_FILES.match(p.name) or (folder.name == 'archives' and ADDED_ARCHIVES.match(p.name))):
                found.append(p.relative_to(game).as_posix())
    return sorted(found)


def notice(game):
    """What to tell the person when the game folder has tracks installed into it, or None."""
    found = traces(game)
    if not found:
        return None
    shown = ', '.join(found[:6]) + (f' and {len(found) - 6} more' if len(found) > 6 else '')
    return ('The game folder was changed by CTR Track Editor ("Install in the game") or CTR Track Installer: '
            f'{shown}. Mods and external content are made against the original game files: restore the '
            'game folder from a backup (or the editor\'s "Restore the game\'s archives"), and add tracks '
            'with the editor\'s OCTR mod export instead. Folders that share files with it through hard '
            'links (the base game, the update) were changed too.')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    counts = sub.add_parser('counts')
    counts.add_argument('archive', type=Path)
    counts.add_argument('--fix', action='store_true')
    found = sub.add_parser('traces')
    found.add_argument('game', type=Path)
    args = parser.parse_args(argv)
    if args.command == 'traces':
        print(notice(args.game) or 'no track editor install found')
        return 0
    result = fix_archive(args.archive) if args.fix else wrong_counts(args.archive)
    for logical, n in sorted(result.items()):
        print(f'{n:>7}  {logical}')
    print(f'{sum(result.values())} objects with a wrong reference count'
          + (', fixed' if args.fix and result else '') + f' in {len(result)} map files')
    return 0


if __name__ == '__main__':
    sys.exit(main())
