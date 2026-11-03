"""Export local Time Trial ghost references for AI guidance without modifying game data.

Native v3 recording: fcbec0 positions/angles, fcc140 input snapshots; times
are unsigned 16-bit centiseconds. IGB serializes each short in a 32-bit slot;
the upper half is not part of the value. Never infer chronological order from
object serialization order. Only the supported, single-ghost schema is read.
"""
import argparse
import bisect
import csv
import json
import math
import struct
from pathlib import Path

from igz_file import Igz
import gcn_shaders as iga

TYPES = (
    '__internalObjectBase', 'igObject', 'igNamedObject', 'igDirEntry',
    'igObjectDirEntry', 'igMemoryDirEntry', 'igExternalDirEntry',
    'igExternalIndexedEntry', 'igExternalInfoEntry', 'igExternalObjectEntry',
    'CGhostMoveKey', 'igContainer', 'igDataList', 'igObjectList',
    'CGhostMoveKeyList', 'CGhostSnapshotKey', 'CGhost', 'CGhostSnapshotKeyList',
    'igInfoList', 'igReferenceResolver', 'igInfo', 'igObjectDirectoryIGBInfo', 'igNameList',
)


def decode(blob):
    if len(blob) < 48:
        raise ValueError('truncated IGB header')
    header = struct.unpack_from('<12I', blob)
    if header[:2] != (0x134, 13) or header[3] != len(TYPES):
        raise ValueError('unsupported ghost IGB schema')
    size, count, tail = header[4:7]
    end = len(blob) - tail
    start = end - size
    if start < 48 or count > size // 8 or tail % 4:
        raise ValueError('invalid IGB section bounds')
    # The type descriptors precede the object section. The index order varies
    # between files, even between two ghosts on the same track.
    prefix = blob.find(b'__internalObjectBase\0', 48, start)
    if prefix < 0:
        raise ValueError('missing IGB type descriptors')
    located = []
    for name in TYPES:
        needle = name.encode() + b'\0'
        at = blob.find(needle, prefix, start)
        if at < 0 or blob.find(needle, at + len(needle), start) >= 0:
            raise ValueError('missing or ambiguous ghost type: ' + name)
        located.append((at, name))
    types = [name for _, name in sorted(located)]
    objects = []
    at = start
    for _ in range(count):
        if at + 8 > end:
            raise ValueError('truncated IGB object header')
        kind, length = struct.unpack_from('<II', blob, at)
        if kind >= len(types) or length < 8 or at + length > end:
            raise ValueError('invalid IGB object record')
        objects.append((types[kind], blob[at + 8:at + length]))
        at += length
    if at != end:
        raise ValueError('IGB object count/size mismatch')
    roots = [data for kind, data in objects if kind == 'CGhost']
    if len(roots) != 1 or len(roots[0]) != 24:
        raise ValueError('expected one v3 ghost')
    version, _, _, finish, moves_id, snapshots_id = struct.unpack('<III f II', roots[0])
    if version != 3 or not math.isfinite(finish) or not 0 < finish < 600:
        raise ValueError('unsupported ghost version/time')
    memories = {}
    for kind, data in objects:
        if kind in ('CGhostMoveKeyList', 'CGhostSnapshotKeyList', 'igObjectList',
                    'igInfoList', 'igNameList'):
            if len(data) != 12:
                raise ValueError('invalid ghost list record')
            length, capacity, reference = struct.unpack('<3I', data)
            if length > capacity or reference in memories or reference >= count + header[7]:
                raise ValueError('invalid ghost memory reference')
            memories[reference] = (kind, length)
    if len(memories) != header[7]:
        raise ValueError('ghost memory count mismatch')

    def object_at(reference):
        index = reference - sum(m < reference for m in memories)
        if reference in memories or not 0 <= index < len(objects):
            raise ValueError('invalid ghost object reference')
        return objects[index]

    ordered = {}
    for reference, (kind, length) in sorted(memories.items()):
        width = 8 if kind == 'igNameList' else 4
        if at + length * width > len(blob):
            raise ValueError('truncated ghost list memory')
        if kind in ('CGhostMoveKeyList', 'CGhostSnapshotKeyList'):
            if kind in ordered:
                raise ValueError('multiple ghost key lists')
            ordered[kind] = [object_at(v) for (v,) in struct.iter_unpack(
                '<I', blob[at:at + length * width])]
        at += length * width
    if at != len(blob):
        raise ValueError('ghost memory size mismatch')
    if object_at(moves_id)[0] != 'CGhostMoveKeyList' or object_at(snapshots_id)[0] != 'CGhostSnapshotKeyList':
        raise ValueError('invalid ghost root list reference')
    moves, snapshots = [], []
    for kind, data in ordered['CGhostMoveKeyList'] + ordered['CGhostSnapshotKeyList']:
        if kind == 'CGhostMoveKey':
            if len(data) != 28:
                raise ValueError('invalid move key size')
            x, y, z, time, ax, ay, az = struct.unpack('<3f4I', data)
            if not all(math.isfinite(v) for v in (x, y, z)):
                raise ValueError('nonfinite ghost position')
            # Native angle multiplier is 65535/(2*pi). Preserve signed angles.
            angles = [((v & 65535) - (65536 if v & 32768 else 0)) / 10430.0595703125
                      for v in (ax, ay, az)]
            moves.append(((time & 65535) / 100, x, y, z, *angles))
        elif kind == 'CGhostSnapshotKey':
            if len(data) != 8:
                raise ValueError('invalid snapshot key size')
            bits, time = struct.unpack('<II', data)
            bits &= 65535
            turn = (bits & 255) - (256 if bits & 128 else 0)
            snapshots.append(((time & 65535) / 100, turn,
                              *((bits >> bit) & 1 for bit in range(8, 15))))
        else:
            raise ValueError('unexpected object in ghost key list')
    for rows in (moves, snapshots):
        if not rows or any(a[0] > b[0] for a, b in zip(rows, rows[1:])):
            raise ValueError('empty or wrapped ghost timeline')
    starts = [row[0] for row in snapshots if row[-1]]
    if len(starts) != 1:
        raise ValueError('expected one race-start snapshot')
    return {'finish_seconds': finish, 'race_start_seconds': starts[0],
            'moves': moves, 'snapshots': snapshots}


