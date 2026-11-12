"""Archive patches: a game archive as the game should read it, the archive file itself unchanged.
The runtime (src/runtime_file.c, BB_ARCHIVE_PATCHES) applies one when the game opens the archive
named in it, and only while the archive still has the size it was made for.

    "BBPAK001", u64 archive size, u32 path length, guest path (/app0/archives/...),
    u32 count, then count x {u64 offset, u32 length, bytes}

A range below the archive's size replaces those bytes; ranges at or after it are added after its
end (anything between is zeros), and the game sees the archive that much longer.
"""
import os
import struct
from pathlib import Path
from mod_file_paths import resolve

MAGIC = b'BBPAK001'
SECTOR = 0x800
MAX_ARCHIVE_OVERLAYS = 128  # src/runtime_file.c: MAX_ARCHIVE_PATCHES


def write(target, archive, guest, ranges):
    archive = resolve(archive)
    size = Path(archive).stat().st_size
    path = guest.encode('utf-8')
    out = bytearray(MAGIC + struct.pack('<QI', size, len(path)) + path + struct.pack('<I', len(ranges)))
    for offset, data in sorted(ranges):
        out += struct.pack('<QI', offset, len(data)) + data
    target = Path(target)
    temp = target.with_suffix('.tmp')
    temp.write_bytes(out)
    os.replace(temp, target)


def read(path):
    """(archive size, guest path, [(offset, bytes)])."""
    blob = Path(path).read_bytes()
    if blob[:8] != MAGIC:
        raise ValueError('not an archive patch')
    size, n = struct.unpack_from('<QI', blob, 8)
    at = 20
    guest = blob[at:at + n].decode('utf-8')
    at += n
    count, = struct.unpack_from('<I', blob, at)
    at += 4
    ranges = []
    for _ in range(count):
        offset, length = struct.unpack_from('<QI', blob, at)
        at += 12
        ranges.append((offset, blob[at:at + length]))
        at += length
    return size, guest, ranges


def write_entry_patch(target, archive, guest, index, payload):
    """A patch that replaces entry index of an IGA v13 archive with payload, stored uncompressed
    after the end of the archive (on a sector boundary)."""
    write_entries_patch(target, archive, guest, {index: payload})


def write_entries_patch(target, archive, guest, entries, base=None, override=()):
    """A patch that replaces entries {index: payload} of an IGA v13 archive, each stored
    uncompressed after the end of the archive (on sector boundaries). base, another patch for the
    same archive (path), is kept in it: its ranges first, these entries after its data. The
    runtime takes one patch per archive, so two features changing one archive share a file.
    override names entries among them that base may already replace (its own old record and
    payload are dropped): the caller already read base's current bytes for those into its own
    payload, so this is a deliberate second replacement, not an accidental collision."""
    archive = resolve(archive)
    size = Path(archive).stat().st_size
    ranges, end = [], size
    if base is not None:
        base_size, base_guest, ranges = read(base)
        if base_size != size or base_guest.lower() != guest.lower():
            raise ValueError(f'{base} was made for another archive')
        end = max([size] + [o + len(d) for o, d in ranges])
    with open(archive, 'rb') as f:
        header = f.read(0x38)
        count, = struct.unpack_from('<I', header, 0xc)
        records = {}
        for index in entries:
            if not 0 <= index < count:
                raise ValueError('no such entry')
            f.seek(0x38 + 4 * count + 16 * index)
            records[index] = struct.unpack('<4I', f.read(16))
    if override:
        drop = {0x38 + 4 * count + 16 * index for index in override}
        ranges = [(o, d) for o, d in ranges if o not in drop]
    taken = {o for o, d in ranges if len(d) == 16}
    for index in sorted(entries):
        record_at = 0x38 + 4 * count + 16 * index
        if record_at in taken:
            raise ValueError(f'entry {index} is already replaced by {base}')
        payload = entries[index]
        position = (end + SECTOR - 1) // SECTOR * SECTOR
        _offset, extra, _length, _mode = records[index]
        record = struct.pack('<4I', position & 0xFFFFFFFF, (extra & 0xFFFFFF00) | (position >> 32),
                             len(payload), 0xFFFFFFFF)
        ranges += [(record_at, record), (position, payload)]
        end = position + len(payload)
    write(target, archive, guest, ranges)


def path_hash(path):
    """An entry's hash in an IGA v13 index: FNV-1a (32 bits) of its path in lower case."""
    h = 0x811C9DC5
    for c in path.lower().encode('utf-8'):
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


