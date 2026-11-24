#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tracks made with CTR Track Editor (https://github.com/Boooing/CTR-Nitro-Fueled-Track-Editor, v0.6.5
read): what the launcher does for them.

Reference counts. Each object of a map file (maps/*.igz) keeps, in the u32 at +0x08, the number of
references to it. In the game's own map files it equals the pointers (ROFS fixups) to the object.
Wrong counts let a track play, but the level loaded after it crashes the game ("Fault ... guest
offset 0x14a94e9", igStreamingChunkManager thread, removing a script class from a list holding
freed memory) or, with counts too high, the game leaves the track's memory allocated and can hang
on the loading screen. Two conventions are found in community tracks:

  all pointers      CTR Track Editor (igz.IgzFile.fix_refcounts, its fix of 2026-11-08, v0.5): every
                    pointer to the object counts. Tracks saved before had them wrong (Backrooms
                    12 603 objects, Suzuka 12 618). Its files keep entities outside the root list,
                    and the game uses them.
  reached pointers  Neo Bowser City (another tool): only pointers from what the file reaches from
                    its root lists (ROOT, ONAM: objects and their data blocks) count; the many
                    objects nothing reaches (2 342 loose spline points...) keep theirs. The editor's
                    rule raised 7 159 of its counts: 35 674 blocks left allocated and a hang. Its
                    own rule changes 531 (tried in game: it plays, Coco Park after it, and again).

A track keeps the convention most of its map files' own counts follow (one tool made them all),
made exact (the game's files match both). An object without pointers (root lists) stays as it is; files that are not maps are never
touched (script files keep counts lower than their pointers on purpose).

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


def reached_pointers(z):
    """(objects reached from the root lists, Counter of the pointers to each from reached places)."""
    roots = [int.from_bytes(f[3][:4], 'little') for f in z.fixups if f[0] in ('ROOT', 'ONAM') and f[1]]
    reached = {r for r in roots if r in z.typed}
    blocks, pointers = set(), Counter()
    stack = [(r, z.size(r)) for r in reached]
    while stack:
        at, length = stack.pop()
        for tag, site in z.sites_in(at, length):
            if tag != 'ROFS':
                continue
            ref = z.ref(site)
            if ref in z.typed:
                pointers[ref] += 1
                if ref not in reached:
                    reached.add(ref)
                    stack.append((ref, z.size(ref)))
            elif site - at >= 8 and ref not in blocks:
                size = z.u64(site - 8) & 0xFFFFFFFF
                if z.block_at(ref, size):
                    blocks.add(ref)
                    stack.append((ref, size))
    return reached, pointers


ALL, REACHED = 'all pointers', 'reached pointers'


def counts_by_rule(z):
    """{rule: {object: count}} of a map file, for the objects each rule sets."""
    every = Counter(z.ref(site) for site in z.sites.get('ROFS', []))
    reached, pointers = reached_pointers(z)
    return {ALL: {o: n for o, n in every.items() if o in z.typed},
            REACHED: {o: pointers[o] for o in reached if pointers[o]}}


def votes(z):
    """Counter: how many of the file's own counts each rule gives."""
    return Counter({rule: sum(1 for o, n in counts.items() if z.u32(o + 8) == n)
                    for rule, counts in counts_by_rule(z).items()})


def rule_of(blobs):
    """The convention of a track: the rule most of its map files' counts follow."""
    total = Counter()
    for blob in blobs:
        try:
            total += votes(File(blob))
        except Exception:  # noqa: BLE001 - not a standard IGZ
            continue
    return REACHED if total[REACHED] > total[ALL] else ALL


def fix_counts(blob, rule=None):
    """(bytes, objects fixed) of one map file under `rule` (the file's own when None); the same
    bytes when nothing is wrong or it is not a standard IGZ."""
    try:
        z = File(blob)
    except Exception:  # noqa: BLE001 - not a standard IGZ (some maps/ files): left as it is
        return blob, 0
    rule = rule or (REACHED if votes(z)[REACHED] > votes(z)[ALL] else ALL)
    wrong = [(o, n) for o, n in counts_by_rule(z)[rule].items() if z.u32(o + 8) != n]
    if not wrong:
        return blob, 0
    for o, n in wrong:
        z.set_u32(o + 8, n)
    return z.build(), len(wrong)


def map_files(archive):
    """{index: bytes} of an archive's map files."""
    return {i: archive.read(i) for i in range(len(archive.names)) if is_map(archive.logical(i))}


def wrong_counts(path):
    """({logical path: objects with a wrong count} of an archive's map files, the track's rule)."""
    archive = Archive(Path(path))
    try:
        maps = map_files(archive)
        rule = rule_of(maps.values())
        out = {}
        for i, blob in maps.items():
            _, n = fix_counts(blob, rule)
            if n:
                out[archive.logical(i)] = n
        return out, rule
    finally:
        archive.f.close()


def fix_archive(path):
    """The map files of an archive with wrong counts, fixed in place: each fixed file is appended
    (uncompressed, sector aligned) and its table entry points at it. Returns {logical: objects}."""
    path = Path(path)
    archive = Archive(path)
    try:
        count = len(archive.names)
        maps = map_files(archive)
        rule = rule_of(maps.values())
        fixed = {}
        for i, blob in maps.items():
            data, n = fix_counts(blob, rule)
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
    if args.fix:
        result = fix_archive(args.archive)
    else:
        result, rule = wrong_counts(args.archive)
        print(f'convention: {rule}')
    for logical, n in sorted(result.items()):
        print(f'{n:>7}  {logical}')
    print(f'{sum(result.values())} objects with a wrong reference count'
          + (', fixed' if args.fix and result else '') + f' in {len(result)} map files')
    return 0


if __name__ == '__main__':
    sys.exit(main())
