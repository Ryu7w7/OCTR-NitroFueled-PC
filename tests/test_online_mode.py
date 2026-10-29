"""Native menu adapter checks; BB_TEST_GAME enables tests using a local game dump."""
from paths import ROOT
import os
import struct
import subprocess
from pathlib import Path
import unittest
from unittest.mock import patch

import gcn_shaders as iga
from igz_file import Igz
import online_mode
from prepare import parse_self


class OnlineModeTests(unittest.TestCase):
    def test_linux_launch_prepares_both_regions(self):
        script = (ROOT / 'run.sh').read_text()
        block = script[script.index('unset BB_ONLINE_MODE_HOOK BB_SPEED_MODE'):
                       script.index('# The game runs at its own 30 FPS')]
        setup = '''
set -eu
prepare_test() {
    case "$1" in
        scripts/online_mode.py)
            echo 'archive_patches=menu.bbpatch'
            echo 'online_mode_hook=test-hook' ;;
        scripts/speed_mode.py)
            echo 'archive_patches=normal.bbpatch'
            echo 'private_archive_patches=speed.bbpatch' ;;
        *) return 1 ;;
    esac
}
PYTHON=prepare_test
game=game
out=out
title_id=$1
app_version=$2
BB_ONLINE=$3
BB_SPEED_SOLO_TEST=0
unset BB_ARCHIVE_PATCHES BB_PRIVATE_ARCHIVE_PATCHES
'''
        for title, version, online, enabled in (
                ('CUSA13795', '01.21', '1', True), ('CUSA14876', '01.21', '1', True),
                ('CUSA14876', '01.00', '1', False), ('CUSA14876', '01.21', '0', False)):
            with self.subTest(title=title, version=version, online=online):
                result = subprocess.run(
                    ['bash', '-c', setup + block +
                     '\nprintf "%s|%s|%s" "${BB_ONLINE_MODE_HOOK:-}" '
                     '"${BB_ARCHIVE_PATCHES:-}" "${BB_PRIVATE_ARCHIVE_PATCHES:-}"',
                     'test', title, version, online], capture_output=True, text=True, check=True)
                self.assertEqual(result.stdout,
                                 'test-hook|normal.bbpatch|speed.bbpatch' if enabled else '||')

    def test_unknown_graph_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'supported USA'):
            online_mode.rebuild(b'not a supported menu')

    def test_hook_requires_supported_adapter(self):
        with self.assertRaisesRegex(ValueError, 'not uniquely recognized'):
            online_mode.hook_site(bytes(512))

    def test_hook_regions_and_invalid_displacement(self):
        for address, displacement in ((0x1277e00, 0x187a197), (0x1277df0, 0x187a1a7)):
            code = bytes.fromhex('55 48 89 e5 41 57 41 56 53 50 48 8d 05')
            code += struct.pack('<i', displacement)
            with patch.object(online_mode, 'load_segments', return_value=[(address, 0, len(code))]):
                self.assertEqual(online_mode.hook_site(code), address)
                wrong = code[:13] + struct.pack('<i', displacement ^ 0x30)
                with self.assertRaises(ValueError):
                    online_mode.hook_site(wrong)
            with patch.object(online_mode, 'load_segments', return_value=[(address, 0, len(code))] * 2):
                with self.assertRaises(ValueError):
                    online_mode.hook_site(code)

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires a local USA/EU 1.21 game dump')
    def test_real_selector_options_and_relocations(self):
        game = Path(os.environ['BB_TEST_GAME'])
        elf, _, headers, *_ = parse_self((game / 'eboot.bin').read_bytes())
        hook = online_mode.hook_site(elf)
        shift = 0x1277e00 - hook

        def at(address, size):
            segment = next(p for p in headers if p['type'] in (1, 0x61000010) and
                           p['vaddr'] <= address and address + size <= p['vaddr'] + p['filesz'])
            offset = segment['offset'] + address - segment['vaddr']
            return elf[offset:offset + size]

        self.assertEqual(struct.unpack('<Q', at(0x2773b40, 8))[0], 0x1277c70 - shift)
        self.assertEqual(at(0xfb7710 - shift, 15),
                         bytes.fromhex('55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 20'))
        for address in (0x125d570 - shift, 0xf50350 - shift):
            self.assertEqual(at(address, 8), bytes.fromhex('55 48 89 e5 41 57 41 56'))
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
