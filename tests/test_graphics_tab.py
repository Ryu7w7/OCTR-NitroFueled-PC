"""Graphics/Quit menu checks; BB_TEST_GAME enables tests against a local 1.21 edition."""
from paths import ROOT
import contextlib
import io
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

import graphics_tab
from igz_file import Igz
from prepare import parse_self


class GraphicsTabTests(unittest.TestCase):
    def test_labels_preserve_string_positions(self):
        for label in graphics_tab.LABELS.values():
            original = b'prefix\0Language selection padding\0\0' + graphics_tab.ANCHOR + b'\0Gameplay\0'
            changed = graphics_tab.relabel(original, label)
            self.assertEqual(len(changed), len(original))
            self.assertEqual(changed.count(b'\0'), original.count(b'\0'))
            self.assertEqual(changed.index(graphics_tab.ANCHOR), original.index(graphics_tab.ANCHOR))
            self.assertTrue(changed.endswith(b'\0Gameplay\0'))

    def test_linux_prepares_graphics_for_both_regions(self):
        script = (ROOT / 'run.sh').read_text()
        start = script.index('unset BB_GRAPHICS_TAB_ARCHIVE')
        end = script.index('\nfi', start) + 3
        block = script[start:end]
        setup = '''set -eu
title_id=$1; app_version=$2; game=game; out=out; BB_CONFIG=config
prepare_test() { echo 'archives/update3.pak=graphics.pak'; }
PYTHON=prepare_test
'''
        for title, version, expected in (('CUSA13795', '01.21', 'graphics.pak'),
                                          ('CUSA14876', '01.21', 'graphics.pak'),
                                          ('CUSA14876', '01.00', '')):
            result = subprocess.run(['bash', '-c', setup + block +
                                     '\nprintf "%s" "${BB_GRAPHICS_TAB_ARCHIVE:-}"',
                                     'test', title, version], text=True, capture_output=True, check=True)
            self.assertEqual(result.stdout, expected)

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires a local USA/EU 1.21 game dump')
    def test_real_graphics_and_quit_archive(self):
        game = Path(os.environ['BB_TEST_GAME'])
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            dest = Path(tmp) / 'update3.pak'
            graphics_tab.build(game, dest, Path(tmp) / 'dump')
            archive = graphics_tab.Archive(dest)
            self.addCleanup(archive.f.close)
            files = {archive.logical(i).lower(): archive.read(i) for i in range(len(archive.names))}
            for name in ('ui/octane_options_menu.igz', 'ui/octane_mainmenu.igz'):
                self.assertIn(name, files)
            options = Igz(files['ui/octane_options_menu.igz'])
            self.assertEqual(struct.unpack_from('<f', options.data, 0x1370 + 0xe0)[0], 1.0)
            main = Igz(files['ui/octane_mainmenu.igz'])
            quit_name = graphics_tab.QUIT_LEVEL[1][:-1].decode()
            online_name = graphics_tab.QUIT_LEVEL[0][:-1].decode()
            self.assertEqual(sum(main.string_at(s) == quit_name for s in main.sites['RSTT']), 1)
            self.assertGreaterEqual(sum(main.string_at(s) == online_name for s in main.sites['RSTT']), 1)
            for language, label in graphics_tab.LABELS.items():
                key = f'ui/octane_options_menu_{language}.lng'
                if key not in files:
                    continue
                text = Igz(files[key])
                i = text.strings.index(graphics_tab.ANCHOR.decode())
                self.assertEqual(text.strings[i - 1].strip(), label)
                quit_text = Igz(files[f'ui/octane_mainmenu_{language}.lng'])
                i = quit_text.strings.index(graphics_tab.QUIT_KEY[:-1].decode())
                self.assertEqual(quit_text.strings[i + 1].strip(), graphics_tab.QUIT_LABELS[language][0])


@unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('c++'), 'requires Linux and a C++ compiler')
class GraphicsHookTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='graphics-hook-test-')
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.exe = Path(cls.tmp.name) / 'graphics-test'
        subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-pthread',
                        '-I' + str(ROOT / 'gpu/shadps4'), str(ROOT / 'tests/test_graphics_tab.cpp'),
                        '-o', str(cls.exe)], check=True, capture_output=True)

    def test_synthetic_regions_and_rejection(self):
        subprocess.run([str(self.exe)], check=True, capture_output=True)

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires a local USA/EU 1.21 game dump')
    def test_real_hook_installation(self):
        game = Path(os.environ['BB_TEST_GAME'])
        elf, _, headers, *_ = parse_self((game / 'eboot.bin').read_bytes())
        loads = [p for p in headers if p['type'] in (1, 0x61000010)]
        image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
        for p in loads:
            image[p['vaddr']:p['vaddr'] + p['filesz']] = elf[p['offset']:p['offset'] + p['filesz']]
        path = Path(self.tmp.name) / 'image.bin'
        path.write_bytes(image)
        shift = '0' if image[0x10de0c9:0x10de0ce] == bytes.fromhex('e8 42 7f f5 ff') else '10'
        subprocess.run([str(self.exe), str(path), shift], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
