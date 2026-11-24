"""Built-in mods next to community mods (scripts/builtin_coexist.py, scripts/igz_rebase.py): a
built-in's change merged over another mod's version of a file, else the built-in steps aside.
The files are built here (no game data)."""
import json
import struct
import tempfile
import unittest
from pathlib import Path

import paths  # noqa: F401
import builtin_coexist as bc
import igz_rebase
from graphics_tab import write_archive
from igz_merge import File
from mod_components import sha_bytes
from racer_registry import PREFIX, read_archive
from test_racer_registry import SIZES, Builder

SIZES.setdefault('COutfitMaterialPackage', 0x18)
TYPES = ['igObjectList', 'igNameList', 'COutfitMaterialPackageList', 'COutfitMaterialPackage']
ASSET = 'ui/Menu.igz'


def menu(points=('1', '2'), label='Hello', extra=None, orphan=None, extra_in_points=False):
    """A file with named objects: Points (a list of objects with a text), Label; Extra when
    given (one of Points' items too with extra_in_points); an object nothing refers to when orphan
    is given."""
    b = Builder(TYPES)
    named = []
    items = []
    for value in points:
        p = b.obj('COutfitMaterialPackage')
        b.string(p + 0x10, value)
        items.append(p)
    e = None
    if extra is not None:
        e = b.obj('COutfitMaterialPackage')
        b.string(e + 0x10, extra)
        if extra_in_points:
            items.append(e)
    named.append(('Points', b.lst('COutfitMaterialPackageList', items)))
    lab = b.obj('COutfitMaterialPackage')
    b.string(lab + 0x10, label)
    named.append(('Label', lab))
    if e is not None:
        named.append(('Extra', e))
    if orphan is not None:
        o = b.obj('COutfitMaterialPackage')
        b.string(o + 0x10, orphan)
    names = b.lst('igNameList', [n for n, _ in named], names=True)
    root = b.lst('igObjectList', [o for _, o in named])
    return b.build(root, names)


def read(blob):
    """{name: text or [texts]} of a menu()."""
    f = File(blob)
    out = {}
    for name, off in f.roots()[2]:
        if f.typed[off] == 'COutfitMaterialPackageList':
            out[name] = [f.strings[f.u64(i + 0x10)] for i in f.items(off)]
        else:
            out[name] = f.strings[f.u64(off + 0x10)]
    return out


class RebaseTests(unittest.TestCase):
    def test_changes_of_both_kept(self):
        merged = igz_rebase.rebase(menu(), menu(points=('1', '2', '3')), menu(label='Hola'))
        self.assertEqual(read(merged), {'Points': ['1', '2', '3'], 'Label': 'Hola'})

    def test_added_object(self):
        merged = igz_rebase.rebase(menu(), menu(extra='new'), menu(label='Hola'))
        self.assertEqual(read(merged), {'Points': ['1', '2'], 'Label': 'Hola', 'Extra': 'new'})

    def test_added_object_a_changed_one_refers_to(self):
        # a placeable table with a new named child: the child is added once, by its name
        ours = menu(extra='new', extra_in_points=True)
        merged = igz_rebase.rebase(menu(), ours, menu(label='Hola'))
        self.assertEqual(read(merged), {'Points': ['1', '2', 'new'], 'Label': 'Hola', 'Extra': 'new'})
        f = File(merged)
        extra = dict((n, o) for n, o in f.roots()[2])['Extra']
        self.assertEqual(f.items(dict((n, o) for n, o in f.roots()[2])['Points'])[-1], extra)

    def test_same_change_twice(self):
        ours = menu(points=('1', '2', '3'))
        self.assertEqual(read(igz_rebase.rebase(menu(), ours, ours)), read(ours))

    def test_changed_by_both(self):
        with self.assertRaisesRegex(igz_rebase.Unmergeable, 'Points is changed by both'):
            igz_rebase.rebase(menu(), menu(points=('1', '2', '3')), menu(points=('9',)))

    def test_change_no_named_object_reaches(self):
        with self.assertRaisesRegex(igz_rebase.Unmergeable, 'no named object reaches'):
            igz_rebase.rebase(menu(orphan='a'), menu(orphan='b'), menu(orphan='a', label='Hola'))