def tt_entries(blob, tier=3):
    g = Igz(blob)
    objects = dict(g.objects())
    result = []
    for off, kind in objects.items():
        if kind != 'CChallengeGhostData':
            continue
        track = g.string_at(off + 0x10)
        listing = g.u64(off + 0x20)
        if not listing:
            continue
        if objects.get(listing) != 'CGhostHeaderDataList':
            raise ValueError('unexpected challenge ghost list')
        count, array = g.u32(listing + 12), g.u64(listing + 32)
        if count < 4:
            continue
        header = g.u64(array + tier * 8)  # Tropy, Oxide, Velo, Developer.
        if objects.get(header) != 'CChallengeGhostHeaderData':
            raise ValueError('unexpected TT ghost header')
        filename = g.string_at(header + 0x28).replace('\\', '/').split('/')[-1]
        if not filename.endswith('.igb') or filename in ('.', '..'):
            raise ValueError('invalid TT ghost filename')
        result.append((track, filename))
    return result


def reference_binary(entries, decoded):
    """Spatial guidance samples; snapshot order is retained at equal timestamps."""
    out = bytearray(b'BBGHOST1' + struct.pack('<I', len(entries)))
    for track, filename in entries:
        data = decoded[filename]
        name = track.lower().split('/')[-1].encode('ascii')
        if not name or len(name) >= 64 or any(c not in b'abcdefghijklmnopqrstuvwxyz0123456789_' for c in name):
            raise ValueError('invalid reference track name')
        snaps = data['snapshots']
        times = [row[0] for row in snaps]
        moves = [row for row in data['moves'] if row[0] >= data['race_start_seconds']]
        # Keep the original takeoff timestamp instead of snapping it to a
        # movement key. Interpolate only position; never invent an input edge.
        original_times = [row[0] for row in moves]
        inserted = {}
        for j in range(1, len(snaps)):
            time = snaps[j][0]
            if not original_times or not any(snaps[j][field] and not snaps[j-1][field]
                                             for field in (5, 6, 7)):
                continue
            at = bisect.bisect_left(original_times, time)
            if at == 0 or at == len(moves) or original_times[at] == time:
                continue
            a, b = moves[at-1], moves[at]
            fraction = (time-a[0])/(b[0]-a[0])
            inserted[time] = (time, *(a[k]+fraction*(b[k]-a[k]) for k in range(1,4)))
        moves = sorted(moves + list(inserted.values()), key=lambda row: row[0])
        if len(moves) > 4096:
            raise ValueError('too many spatial ghost samples')
        out += name.ljust(64, b'\0') + struct.pack('<I', len(moves))
        move_times = [row[0] for row in moves]
        edges = {}
        for j in range(1, len(snaps)):
            for field, flag in ((5, 2), (6, 8), (7, 8192)):
                if not snaps[j][field] or snaps[j - 1][field] or not move_times:
                    continue
                at = bisect.bisect_left(move_times, snaps[j][0])
                candidates = [i for i in (at - 1, at) if 0 <= i < len(moves)]
                closest = min(candidates, key=lambda i: abs(move_times[i] - snaps[j][0]))
                if abs(move_times[closest] - snaps[j][0]) <= .10:
                    edges[closest] = edges.get(closest, 0) | flag
        # A shoulder hold can be a plain hop. Only a hold containing the
        # recorded start_drift transition denotes a powerslide. Its entry
        # direction stays fixed when the ghost later countersteers.
        slides = [0] * len(snaps)
        begin = 0
        while begin < len(snaps):
            if not snaps[begin][5] or snaps[begin][3] or snaps[begin][4]:
                begin += 1
                continue
            end = begin + 1
            while end < len(snaps) and snaps[end][5] and not snaps[end][3] and not snaps[end][4]:
                end += 1
            entry = next((j for j in range(begin, end) if snaps[j][7]), None)
            if entry is not None:
                turn = snaps[entry][1]
                direction = ((128 if turn > 0 else 256) | ((turn & 255) << 16)) if abs(turn) > 16 else 0
                for j in range(begin, end):
                    slides[j] = 1 | direction
            begin = end
        for move_index, (time, x, y, z, *_) in enumerate(moves):
            current = max(0, bisect.bisect_right(times, time) - 1)
            snapshot = snaps[current]
            flags = slides[current]
            if snapshot[3] and snapshot[4] and not snapshot[2] and abs(snapshot[1]) >= 64:
                flags |= 16 | (32 if snapshot[1] > 0 else 0) | (64 if snapshot[5] else 0)
            # One spatial marker per edge; a broad window triggers before the lip.
            flags |= edges.get(move_index, 0)
            out += struct.pack('<4fI', time, x, y, z, flags)
    return out


