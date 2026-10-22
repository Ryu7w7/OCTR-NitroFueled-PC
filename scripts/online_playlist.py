"""The online race playlist with every track of the game, at the same weight.

Public online races vote between tracks drawn from the race playlist in
looseData/OnlineSystemData.igz (the game's archives; the newest copy is in update2.pak). Its last
version (October 2019) stops at the Spooky Grand Prix: the four later Grand Prix tracks (Koala
Carnival and the others) were added by the official server, which this port's server cannot do
yet, so they never came up. It also still favours that season's tracks (Twilight Tour twice,
Spooky at three times a normal track's weight).

This builds, from the player's own game, a copy of that file whose race playlist lists every race
track the archives hold (maps/octane/t<NNN>_<name>/ with a zone info), each at the same weight,
and an archive patch (scripts/archive_patch.py) that the runtime applies when the game reads the
archive (src/runtime_file.c): the entry points at the new copy, put after the end of the
archive. The archive itself is never changed. Written to <out>/online_playlist.bbpatch again only
when the archives change.

    online_playlist.py <game> --out <dir> [--weight 100]
"""
import argparse
import json
import os
import re
import struct
import sys
from pathlib import Path

import archive_patch
import mod_file_paths
import gcn_shaders as iga
from igz_file import Igz

FORMAT = 1
ENTRY = 'temporary/octane/data/ps4/output/loosedata/onlinesystemdata.igz'
ARCHIVES = ('update2.pak', 'update.pak', 'permanent.pak')  # newest first
TRACK = re.compile(r'/maps/octane/(t\d{3}_[a-z0-9_]+)/\1_zoneinfo\.igz$')

# Offsets inside the objects (CTR 1.21's types).
PLAYLIST_MAPLIST, PLAYLIST_WEIGHTED = 0x28, 0x30          # COnlinePlaylistData
LIST_COUNT, LIST_CAPACITY, LIST_MEMORY, LIST_ITEMS = 0x0C, 0x10, 0x18, 0x20  # igDataList
MAP_NAME, MAP_WEIGHT, MAP_MODES = 0x10, 0x18, 0x20         # COnlinePlaylistMapData
MAP_PAIR = 0x68  # COnlinePlaylistMapData (0x28) and its CPlaylistModesWeightsTable (0x40)


def track_key(name):
    """'octane/t505_nitro_festival/t505_nitro_festival' -> 't505_nitro_festival'."""
    return name.rsplit('/', 1)[-1].lower()


def game_tracks(archives):
    """Race tracks of the game (folder names), from every archive's names."""
    found = set()
    for pak in mod_file_paths.archives(archives):
        try:
            for name, *_ in iga.pak_entries(pak):
                m = TRACK.search(name.lower())
                if m:
                    found.add(m.group(1))
        except (OSError, struct.error, ValueError, UnicodeDecodeError):
            continue
    return found


def list_items(g, lst):
    count = g.u32(lst + LIST_COUNT)
    items = g.u64(lst + LIST_ITEMS)
    return [items + 8 * i for i in range(count)]


def set_list(g, lst, items, tag, values):
    """Points list lst at a new array of values (references or string indices)."""
    raw = b''.join(struct.pack('<Q', v) for v in values)
    at = g.append(raw)
    for i in range(len(values)):
        g.add_site(tag, at + 8 * i)
    g.set_u32(lst + LIST_COUNT, len(values))
    g.set_u64(lst + LIST_CAPACITY, len(values))
    memory = g.u64(lst + LIST_MEMORY)
    g.set_u64(lst + LIST_MEMORY, (memory & ~0xFFFFFFFF) | len(raw))
    g.set_u64(lst + LIST_ITEMS, at)


def race_playlist(g):
    """The COnlinePlaylistData whose weighted list holds the most race tracks."""
    best, best_count = None, 0
    for off, t in g.objects():
        if t != 'COnlinePlaylistData':
            continue
        weighted = g.u64(off + PLAYLIST_WEIGHTED)
        names = [track_key(g.string_at(g.u64(item) + MAP_NAME)) for item in list_items(g, weighted)]
        count = sum(1 for n in names if TRACK.match(f'/maps/octane/{n}/{n}_zoneinfo.igz'))
        if count > best_count:
            best, best_count = off, count
    if best is None:
        raise ValueError('no race playlist in OnlineSystemData')
    return best


