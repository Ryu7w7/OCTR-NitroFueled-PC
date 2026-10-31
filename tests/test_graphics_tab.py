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
    def test_arabic_menu_labels_are_localized_and_fit_original_slots(self):
        import unicodedata
        self.assertEqual(unicodedata.normalize('NFKC', graphics_tab.LABELS['ar']), 'الرسوميات')
        label, hint = graphics_tab.QUIT_LABELS['ar']
        self.assertEqual(unicodedata.normalize('NFKC', label), 'خروج')
        self.assertEqual(unicodedata.normalize('NFKC', hint), 'إغلاق اللعبة والعودة إلى سطح المكتب.')
        self.assertLessEqual(len(graphics_tab.LABELS['ar'].encode()), 34)
        self.assertLessEqual(len(label.encode()) + 1, 16)
        self.assertLessEqual(len(hint.encode()), 191)

    def test_labels_preserve_string_positions(self):
        for label in graphics_tab.LABELS.values():
            original = b'prefix\0' + b'Language selection padding'.ljust(34, b' ') + b'\0\0' + graphics_tab.ANCHOR + b'\0Gameplay\0'
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
    def test_gameplay_camera_native_row(self):
        from gameplay_camera import CAMERA_OPTION, gameplay_page
        game = Path(os.environ['BB_TEST_GAME'])
        archive = graphics_tab.Archive(game / 'archives/update2.pak')
        try:
            i = next(i for i in range(len(archive.names))
                     if archive.logical(i).lower() == 'ui/octane_options_menu_page.igz')
            original = archive.read(i)
        finally:
            archive.f.close()
        changed = gameplay_page(original)
        g = Igz(changed)
        from widescreen_startup import IGZ
        for tag in ('ROOT', 'ONAM'):
            self.assertEqual(g.fixup(tag), Igz(original).fixup(tag))
        self.assertEqual([(o['ptr'], o.get('name')) for o in IGZ(changed).objects
                          if o['ptr'] < len(Igz(original).data)],
                         [(o['ptr'], o.get('name')) for o in IGZ(original).objects])
        selectors = [p for p, kind in g.objects()
                     if kind == 'CGuiBehaviorOctaneGlobalOptionSelector'
                     and g.u32(p + 0x1c8) == CAMERA_OPTION]
        self.assertEqual(len(selectors), 1)
        selector = selectors[0]
        self.assertEqual(g.data[selector + 0x1cc], 0)
        self.assertEqual(g.u64(selector + 0x148), 0)
        options = g.u64(selector + 0x1c0)
        self.assertEqual(g.u32(options + 0x10), 2)
        data = g.u64(options + 0x20)
        self.assertEqual([g.string_at(data + 8*i) for i in range(2)], ['Near', 'Far'])
        row = g.u64(0x1b10 + 0x50)
        self.assertEqual(g.u64(row + 0x40), 0x1b10)
        self.assertEqual(g.u64(selector + 0x110), 0x2b50)
        self.assertEqual(g.u64(selector + 0x118), 0x1c90)
        self.assertEqual(g.u64(0x2ca8 + 0x118), row)
        self.assertEqual(g.u64(0x3a48 + 0x110), row)
        for p in (0x2fd0, 0x3398, 0x3cb8):
            self.assertEqual(g.data[p:p+0xe8], Igz(original).data[p:p+0xe8])

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires a local USA/EU 1.21 game dump')
    def test_real_graphics_and_quit_archive(self):
        game = Path(os.environ['BB_TEST_GAME'])
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            dest = Path(tmp) / 'update3.pak'
            graphics_tab.build(game, dest, Path(tmp) / 'dump')
            archive = graphics_tab.Archive(dest)
            # Closed before the folder goes (Windows cannot delete an open file).
            with archive.f:
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

    def test_camera_ini_round_trip(self):
        exe = Path(self.tmp.name) / 'camera-settings-test'
        subprocess.run(['c++', '-std=c++20', '-O2', '-pthread', '-I' + str(ROOT),
                        str(ROOT / 'tests/test_camera_settings.cpp'),
                        str(ROOT / 'gpu/shim/bbport_settings.cpp'),
                        str(ROOT / 'gpu/shim/bbport_menu_es.cpp'), '-o', str(exe)],
                       check=True, capture_output=True)
        subprocess.run([str(exe)], check=True, capture_output=True)

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
