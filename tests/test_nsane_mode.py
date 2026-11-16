"""N.Sane Mode HUD slot (scripts/nsane_mode.py); BB_TEST_GAME enables the checks on a local game dump."""
import os
import struct
import unittest
from pathlib import Path

from paths import ROOT  # noqa: F401  (adds scripts/ to the import path)
import nsane_mode
from archive_reader import Archive
from igz_file import Igz, unpack_offsets
from speed_mode import ITEM_HUD


def item_hud(pak):
    archive = Archive(pak)
    try:
        return archive.extract(next(item for item in archive.files if item['path'] == ITEM_HUD))
    finally:
        archive.data.close()
        archive._file.close()


@unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires a local USA/EU 1.21 game dump')
class NsaneHudTests(unittest.TestCase):
    def test_second_slot_hidden_and_marked(self):
        game = Path(os.environ['BB_TEST_GAME'])
        for name in ('update2.pak', 'juicedomain_race.pak'):
            with self.subTest(pak=name):
                original = item_hud(game / 'archives' / name)
                new = Igz(nsane_mode.add_second_slot(original))
                handles = new.read_list('RHND', unpack_offsets)
                exnm = new.fixup('EXNM')
                spaces = [struct.unpack_from('<2I', exnm[3], 8 * i) for i in range(exnm[1])]
                names, k = {}, 0
                for asset, space in spaces:
                    if space & 0x80000000:
                        names[0x80000000 | k] = new.strings[asset]
                        k += 1
                marks = {}
                for off, kind in new.objects():
                    if kind != 'igGuiPlaceable':
                        continue
                    z = struct.unpack_from('<f', new.data, off + 0x74)[0]
                    if 1000 <= z <= 1013:
                        marks[int(z) - 1000] = off
                        self.assertTrue(new.u32(off + 0x0c) & 0x400, 'the slot starts hidden')
                        self.assertEqual(struct.unpack_from('<f', new.data, off + 0xe0)[0], 0.0)
                self.assertEqual(sorted(marks), list(range(14)))
                for kind, icon in enumerate(nsane_mode.ICONS):
                    self.assertIn(marks[kind] + 0x38, handles)
                    self.assertEqual(names[new.u32(marks[kind] + 0x38)], f'ico_powerups_{icon}_tga')
                self.assertEqual(max(h & 0x7fffffff for h in (new.u32(s) for s in handles)
                                     if h & 0x80000000) + 1, k, 'every handle names an EXNM entry')
                with self.assertRaises(ValueError):
                    nsane_mode.add_second_slot(new.build())


if __name__ == '__main__':
    unittest.main()