def export(game, out):
    game, out = Path(game), Path(out)
    pak = game / 'archives/update2.pak'
    entry = next(e for e in iga.pak_entries(pak) if e[0].lower().endswith('/loosedata/ghostdata.igz'))
    with pak.open('rb') as f:
        blob = iga.read_entry(f, *entry[1:])
        entries = [(track + "__" + tier, filename) for index, tier in enumerate(("tropy", "oxide", "velo", "developer"))
                   for track, filename in tt_entries(blob, index)]
    decoded = {filename: decode((game / 'ghosts' / filename).read_bytes())
               for _, filename in entries}
    out.mkdir(parents=True, exist_ok=True)
    binary = out / 'reference.bin'
    temporary = binary.with_suffix('.tmp')
    temporary.write_bytes(reference_binary(entries, decoded))
    temporary.replace(binary)
    manifest = []
    for track, filename in entries:
        data = decoded[filename]
        key = track.lower().split('/')[-1]
        for kind, headings in (
            ('moves', ('seconds', 'x', 'y', 'z', 'angle_x', 'angle_y', 'angle_z')),
            ('snapshots', ('seconds', 'turn_raw', 'accelerate', 'reverse', 'brake',
                           'first_jump', 'second_jump', 'start_drift', 'race_start'))):
            with (out / (key + '-' + kind + '.csv')).open('w', newline='') as f:
                writer = csv.writer(f)
                writer.writerow(headings)
                writer.writerows(data[kind])
        manifest.append({'track': track, 'source': filename,
                         'finish_seconds': data['finish_seconds'],
                         'race_start_seconds': data['race_start_seconds'],
                         'move_keys': len(data['moves']), 'snapshots': len(data['snapshots'])})
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def source_stamp(game):
    """What the references are made from: this script and the game's archives (path, size, time),
    so a launch with the same game reuses them (the export takes seconds and writes ~50 MB)."""
    import hashlib
    archives = sorted((Path(game) / 'archives').glob('*.pak'))
    return {'script': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'game': str(Path(game).resolve()),
            'archives': [[p.name, p.stat().st_size, p.stat().st_mtime_ns] for p in archives]}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    stamp_file = args.out / 'stamp.json'
    stamp = source_stamp(args.game)
    try:
        current = json.loads(stamp_file.read_text()) == stamp and (args.out / 'reference.bin').is_file()
    except (OSError, ValueError):
        current = False
    if current:
        print('Time Trial references: up to date')
    else:
        stamp_file.unlink(missing_ok=True)
        manifest = export(args.game, args.out)
        stamp_file.write_text(json.dumps(stamp))
        print(f"Extracted {len(manifest)} track references; no game files changed")
