"""The seasonal title screen's pieces that need no game files (scripts/season_art.py): which theme
and Crash apply, the hand-set layout, and archive patches that add entries (scripts/archive_patch.py)."""
from paths import ROOT  # noqa: F401
import struct
import tempfile
import unittest
from pathlib import Path

import archive_patch
import season_art
from archive_reader import Archive


class ThemeTests(unittest.TestCase):
    def ini(self, text):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        path = Path(folder.name) / 'bbport.ini'
        path.write_text(text, encoding='utf-8')
        return path

    def test_server_season(self):
        config = self.ini('[bbport]\n')
        self.assertEqual(season_art.chosen_theme(config, {'BB_THEME': 'christmas'}), 'christmas')
        self.assertEqual(season_art.chosen_theme(config, {'BB_THEME': 'halloween'}), '')
        self.assertEqual(season_art.chosen_theme(config, {}), '')

    def test_player_choice_over_server(self):
        self.assertEqual(season_art.chosen_theme(self.ini('title_theme=off\n'), {'BB_THEME': 'christmas'}), '')
        self.assertEqual(season_art.chosen_theme(self.ini('title_theme=christmas\n'), {}), 'christmas')
        self.assertEqual(season_art.chosen_theme(self.ini('title_theme=off\n'),
                                                 {'BB_TITLE_THEME': 'christmas'}), 'christmas')

    def test_crash_follows_theme(self):
        config = self.ini('')
        self.assertEqual(season_art.chosen_crash(config, {}, 'christmas'), 'reindeer')
        self.assertEqual(season_art.chosen_crash(config, {}, ''), '')
        self.assertEqual(season_art.chosen_crash(self.ini('title_crash=off\n'), {}, 'christmas'), '')
        self.assertEqual(season_art.chosen_crash(self.ini('title_crash=reindeer\n'), {}, ''), 'reindeer')

    def test_layout_file(self):
        config = self.ini('')
        (config.parent / season_art.LAYOUT_FILE).write_text(
            '{"cupcakes": [{"cupcake": 2, "x": 1, "y": -2, "z": 3, "size": 1.5}], "snow": [[4, 5, 6]]}',
            encoding='utf-8')
        layout, text = season_art.decoration_layout(config)
        self.assertEqual(layout, (((1, 1.0, -2.0, 3.0, 1.5, 0.0),), ((4.0, 5.0, 6.0),)))
        self.assertTrue(text)
        (config.parent / season_art.LAYOUT_FILE).write_text('{"cupcakes": [{"cupcake": 3, "x": 0}]}',
                                                            encoding='utf-8')
        self.assertIsNone(season_art.decoration_layout(config)[0])


def iga(entries):
    """A small uncompressed IGA v13 archive of {path: data}, entries in hash order."""
    rows = sorted((archive_patch.path_hash(p), p, d) for p, d in entries.items())
    count = len(rows)
    first = (0x38 + 20 * count + 0x7FF) // 0x800 * 0x800 + 0x800
    data, offsets = bytearray(), []
    for _h, _p, d in rows:
        offsets.append(first + len(data))
        data += d + bytes((-len(d)) % 0x800)
    names = bytearray(4 * count)
    for i, (_h, p, _d) in enumerate(rows):
        struct.pack_into('<I', names, 4 * i, len(names))
        names += b'temporary/' + p.encode() + b'\0' + p.encode() + b'\0' + bytes(4)
    names_at = first + len(data)
    head = bytearray(0x38)
    struct.pack_into('<5I', head, 0, 0x1A414749, 13, count * 20, count, 0x800)
    struct.pack_into('<II', head, 0x14, 0xFFFFFFFF // count, 0)
    struct.pack_into('<QI', head, 0x28, names_at, len(names))
    index = bytes(head) + b''.join(struct.pack('<I', h) for h, _p, _d in rows) + b''.join(
        struct.pack('<4I', o, i << 8, len(d), 0xFFFFFFFF) for i, (o, (_h, _p, d)) in enumerate(zip(offsets, rows)))
    return index.ljust(first, b'\0') + bytes(data) + bytes(names)


class GrownPatchTests(unittest.TestCase):
    def test_adds_and_replaces_entries(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            source = folder / 'title.pak'
            source.write_bytes(iga({'a/one.igz': b'one', 'b/two.igz': b'two' * 10}))
            original = Archive(source)
            index = {f['path']: f['index'] for f in original.files}
            original.data.close()
            original._file.close()
            patch = folder / 'title.bbpatch'
            archive_patch.write_grown_patch(patch, source, '/app0/archives/title.pak',
                                            {index['b/two.igz']: b'TWO'}, {'c/Three.igz': b'three!'})
            size, _guest, ranges = archive_patch.read(patch)
            end = max(o + len(d) for o, d in ranges)
            seen = folder / 'seen.pak'
            seen.write_bytes(archive_patch.view(source, patch, 0, end))
            archive = Archive(seen)
            got = {f['path']: bytes(archive.extract(f)) for f in archive.files}
            archive.data.close()
            archive._file.close()  # Windows keeps an open file from being removed
            self.assertEqual(got, {'a/one.igz': b'one', 'b/two.igz': b'TWO', 'c/Three.igz': b'three!'})
            raw = seen.read_bytes()
            count, divider, slop = struct.unpack_from('<I', raw, 0xC)[0], *struct.unpack_from('<II', raw, 0x14)
            hashes = struct.unpack_from(f'<{count}I', raw, 0x38)
            self.assertEqual(list(hashes), sorted(hashes))
            for i, h in enumerate(hashes):
                self.assertLessEqual(abs(i - h // divider), slop)

    def test_refuses_a_second_copy(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'title.pak'
            source.write_bytes(iga({'a/one.igz': b'one'}))
            with self.assertRaises(ValueError):
                archive_patch.write_grown_patch(Path(folder) / 'p', source, '/app0/archives/title.pak', {},
                                                {'A/One.igz': b'x'})


if __name__ == '__main__':
    unittest.main()
