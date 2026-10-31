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