def rebuild(blob, tracks, weight):
    """(new file, report) with the race playlist listing every track in tracks."""
    g = Igz(blob)
    playlist = race_playlist(g)
    weighted, maplist = g.u64(playlist + PLAYLIST_WEIGHTED), g.u64(playlist + PLAYLIST_MAPLIST)
    entries = [g.u64(item) for item in list_items(g, weighted)]
    path_of = {track_key(s): s for s in g.strings}
    # Keep the first entry of each track; later duplicates take missing tracks.
    keep, seen, spare = [], set(), []
    for e in entries:
        key = track_key(g.string_at(e + MAP_NAME))
        if key in seen:
            spare.append(e)
        else:
            seen.add(key)
            keep.append(e)
    missing = sorted(t for t in tracks if t not in seen)
    template = keep[0]
    added = []
    for key in missing:
        path = path_of.get(key, f'octane/{key}/{key}')
        if spare:
            e = spare.pop(0)
        else:
            # A copy of the first entry and its mode table, with their fixups.
            pair = bytes(g.data[template:template + MAP_PAIR])
            e = g.append(pair)
            for tag in ('RVTB', 'RSTT', 'ROFS', 'RPID'):
                for site in list(g.sites[tag]):
                    if template <= site < template + MAP_PAIR:
                        g.add_site(tag, site - template + e)
            for site in [s for s in g.sites['ROFS'] if e <= s < e + MAP_PAIR]:
                target = g.u64(site)
                if template <= target < template + MAP_PAIR:
                    g.set_u64(site, target - template + e)
        g.set_u64(e + MAP_NAME, g.string_index(path))
        keep.append(e)
        added.append(key)
    for e in keep:
        g.set_u64(e + MAP_WEIGHT, weight)
    set_list(g, weighted, keep, 'ROFS', keep)
    set_list(g, maplist, keep, 'RSTT', [g.u64(e + MAP_NAME) for e in keep])
    names = [track_key(g.string_at(e + MAP_NAME)) for e in keep]
    return g.build(), {'tracks': names, 'added': added, 'weight': weight}


def build(game, out, weight):
    out = Path(out)
    archives = Path(game) / 'archives'
    target = out / 'online_playlist.bbpatch'
    stamp_file = out / 'online_playlist.json'
    paks = mod_file_paths.archives(archives)
    if not paks:
        print('Online playlist: no archives folder in the game; left as the game has it')
        target.unlink(missing_ok=True)
        return 0
    stamp = {'format': FORMAT, 'weight': weight,
             'archives': [[p.name, p.stat().st_size, p.stat().st_mtime_ns] for p in paks]}
    try:
        if target.is_file() and json.loads(stamp_file.read_text())['stamp'] == stamp:
            return 0
    except (OSError, ValueError, KeyError):
        pass
    for name in ARCHIVES:
        pak = mod_file_paths.resolve(archives / name)
        if not pak.is_file():
            continue
        entries = iga.pak_entries(pak)
        index = next((i for i, e in enumerate(entries) if e[0].lower() == ENTRY), None)
        if index is None:
            continue
        _name, off, length, mode = entries[index]
        with open(pak, 'rb') as f:
            blob = iga.read_entry(f, off, length, mode)
        new, report = rebuild(blob, game_tracks(archives), weight)
        Igz(new)  # it must read back
        out.mkdir(parents=True, exist_ok=True)
        archive_patch.write_entry_patch(target, pak, f'/app0/archives/{pak.name}', index, new)
        stamp_file.write_text(json.dumps({'stamp': stamp, 'report': report}))
        print(f'Online playlist: {len(report["tracks"])} race tracks at the same weight'
              + (f', added {", ".join(report["added"])}' if report['added'] else ''))
        return 0
    print('Online playlist: OnlineSystemData not found in the archives; left as the game has it')
    target.unlink(missing_ok=True)
    return 0


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game')
    p.add_argument('--out', required=True)
    p.add_argument('--weight', type=int, default=100)
    a = p.parse_args()
    try:
        sys.exit(build(a.game, a.out, a.weight))
    except Exception as error:  # the game runs with its own playlist rather than not at all
        print(f'Online playlist: not built ({error!r}); the game keeps its own')
        sys.exit(0)