class Mods:
    """A game folder with archives/update2.pak holding stock entries, and package folders."""

    def __init__(self, root):
        self.root = Path(root)
        (self.root / 'game/archives').mkdir(parents=True)

    def archive(self, name, entries, where='game/archives'):
        path = self.root / where / name
        path.parent.mkdir(parents=True, exist_ok=True)
        template = type('T', (), {'flags': 0})()
        write_archive(path, [(PREFIX + p, p, i << 8, d) for i, (p, d) in enumerate(entries.items())], template)
        return path

    def op(self, mod, stock, data, asset=ASSET, index=0):
        folder = self.root / mod
        folder.mkdir(exist_ok=True)
        payload = sha_bytes(data)[:12] + '.bin'
        (folder / payload).write_bytes(data)
        return ({'kind': 'archive_asset', 'archive': 'update2.pak', 'asset': asset, 'index': index,
                 'original_sha256': sha_bytes(stock), 'patched_sha256': sha_bytes(data), 'payload': payload}, folder)


class MergeEntriesTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.mods = Mods(self.temporary.name)
        self.stock = menu()
        self.mods.archive('update2.pak', {ASSET: self.stock})
        self.game = self.mods.root / 'game'
        self.logged = []

    def tearDown(self):
        self.temporary.cleanup()

    def merge(self, builtins, community=(), archives=()):
        plan = bc.Plan()
        bc.merge_entries(plan, self.mods.root / 'merged', self.game, builtins, list(community), list(archives),
                         self.logged.append)
        return plan

    def test_operations_on_one_entry_become_one(self):
        ours = self.mods.op('uc', self.stock, menu(points=('1', '2', '3')))
        theirs = self.mods.op('other', self.stock, menu(label='Hola'))
        plan = self.merge({bc.UC: [ours]}, [('other', *theirs)])
        self.assertEqual(plan.frozen, {})
        self.assertEqual(plan.drop, {id(ours[0]), id(theirs[0])})
        (op, folder), = plan.add
        self.assertEqual(read((folder / op['payload']).read_bytes()), {'Points': ['1', '2', '3'], 'Label': 'Hola'})
        self.assertEqual(plan.merged, [ASSET + ': ' + bc.UC + ' + other'])

    def test_whole_archive_written_again(self):
        ours = self.mods.op('uc', self.stock, menu(points=('1', '2', '3')))
        other = self.mods.archive('update3.pak', {ASSET: menu(label='Hola'), 'other.igz': b'kept'}, where='racer')
        plan = self.merge({bc.UC: [ours]}, archives=[('racer', 'archives/update3.pak', other)])
        self.assertEqual(plan.frozen, {})
        self.assertNotIn(id(ours[0]), plan.drop)  # the built-in's own entry stays changed too
        files = self.mods.root / 'files'
        bc.write_archives(plan, files, {'archives/update3.pak': other})
        written = read_archive(files / 'archives/update3.pak')
        self.assertEqual(read(written[ASSET]), {'Points': ['1', '2', '3'], 'Label': 'Hola'})
        self.assertEqual(written['other.igz'], b'kept')

    def test_unchanged_copy_is_not_a_change(self):
        ours = self.mods.op('uc', self.stock, menu(points=('1', '2', '3')))
        other = self.mods.archive('update3.pak', {ASSET: self.stock}, where='classic')
        plan = self.merge({bc.UC: [ours]}, archives=[('classic', 'archives/update3.pak', other)])
        self.assertEqual((plan.frozen, plan.archives, plan.merged), ({}, {}, []))

    def test_clash_freezes_the_builtin_and_its_pictures(self):
        ours = self.mods.op('uc', self.stock, menu(points=('1', '2', '3')))
        theirs = self.mods.op('other', self.stock, menu(points=('9',)))
        plan = self.merge({bc.UC: [ours]}, [('other', *theirs)])
        self.assertEqual(set(plan.frozen), {bc.UC, bc.ART})
        self.assertIn('other changes ' + ASSET, plan.frozen[bc.UC])
        self.assertEqual((plan.drop, plan.add), (set(), []))

    def test_lobby_clash_leaves_only_the_lobby(self):
        lobby = 'ui/Octane_Lobby_PrivateGameSettings.igz'
        self.mods.archive('update2.pak', {lobby: self.stock, ASSET: self.stock})
        index = {p: i for i, p in enumerate(sorted([lobby, ASSET], key=lambda p: __import__('graphics_tab').fnv1a(p)))}
        mine = self.mods.op('uc', self.stock, menu(points=('1', '2', '3')), asset=lobby, index=index[lobby])
        menu_op = self.mods.op('uc', self.stock, menu(label='Mine'), asset=ASSET, index=index[ASSET])
        theirs = self.mods.op('other', self.stock, menu(points=('9',)), asset=lobby, index=index[lobby])
        plan = self.merge({bc.UC: [mine, menu_op]}, [('other', *theirs)])
        self.assertEqual(plan.frozen, {})
        self.assertEqual(plan.yielded, {bc.UC: [lobby]})
        self.assertEqual(plan.drop, {id(mine[0])})

    def test_declared_incompatibility_freezes(self):
        plan = bc.Plan()
        bc.declared(plan, [bc.UC], {bc.UC: {'conflicts': ['laps']}, 'laps': {}})
        self.assertEqual(plan.frozen[bc.UC], 'laps is a mod of the same kind')
        plan = bc.Plan()
        bc.declared(plan, ['custom-cup'], {'custom-cup': {}, 'cups': {'conflicts': ['custom-cup']}})
        self.assertEqual(list(plan.frozen), ['custom-cup'])


