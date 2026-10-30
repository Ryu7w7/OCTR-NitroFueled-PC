"""Read native CUP tracks and the scoring table from a local CTR 1.21 dump.

This is a read-only research tool, not an online CUP implementation.
"""
import argparse
import json
from pathlib import Path
import struct

import gcn_shaders as iga
from igz_file import Igz
from mod_file_paths import resolve

ENTRY = 'temporary/octane/data/ps4/output/loosedata/systemdata.igz'


def inspect(blob):
    g = Igz(blob)
    objects = dict(g.objects())

    def require(at, kind):
        if objects.get(at) != kind:
            raise ValueError(f'expected {kind} at {at:#x}')
        return at

    def reference(at, kind):
        if at not in g.sites['ROFS']:
            raise ValueError(f'missing object reference at {at:#x}')
        return require(g.u64(at), kind)

    def items(at, width):
        count, start = g.u32(at + 12), g.u64(at + 32)
        if count > g.u32(at + 16) or start + count * width > len(g.data):
            raise ValueError('invalid native CUP array')
        return range(start, start + count * width, width)

    systems = [o for o, t in objects.items() if t == 'CSystemData']
    if len(systems) != 1:
        raise ValueError('expected one CSystemData')
    system = systems[0]
    points = reference(system + 0x180, 'CCupPositionPointsData')
    cups = reference(system + 0x188, 'COctaneCupSelectDataListList')
    result = []
    for site in items(cups, 8):
        cup = reference(site, 'COctaneCupSelectDataList')
        tracks = []
        for entry in items(cup, 8):
            track = reference(entry, 'COctaneCupSelectData')
            if track + 16 not in g.sites['RSTT']:
                raise ValueError('missing native CUP track string')
            tracks.append(g.string_at(track + 16))
        if len(tracks) != 4:
            raise ValueError('expected four tracks per native CUP')
        if cup + 0x28 not in g.sites['RSTT']:
            raise ValueError('missing native CUP name')
        result.append({'name': g.string_at(cup + 0x28),
                       'gem_cup': bool(g.u32(cup + 0x38)), 'tracks': tracks})
    return {'position_points': [g.u32(at) for at in items(points, 4)], 'cups': result}


def read_game(game):
    for name in ('update2.pak', 'update.pak', 'permanent.pak'):
        pak = resolve(Path(game) / 'archives' / name)
        if not pak.is_file():
            continue
        entry = next((e for e in iga.pak_entries(pak) if e[0].lower() == ENTRY), None)
        if entry:
            with pak.open('rb') as f:
                return iga.read_entry(f, *entry[1:])
    raise ValueError('native SystemData.igz is missing')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(inspect(read_game(args.game)), indent=2))
    except (OSError, ValueError, IndexError, KeyError, struct.error) as error:
        parser.exit(1, f'Native CUP inspection failed: {error}\n')