def write_grown_patch(target, archive, guest, entries, added):
    """A patch that replaces entries {index: payload} of an IGA v13 archive and adds new ones
    {path: payload}, each stored uncompressed after the end of the archive. The index (header,
    hashes in order, records, compression tables) is written again in place, which needs room
    before the first entry's data (20 bytes an entry; the entries whose data is in the way move
    after the end, uncompressed), and the names after the end. Entries keep their data but not
    their index numbers, so it is the only patch for its archive (no base)."""
    archive = resolve(archive)
    size = Path(archive).stat().st_size
    with open(archive, 'rb') as f:
        blob = f.read()
    u32 = lambda o: struct.unpack_from('<I', blob, o)[0]
    count, names_at, names_size = u32(0xC), struct.unpack_from('<Q', blob, 0x28)[0], u32(0x30)
    tables_size = u32(0x1C) * 4 + u32(0x20) * 2 + u32(0x24)
    if u32(0) != 0x1A414749 or u32(4) != 13 or u32(8) != count * 20 + tables_size:
        raise ValueError('not an IGA v13 archive index')
    hashes = struct.unpack_from(f'<{count}I', blob, 0x38)
    records = [struct.unpack_from('<4I', blob, 0x38 + 4 * count + 16 * i) for i in range(count)]
    tables = blob[0x38 + 20 * count:0x38 + 20 * count + tables_size]
    starts = sorted(set(struct.unpack_from(f'<{count}I', blob, names_at)))
    name_blocks = []
    for i in range(count):
        at = u32(names_at + 4 * i)
        later = [s for s in starts if s > at]
        name_blocks.append(blob[names_at + at:names_at + (later[0] if later else names_size)])
    paths = {name_blocks[i].split(b'\0')[1].decode('utf-8').lower() for i in range(count)}
    total = count + len(added)
    # Room for the longer index: the entries whose data comes first move after the end too.
    entries, reader = dict(entries), None
    position_of = lambda i: (records[i][1] & 0xFF) << 32 | records[i][0]
    while True:
        staying = [i for i in range(count) if i not in entries]
        if not staying:
            raise ValueError('no room in the archive index for the new entries')
        first = min(staying, key=position_of)
        if 0x38 + 20 * total + tables_size <= position_of(first):
            break
        if reader is None:
            from archive_reader import Archive
            reader = Archive(archive)
        entries[first] = bytes(reader.extract(reader.files[first]))
    end, ranges = size, []

    def place(payload):
        nonlocal end
        position = (end + SECTOR - 1) // SECTOR * SECTOR
        ranges.append((position, payload))
        end = position + len(payload)
        return position

    def record(position, payload, ordinal):
        return struct.pack('<4I', position & 0xFFFFFFFF, ordinal << 8 | position >> 32, len(payload), 0xFFFFFFFF)

    rows = []  # (hash, record, name block)
    for i in range(count):
        row = records[i]
        if i in entries:
            row = struct.unpack('<4I', record(place(entries[i]), entries[i], row[1] >> 8))
        rows.append((hashes[i], struct.pack('<4I', *row), name_blocks[i]))
    ordinal = max((r[1] >> 8 for r in records if r[1] >> 8 != 0xFFFFFF), default=-1) + 1
    for path, payload in added.items():
        if path.lower() in paths:
            raise ValueError(f'{path} is already in the archive')
        name = path.encode('utf-8')
        rows.append((path_hash(path), record(place(payload), payload, ordinal),
                     b'temporary/octane/data/ps4/output/' + name + b'\0' + name + b'\0' + bytes(4)))
        ordinal += 1
    rows.sort(key=lambda row: row[0])
    if len({row[0] for row in rows}) != total:
        raise ValueError('two entries with the same hash')
    names = bytearray(4 * total)
    for i, row in enumerate(rows):
        struct.pack_into('<I', names, 4 * i, len(names))
        names += row[2]
    names_position = place(bytes(names))
    divider = 0xFFFFFFFF // total
    slop = max(abs(i - row[0] // divider) for i, row in enumerate(rows))
    header = bytearray(blob[:0x38])
    struct.pack_into('<II', header, 0x8, total * 20 + tables_size, total)
    struct.pack_into('<II', header, 0x14, divider, slop)
    struct.pack_into('<QI', header, 0x28, names_position, len(names))
    index = bytes(header) + b''.join(struct.pack('<I', row[0]) for row in rows) \
        + b''.join(row[1] for row in rows) + tables
    write(target, archive, guest, [(0, index)] + ranges)


def view(archive, patch, offset, length):
    """What the game reads (for tests): the archive with the patch applied."""
    archive = resolve(archive)
    size, _guest, ranges = read(patch)
    end_virtual = max([size] + [o + len(d) for o, d in ranges])
    length = max(0, min(length, end_virtual - offset))
    out = bytearray(length)
    with open(archive, 'rb') as f:
        if offset < size:
            f.seek(offset)
            got = f.read(min(length, size - offset))
            out[:len(got)] = got
    for o, d in ranges:
        lo, hi = max(o, offset), min(o + len(d), offset + length)
        if lo < hi:
            out[lo - offset:hi - offset] = d[lo - o:hi - o]
    return bytes(out)