class MergeBuiltinsTests(unittest.TestCase):
    """Two built-ins changing one entry (Custom Cup's race menu thumbnails and Ultimate
    Customizer's lap list): one merged operation of the first."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.mods = Mods(self.temporary.name)
        self.stock = menu()
        self.mods.archive('update2.pak', {ASSET: self.stock})
        self.game = self.mods.root / 'game'
        self.logged = []

    def tearDown(self):
        self.temporary.cleanup()

    def test_both_changes_in_one_operation(self):
        uc = self.mods.op('uc', self.stock, menu(points=('1', '2', '3')))
        cc = self.mods.op('cc', self.stock, menu(extra='thumbnails'))
        swap, gone = bc.merge_builtins(self.mods.root / 'merged', self.game, [(bc.UC, [uc]), ('custom-cup', [cc])],
                                       self.logged.append)
        self.assertEqual(gone, {id(cc[0])})
        op, folder = swap[id(uc[0])]
        self.assertEqual(read((folder / op['payload']).read_bytes()),
                         {'Points': ['1', '2', '3'], 'Label': 'Hello', 'Extra': 'thumbnails'})
        self.assertEqual(self.logged, ['OCTR mods: merged ' + ASSET + ': ' + bc.UC + ' + custom-cup'])

    def test_a_change_that_cannot_be_merged_is_left_out(self):
        uc = self.mods.op('uc', self.stock, menu(points=('1', '2', '3')))
        cc = self.mods.op('cc', self.stock, menu(points=('9',)))
        swap, gone = bc.merge_builtins(self.mods.root / 'merged', self.game, [(bc.UC, [uc]), ('custom-cup', [cc])],
                                       self.logged.append)
        self.assertEqual((swap, gone), ({}, {id(cc[0])}))
        self.assertIn('custom-cup leaves out its change of ' + ASSET, self.logged[0])

    def test_different_entries_are_left_alone(self):
        uc = self.mods.op('uc', self.stock, menu(points=('1', '2', '3')))
        self.assertEqual(bc.merge_builtins(self.mods.root / 'merged', self.game, [(bc.UC, [uc]), ('custom-cup', [])]),
                         ({}, set()))


if __name__ == '__main__':
    unittest.main()
