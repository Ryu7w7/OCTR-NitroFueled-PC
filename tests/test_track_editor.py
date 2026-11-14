"""track_editor.py: reference counts of map files made right (CTR Track Editor tracks before v0.5),
and the files an editor install leaves in the game folder. Synthetic files only (no game data)."""
import shutil
import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

import paths  # noqa: F401
import content_convert
import track_editor as te
from graphics_tab import Archive, write_archive
from igz_merge import File
from test_racer_registry import SIZES, Builder

SIZES.setdefault('CSharedUnlock', 0x10)
SIZES.setdefault('igNameList', 0x28)


def map_file(counts):
    """An IGZ: a list holding the shared object twice and a single object once; `counts` are the
    reference counts written for (list, shared, single)."""
    b = Builder(['igObjectList', 'CSharedUnlock'])
    shared, single = b.obj('CSharedUnlock'), b.obj('CSharedUnlock')
    lst = b.lst('igObjectList', [shared, shared, single])
    for o, n in zip((lst, shared, single), counts):
        struct.pack_into('<I', b.data, o + 8, n)
    return b.build(), (lst, shared, single)


def reached_file(shared_count, single_count):
    """An IGZ whose root list holds the shared object and a single one, and whose loose list (nothing
    reaches it, as Neo Bowser City's spline points) holds the shared object twice more."""
    b = Builder(['igObjectList', 'igNameList', 'CSharedUnlock'])
    shared, single = b.obj('CSharedUnlock'), b.obj('CSharedUnlock')
    root = b.lst('igObjectList', [shared, single])
    names = b.lst('igNameList', ['Shared', 'Single'], names=True)
    b.lst('igObjectList', [shared, shared])
    struct.pack_into('<I', b.data, shared + 8, shared_count)
    struct.pack_into('<I', b.data, single + 8, single_count)
    return b.build(root, names), (shared, single)


def counts(blob, objects):
    z = File(blob)
    return [z.u32(o + 8) for o in objects]


def read(path):
    a = Archive(Path(path))
    try:
        return {a.logical(i): a.read(i) for i in range(len(a.names))}
    finally:
        a.f.close()


class CountTests(unittest.TestCase):
    def test_counts_become_the_pointers_to_each_object(self):
        blob, objects = map_file((7, 18, 0))
        fixed, n = te.fix_counts(blob)
        # the list has no pointer to it (a root list): left as it is
        self.assertEqual(counts(fixed, objects), [7, 2, 1])
        self.assertEqual(n, 2)
        self.assertEqual(te.fix_counts(fixed), (fixed, 0))

    def test_a_track_keeping_counts_of_reached_pointers_keeps_that_rule(self):
        # the shared object's count, 1, is the pointers from reached places, not all 3
        blob, objects = reached_file(1, 5)
        self.assertEqual(te.rule_of([blob]), te.REACHED)
        fixed, n = te.fix_counts(blob)
        self.assertEqual((counts(fixed, objects), n), ([1, 1], 1))
        # the same file under the editor's rule (the track's other files decide)
        fixed, n = te.fix_counts(blob, te.ALL)
        self.assertEqual(counts(fixed, objects), [3, 1])

    def test_files_that_are_not_igz_are_left_alone(self):
        self.assertEqual(te.fix_counts(b'not an igz'), (b'not an igz', 0))

    def test_only_map_files_of_an_archive_are_fixed_in_place(self):
        root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, root, ignore_errors=True)
        wrong, objects = map_file((1, 5, 3))
        right, _ = map_file((1, 2, 1))
        entries = {'maps/octane/t901_x/t901_x_art.igz': wrong, 'Maps/Octane/T901_X/T901_X_Ok.igz': right,
                   'vsc/octane/t901_x/script.igz': wrong, 'textures/x.igz': b'texture bytes'}
        pak = root / 't901_x.pak'
        write_archive(pak, [('t/' + p, p, i << 8, d) for i, (p, d) in enumerate(entries.items())],
                      SimpleNamespace(flags=0))
        self.assertEqual(te.wrong_counts(pak), ({'maps/octane/t901_x/t901_x_art.igz': 2}, te.ALL))
        size = pak.stat().st_size
        self.assertEqual(te.fix_archive(pak), {'maps/octane/t901_x/t901_x_art.igz': 2})
        after = read(pak)
        self.assertEqual(counts(after['maps/octane/t901_x/t901_x_art.igz'], objects), [1, 2, 1])
        for path in ('Maps/Octane/T901_X/T901_X_Ok.igz', 'vsc/octane/t901_x/script.igz', 'textures/x.igz'):
            self.assertEqual(after[path], entries[path])  # scripts keep their counts on purpose
        self.assertGreater(pak.stat().st_size, size)  # appended, the rest as it was
        self.assertEqual(te.fix_archive(pak), {})


class TraceTests(unittest.TestCase):
    def setUp(self):
        self.game = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.game, ignore_errors=True)
        (self.game / 'archives').mkdir()
        for name in ('update.pak', 'update2.pak', 't111_crash_cove.pak'):
            (self.game / 'archives' / name).write_bytes(b'x')

    def test_a_game_as_dumped_has_no_traces(self):
        self.assertEqual(te.traces(self.game), [])
        self.assertIsNone(te.notice(self.game))

    def test_an_editor_install_is_named(self):
        for name in ('t903_custom_map.pak', 'update3.pak', 'update.pak.ctrmod.json', 'ctr_installed_tracks.json'):
            (self.game / 'archives' / name).write_bytes(b'x')
        (self.game / 'chunks.igx.ctrmod').write_bytes(b'x')
        self.assertEqual(te.traces(self.game), ['archives/ctr_installed_tracks.json', 'archives/t903_custom_map.pak',
                                                'archives/update.pak.ctrmod.json', 'archives/update3.pak',
                                                'chunks.igx.ctrmod'])
        self.assertIn('CTR Track Editor', te.notice(self.game))

    def test_no_conversion_against_a_changed_game(self):
        (self.game / 'archives' / 'update.pak.ctrmod.json').write_bytes(b'{}')
        with self.assertRaisesRegex(ValueError, 'restore the game folder'):
            content_convert.convert(self.game, self.game / 'missing.zip', self.game / 'out')


if __name__ == '__main__':
    unittest.main()
