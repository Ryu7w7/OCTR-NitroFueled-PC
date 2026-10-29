"""Native menu adapter checks; BB_TEST_GAME enables tests using a local game dump."""
from paths import ROOT
import os
from pathlib import Path
import unittest

import gcn_shaders as iga
from igz_file import Igz
import online_mode
from prepare import parse_self


class OnlineModeTests(unittest.TestCase):
    def test_unknown_graph_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'supported USA'):
            online_mode.rebuild(b'not a supported menu')

    def test_hook_requires_supported_adapter(self):
        with self.assertRaisesRegex(ValueError, 'not uniquely recognized'):
            online_mode.hook_site(bytes(512))

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires a local USA 1.21 game dump')
    def test_real_selector_options_and_relocations(self):
        game = Path(os.environ['BB_TEST_GAME'])
        elf, *_ = parse_self((game / 'eboot.bin').read_bytes())
        self.assertGreater(online_mode.hook_site(elf), 0)
        pak = game / 'archives/update2.pak'
        entry = next(e for e in iga.pak_entries(pak) if e[0].lower() == online_mode.ENTRY)
        with pak.open('rb') as f:
            original = iga.read_entry(f, *entry[1:])
        old, new = Igz(original), Igz(online_mode.rebuild(original))
        self.assertEqual(len(old.objects()), len(new.objects()))
        self.assertEqual(new.u32(0x188c), 3)
        self.assertEqual(new.u32(0x18b4), 3)
        options = new.u64(0x18a0)
        self.assertEqual([new.string_at(options + i * 8) for i in range(3)],
                         ['NORMAL MODE', 'SPEED MODE', 'BATTLE MODE'])
        available = new.u64(0x18c8)
        self.assertEqual(new.data[available:available + 3], bytes((1, 1, 1)))
        for lst in (0x1880, 0x18a8):
            self.assertTrue(new.u64(lst + 0x18) & (1 << 56), 'archive data must not be heap-freed')
        for i in range(3):
            self.assertIn(options + i * 8, new.sites['RSTT'])
        for site in new.sites['ROFS']:
            self.assertLess(new.u64(site), len(new.data))
        pak = game / 'archives/update.pak'
        entry = next(e for e in iga.pak_entries(pak) if e[0].lower() == online_mode.LABEL_ENTRY)
        with pak.open('rb') as f:
            label = Igz(iga.read_entry(f, *entry[1:]))
        label.strings[label.strings.index('Racing Type')] = 'GAME MODE'
        self.assertIn('GAME MODE', Igz(label.build()).strings)



if __name__ == '__main__':
    unittest.main()
