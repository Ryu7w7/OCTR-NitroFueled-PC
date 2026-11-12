"""Structural checks against the user's native leaderboard assets."""
import os
import struct
import unittest
from pathlib import Path
import gcn_shaders as iga
from igz_file import Igz
from online_mode import ranked_main_button
import hall_of_fame as hof


class DependencyTests(unittest.TestCase):
    def test_native_dependency_walk_stops_before_next_fixup(self):
        g = Igz.__new__(Igz)
        g.fixups = [['TDEP', 0, 16, b'', 16]]
        hof.dependencies(g, {'a': 'b'})
        _, count, start, raw, length = g.fixup('TDEP')
        data = raw + bytes(length - start - len(raw)) + b'TSTR' + bytes(12)
        pairs, position = [], 0
        while data[position]:
            end = data.index(0, position)
            namespace = data[position:end]
            position = end + 1
            end = data.index(0, position)
            pairs.append((namespace, data[position:end]))
            position = end + 1
        self.assertEqual(count, 1)
        self.assertEqual(pairs, [(b'a', b'b')])


@unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'BB_TEST_GAME needs a native game dump')
class HallOfFameTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update2.pak'
        entries = iga.pak_entries(pak)
        cls.blobs = {}
        with pak.open('rb') as f:
            for name in ('octane_leaderboards', 'octane_highscore_select', 'octane_lobby_main'):
                entry = next(e for e in entries if e[0].lower().endswith('/ui/' + name + '.igz'))
                cls.blobs[name] = iga.read_entry(f, *entry[1:])

    def test_separate_native_project_and_independent_rows(self):
        g = Igz(hof.leaderboard(self.blobs['octane_leaderboards']))
        self.assertIn(hof.PROJECT, g.strings)
        from graphics_tab import fnv1a
        for table, kind in g.objects():
            if kind != 'igNameList': continue
            for i in range(g.u32(table + 12)):
                entry = g.u64(table + 32) + i * 16
                self.assertEqual(g.u32(entry + 8), fnv1a(g.strings[g.u64(entry)]))

        self.assertNotIn('Octane_Leaderboards', g.strings)
        back = g.u64(0x4a38)
        self.assertEqual(struct.unpack_from('<II', g.fixup('EXID')[3], back * 8),
                         (fnv1a('Octane_Lobby_Main'),) * 2)
        rows = [o for o, t in g.objects() if t == 'igGuiPlaceable' and
                o + 0x28 in g.sites['RSTT'] and g.string_at(o + 0x28).startswith('OCTR_HOF_ROW_')]
        self.assertEqual(len(rows), 8)
        self.assertEqual(len({g.u64(o + 0x38) for o in rows}), 8)
        exnm = g.fixup('EXNM')
        handles = [(g.strings[a], g.strings[b & 0x7fffffff])
                   for a, b in struct.iter_unpack('<II', exnm[3][:exnm[1] * 8])
                   if b & 0x80000000]
        names = next(o for o, t in g.objects() if t == 'igNameList')
        exports = {g.strings[g.u64(g.u64(names + 32) + i * 16)]:
                   g.u64(g.u64(32) + i * 8) for i in range(g.u32(12))}
        from igz_file import unpack_offsets
        for row in rows:
            self.assertNotEqual(g.u64(row + 0xe8), 0)
            self.assertTrue(g.u32(row + 12) & 1)
            self.assertIn(row + 0x38, g.read_list('RHND', unpack_offsets))
            self.assertNotIn(row + 0x38, g.sites['ROFS'])
            name, namespace = handles[g.u64(row + 0x38) & 0x7fffffff]
            self.assertEqual(namespace, hof.PROJECT)
            asset = exports[name]
            self.assertEqual(dict(g.objects())[asset], 'igGuiComposition')
            self.assertIn(asset + 0x10, g.sites['RSTT'])
            self.assertLess(g.u64(asset + 0x10), len(g.strings))
            root = g.u64(asset + 0x20)
            self.assertNotEqual(root, 0x6a0)
            self.assertEqual(g.u64(root + 0xe8), 0)
        icons = [o for o, t in g.objects() if t == 'igGuiPlaceable' and
                 o + 0x28 in g.sites['RSTT'] and g.string_at(o + 0x28).startswith('OCTR_HOF_ICON_')]
        self.assertEqual(len(icons), 9 * 14)
        for icon in icons:
            self.assertFalse(g.u32(icon + 12) & 0x400)
            name, namespace = handles[g.u64(icon + 0x38) & 0x7fffffff]
            self.assertEqual(namespace, 'Octane_LobbyMembers')
            self.assertTrue(name.startswith('OCTR_RANK_'))
        def chain(root):
            result = []
            while root:
                self.assertNotIn(root, result)
                result.append(root)
                successor = g.u64(root + 0x50)
                if successor: self.assertEqual(g.u64(successor + 0x58), root)
                root = successor
            return result
        self.assertTrue(set(rows).issubset(chain(0x12f0)))
        for composition in [0x670] + [exports[handles[g.u64(row + 0x38) & 0x7fffffff][0]] for row in rows]:
            root = g.u64(composition + 0x20)
            label = 0x970 if composition == 0x670 else root + 0x760
            parent = g.u64(label + 0x40)
            nickname = 0xa90 if composition == 0x670 else root + 0x640
            nickname_x = struct.unpack_from('<f', g.data, nickname + 0x60)[0]
            nickname_width = struct.unpack_from('<f', g.data, nickname + 0x90)[0]
            self.assertEqual((nickname_x, nickname_width), (-270.0, 330.0))
            rank = 0xcd0 if composition == 0x670 else root + 0x400
            rank_x = struct.unpack_from('<f', g.data, rank + 0x60)[0]
            rank_width = struct.unpack_from('<f', g.data, rank + 0x90)[0]
            self.assertEqual((rank_x, rank_width), (-460.0, 150.0))
            self.assertGreaterEqual(nickname_x - (rank_x + rank_width), 40.0)
            self.assertEqual(g.u32(rank + 0xa4), g.u32(nickname + 0xa4))
            self.assertEqual(g.u32(0x7e90 + 0x68), 1)  # Fit multi-digit ranks inside the rank column.
            self.assertEqual(g.u32(0x7f40 + 0x68), 1)  # Native forceFitInBox.
            row_icons = [icon for icon in icons if g.u64(icon + 0x40) == parent]
            self.assertEqual(len(row_icons), 14)
            self.assertTrue(set(row_icons).issubset(chain(root)))
            label_x, label_y = struct.unpack_from('<2f', g.data, label + 0x60)
            self.assertEqual(struct.unpack_from('<3f', g.data, label + 0x6c), (0.0, 0.5, 0.5))
            label_width = struct.unpack_from('<f', g.data, label + 0x90)[0]
            label_scale = struct.unpack_from('<f', g.data, label + 0x84)[0]
            self.assertEqual(label_scale, 1.0)
            self.assertLessEqual(label_x + label_width * label_scale, 500.0)
            for icon in row_icons:
                icon_x, icon_y = struct.unpack_from('<2f', g.data, icon + 0x60)
                width, height = struct.unpack_from('<2f', g.data, icon + 0x90)
                self.assertEqual((width, height), (40.0, 40.0))
                scale_x, scale_y = struct.unpack_from('<2f', g.data, icon + 0x84)
                self.assertAlmostEqual(100.0 * scale_x, width, places=5)
                self.assertAlmostEqual(100.0 * scale_y, height, places=5)
                self.assertEqual(icon_x + width * 0.5 + 32.0, label_x)
                self.assertLessEqual(nickname_x + nickname_width + 8.0, icon_x - width * 0.5)
                self.assertEqual(icon_y, label_y)
                self.assertEqual(struct.unpack_from('<3f', g.data, icon + 0x6c), (0.0, 0.5, 0.5))
                self.assertEqual(g.u32(icon + 0xa0), g.u32(label + 0xa0))
                self.assertEqual(g.u32(icon + 0xa4), g.u32(label + 0xa4))
        for key in (0x8ea8, 0x8ed0, 0x9180, 0x91a8, 0x91d0):
            self.assertEqual(struct.unpack_from('<3f', g.data, key + 0x18), (140.0, 0.0, 0.0))
        for key in (0xbe50, 0xbe78, 0xbf60, 0xbf88):
            scale = struct.unpack_from('<3f', g.data, key + 0x18)
            self.assertAlmostEqual(scale[0], 0.9)
            self.assertAlmostEqual(scale[1], 0.9)
        for key, kind in g.objects():
            if 0xa2b0 <= key < 0xb6c0 and kind == 'igGuiVec3LinearKeyframe':
                scale = struct.unpack_from('<3f', g.data, key + 0x18)
                self.assertAlmostEqual(scale[0], 0.8)
                self.assertAlmostEqual(scale[1], 0.8)
        for key in (0x9c08, 0x9c30, 0x9df0, 0x9e18, 0x9e40):
            self.assertEqual(struct.unpack_from('<3f', g.data, key + 0x18), (-460.0, 0.0, 0.0))
        self.assertEqual(g.u64(0x7ff0 + 0x20), g.u64(0x7f40 + 0x20))
        self.assertEqual(g.u32(0x7ff0 + 0x5c), 0)
        self.assertEqual(g.u32(0x7ff0 + 0x60), 1)
        self.assertEqual(g.u32(0x7ff0 + 0x68), 1)
        self.assertFalse(g.u32(0x2010 + 12) & 0x400)
        self.assertTrue(g.u32(0x3df0 + 12) & 0x400)
        navigation = g.u64(rows[0] + 0xe8)
        self.assertEqual(g.u32(navigation + 12), 1)
        behavior = g.u64(g.u64(navigation + 0x20))
        self.assertEqual(dict(g.objects())[behavior], 'CGuiBehaviorOctaneNavigationItem')
        self.assertEqual(g.u64(behavior + 0xb0), 0x560)
        self.assertEqual(g.u64(behavior + 0xb8), 0x5d8)
        self.assertIn(behavior + 0xb0, g.sites['ROFS'])
        self.assertIn(behavior + 0xb8, g.sites['ROFS'])
        self.assertEqual(g.u64(behavior + 0xc8), 0x560)
        self.assertEqual(g.u64(behavior + 0xd0), 0x5d8)
        behavior = 0x2ec8
        external_names = [(g.strings[a], g.strings[b])
                          for a, b in struct.iter_unpack('<II', exnm[3][:exnm[1] * 8])
                          if not b & 0x80000000]
        for at, animation in ((0xb0, 'AnimationTag_Focus'), (0xb8, 'AnimationTag_Defocus')):
            self.assertIn(behavior + at, g.read_list('RNEX', unpack_offsets))
            self.assertEqual(external_names[g.u64(behavior + at)], (animation, 'SettingSelector'))
        for selector in (0x2af0, 0x2750, 0x23b0):
            self.assertEqual(g.u32(g.u64(selector + 0xe8) + 12), 1)
        for site in g.sites['ROFS']:
            self.assertLess(g.u64(site), len(g.data))

    def test_native_buttons_bind_project_targets(self):
        from graphics_tab import fnv1a
        g = Igz(ranked_main_button(self.blobs['octane_lobby_main']))
        native = Igz(self.blobs['octane_lobby_main'])
        f = g.fixup('EXID')
        ids = list(struct.iter_unpack('<II', f[3][:f[1] * 8]))
        f = g.fixup('EXNM')
        handles = [(g.strings[a], g.strings[b & 0x7fffffff])
                   for a, b in struct.iter_unpack('<II', f[3][:f[1] * 8]) if b & 0x80000000]
        buttons = {g.string_at(o + 0x28): o for o, t in g.objects()
                   if t == 'igGuiPlaceable' and o + 0x28 in g.sites['RSTT']}
        for tag, target in (('OCTR_RANKED_BUTTON', 'Octane_Lobby_ModeSelect'),
                            ('OCTR_HALL_OF_FAME_BUTTON', 'Octane_Lobby_ModeSelect')):
            li = g.u64(buttons[tag] + 0xe8)
            behavior = g.u64(g.u64(li + 32) + 8)
            self.assertEqual(g.u64(behavior + 0x20), native.u64(0x23b8 + 0x20))
            handle = g.u64(behavior + 0x38)
            if handle & 0x80000000:
                self.assertEqual(handles[handle & 0x7fffffff], (target, target))
            else:
                self.assertEqual(ids[handle], (fnv1a(target), fnv1a(target)))
        hof_ui = Igz(hof.leaderboard(self.blobs['octane_leaderboards']))
        names = next(o for o, t in hof_ui.objects() if t == 'igNameList')
        exports = {hof_ui.strings[hof_ui.u64(hof_ui.u64(names + 32) + i * 16)]:
                   hof_ui.u64(hof_ui.u64(32) + i * 8) for i in range(hof_ui.u32(12))}
        self.assertEqual(dict(hof_ui.objects())[exports[hof.PROJECT]], 'igGuiProject')
        self.assertNotIn(b'ui:/octane_halloffame.igz', g.fixup('TDEP')[3])


if __name__ == '__main__':
    unittest.main()
