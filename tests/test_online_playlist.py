"""The online playlist's pieces that need no game files: packed IGZ offsets (scripts/igz_file.py)
and archive patches (scripts/archive_patch.py, applied by src/runtime_file.c)."""
from paths import ROOT
import struct
import tempfile
import unittest
from pathlib import Path
import archive_patch
import igz_file


class PackedOffsetTests(unittest.TestCase):
    def test_round_trip(self):
        for offsets in ([0], [8, 16, 24], [0, 4, 4096, 4100, 1 << 20], list(range(0, 4000, 8))):
            packed = igz_file.pack_offsets(offsets)
            self.assertEqual(igz_file.unpack_offsets(packed, len(offsets)), offsets)

    def test_rejects_unaligned(self):
        with self.assertRaises(ValueError):
            igz_file.pack_offsets([0, 6])

    def test_strings(self):
        names = ['octane/t505_nitro_festival/t505_nitro_festival', 'ab', 'abc', '']
        raw = igz_file.write_strings(names)
        self.assertEqual(igz_file.read_strings(raw, len(names)), names)


def iga(entries):
    """An IGA v13 archive with stored entries [(name, data)]."""
    n = len(entries)
    header = bytearray(0x38)
    struct.pack_into('<I', header, 0xc, n)
    start = 0x38 + 20 * n
    rows, blobs = bytearray(), bytearray()
    for _name, data in entries:
        rows += struct.pack('<IIII', start + len(blobs), 0x1200, len(data), 0xFFFFFFFF)
        blobs += data
    names = bytearray(4 * n)
    strings = bytearray()
    for i, (name, _data) in enumerate(entries):
        struct.pack_into('<I', names, 4 * i, 4 * n + len(strings))
        strings += name.encode() + b'\0'
    struct.pack_into('<QI', header, 0x28, start + len(blobs), len(names) + len(strings))
    return bytes(header) + bytes(4 * n) + bytes(rows) + bytes(blobs) + bytes(names) + bytes(strings)


class ArchivePatchTests(unittest.TestCase):
    def test_entry_patch_is_read_as_the_new_entry(self):
        import gcn_shaders as reader
        with tempfile.TemporaryDirectory() as tmp:
            pak = Path(tmp) / 'update2.pak'
            pak.write_bytes(iga([('a.igz', b'first'), ('b.igz', b'second entry')]))
            original = pak.read_bytes()
            patch = Path(tmp) / 'p.bbpatch'
            archive_patch.write_entry_patch(patch, pak, '/app0/archives/update2.pak', 1, b'NEW DATA!')
            self.assertEqual(pak.read_bytes(), original)  # the archive is never changed
            size, guest, ranges = archive_patch.read(patch)
            self.assertEqual((size, guest), (len(original), '/app0/archives/update2.pak'))
            # What the game sees: the patched index points past the end, at the new data.
            view = archive_patch.view(pak, patch, 0, 1 << 20)
            self.assertGreater(len(view), len(original))
            view_file = Path(tmp) / 'view.pak'
            view_file.write_bytes(view)
            entries = reader.pak_entries(view_file)
            self.assertEqual([e[0] for e in entries], ['a.igz', 'b.igz'])
            with open(view_file, 'rb') as f:
                self.assertEqual(reader.read_entry(f, *entries[0][1:]), b'first')
                self.assertEqual(reader.read_entry(f, *entries[1][1:]), b'NEW DATA!')
            self.assertEqual(entries[1][1] % archive_patch.SECTOR, 0)
            # The record keeps the bits above the offset's high byte.
            record = ranges[0][1]
            self.assertEqual(struct.unpack('<4I', record)[1] & 0xFFFFFF00, 0x1200)


if __name__ == '__main__':
    unittest.main()
