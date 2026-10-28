"""Alchemy IGZ (version 10, PS4) files: read, change and write them back.

    header      u32 magic 0x49475A01, u32 version 10, u32 type hash, u32 platform, u32 fixup count,
                then section descriptors {u32 0, u32 offset, u32 length, u32 alignment} up to 0x800
    section 0   fixups: {char[4] tag, u32 count, u32 length, u32 data start} + data, one after another
    section 1   the objects; a reference inside it is the byte offset from the section's start

Fixups this needs:
    TSTR        the strings (each NUL-terminated, the next one starting on an even offset)
    TMET        the type names (same layout); an object's first 8 bytes hold its type's index
    MTSZ        u32 size of each type
    RVTB        where objects start; RSTT where string indices are; ROFS where references are;
    RPID        where memory references are (packed: see pack_offsets)
Other fixups are kept as they are.
"""
import struct

MAGIC = 0x49475A01
STRING_LISTS = ('TSTR', 'TMET', 'EXNM', 'TDEP')
PACKED = ('RVTB', 'ROFS', 'RSTT', 'RPID', 'RHND', 'RNEX', 'REXT', 'ROOT', 'ONAM', 'RMHN')


def unpack_offsets(data, count):
    """Packed offset list: nibbles (low first), 3 value bits + a continuation bit, little-endian
    groups; the first value is an offset / 4, each next one the distance from the previous / 4."""
    nibbles = []
    for b in data:
        nibbles += (b & 15, b >> 4)
    out, prev, i = [], 0, 0
    for _ in range(count):
        v, shift = 0, 0
        while True:
            n = nibbles[i]
            i += 1
            v |= (n & 7) << shift
            shift += 3
            if not n & 8:
                break
        prev = prev + v * 4 if out else v * 4
        out.append(prev)
    return out


def pack_offsets(offsets):
    nibbles, prev = [], 0
    for k, off in enumerate(offsets):
        delta = (off - prev) // 4 if k else off // 4
        if off % 4 or delta < 0 or (k and delta == 0 and off != prev):
            raise ValueError('offsets must be ascending multiples of 4')
        prev = off
        while True:
            n = delta & 7
            delta >>= 3
            nibbles.append(n | (8 if delta else 0))
            if not delta:
                break
    if len(nibbles) % 2:
        nibbles.append(0)
    return bytes(nibbles[i] | nibbles[i + 1] << 4 for i in range(0, len(nibbles), 2))


def read_strings(data, count):
    out, p = [], 0
    for _ in range(count):
        end = data.index(b'\0', p)
        out.append(data[p:end].decode('utf-8'))
        p = end + 1
        if p & 1:
            p += 1
    return out


def write_strings(strings):
    out = bytearray()
    for s in strings:
        out += s.encode('utf-8') + b'\0'
        if len(out) & 1:
            out += b'\0'
    return bytes(out)


class Igz:
    def __init__(self, blob):
        self.blob = bytes(blob)
        magic, self.version, self.type_hash, self.platform, count = struct.unpack_from('<5I', blob, 0)
        if magic != MAGIC or self.version != 10:
            raise ValueError('not an IGZ version 10 file')
        self.sections = []
        at = 0x14
        while at + 16 <= 0x800:
            zero, off, length, align = struct.unpack_from('<4I', blob, at)
            if off == 0 and length == 0:
                break
            self.sections.append([zero, off, length, align])
            at += 16
        if len(self.sections) != 2:
            raise ValueError('expected a fixup section and one data section')
        self.fixups = []  # [tag, count, start, raw data] in file order
        p = self.sections[0][1]
        for _ in range(count):
            tag, n, length, start = struct.unpack_from('<4sIII', blob, p)
            self.fixups.append([tag.decode('ascii'), n, start, bytes(blob[p + start:p + length]), length])
            p += length
        off, length = self.sections[1][1], self.sections[1][2]
        self.data = bytearray(blob[off:off + length])
        self.strings = self.read_list('TSTR', read_strings)
        self.types = self.read_list('TMET', read_strings)
        self.sizes = list(struct.unpack(f'<{self.fixup("TMET")[1]}I', self.fixup('MTSZ')[3][:4 * self.fixup('TMET')[1]]))
        # A file without references of a kind (the .lng files have no RPID) has no such fixup.
        present = {f[0] for f in self.fixups}
        self.sites = {tag: self.read_list(tag, unpack_offsets)
                      for tag in ('RVTB', 'RSTT', 'ROFS', 'RPID') if tag in present}

    def fixup(self, tag):
        for f in self.fixups:
            if f[0] == tag:
                return f
        raise KeyError(tag)

    def read_list(self, tag, reader):
        f = self.fixup(tag)
        return reader(f[3], f[1])

    # --- objects ---------------------------------------------------------------------------
    def objects(self):
        """[(offset, type name)] in file order."""
        return [(off, self.types[self.u64(off)]) for off in self.sites['RVTB']]

    def u64(self, off):
        return struct.unpack_from('<Q', self.data, off)[0]

    def set_u64(self, off, value):
        struct.pack_into('<Q', self.data, off, value)

    def u32(self, off):
        return struct.unpack_from('<I', self.data, off)[0]

    def set_u32(self, off, value):
        struct.pack_into('<I', self.data, off, value)

    def string_at(self, off):
        return self.strings[self.u64(off)]

    def string_index(self, s):
        """The index of string s, adding it when missing."""
        if s not in self.strings:
            self.strings.append(s)
        return self.strings.index(s)

    def append(self, raw, align=8):
        """Raw bytes added at the end of the data section: their offset."""
        while len(self.data) % align:
            self.data.append(0)
        off = len(self.data)
        self.data += raw
        return off

    def add_site(self, tag, off):
        if off not in self.sites[tag]:
            self.sites[tag].append(off)
            self.sites[tag].sort()

    # --- writing ---------------------------------------------------------------------------
    def build(self):
        fixups = bytearray()
        for tag, count, start, raw, length in self.fixups:
            if tag == 'TSTR':
                count, raw = len(self.strings), write_strings(self.strings)
            elif tag in self.sites:
                count, raw = len(self.sites[tag]), pack_offsets(self.sites[tag])
            new_length = start + len(raw)
            # Blocks keep 4-byte alignment, as the files have them.
            pad = (-new_length) % 4
            if tag not in ('TSTR', *self.sites) and new_length + pad != length:
                pad = length - new_length  # an untouched block keeps its exact length
            block = struct.pack('<4sIII', tag.encode('ascii'), count, new_length + pad, start)
            block += bytes(start - 16) + raw + bytes(pad)
            fixups += block
        header = bytearray(self.blob[:0x800])
        data_off = 0x800 + len(fixups)
        struct.pack_into('<4I', header, 0x14, self.sections[0][0], 0x800, len(fixups), self.sections[0][3])
        struct.pack_into('<4I', header, 0x24, self.sections[1][0], data_off, len(self.data), self.sections[1][3])
        return bytes(header + fixups + self.data)
