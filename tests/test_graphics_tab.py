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
from unittest import mock

import graphics_tab
import quit_confirmation
from igz_file import Igz
from prepare import parse_self


class GraphicsCacheTests(unittest.TestCase):
    def test_cache_tracks_helpers_and_archive_changes_without_manual_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            game, out = root / 'game', root / 'out'
            archive = game / 'archives/update.pak'
            archive.parent.mkdir(parents=True)
            archive.write_bytes(b'original')
            made = []

            def build(game, dest, dump):
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(b'generated')
                made.append(dest)

            def invoke():
                with mock.patch.object(sys, 'argv', ['graphics_tab', str(game), '--out', str(out)]), \
                     contextlib.redirect_stdout(io.StringIO()):
                    graphics_tab.main()

            with mock.patch.object(graphics_tab, 'build', side_effect=build):
                invoke()
                first = made[-1]
                invoke()
                self.assertEqual(len(made), 1)
                read = Path.read_bytes
                def changed(path):
                    data = read(path)
                    return data + b'\n# changed helper' if path.name == 'quit_confirmation.py' else data
                with mock.patch.object(Path, 'read_bytes', changed):
                    invoke()
                self.assertEqual(len(made), 2)
                self.assertNotEqual(first, made[-1])
                self.assertFalse(first.exists())
                # Same-size replacements must invalidate the old archive fingerprint.
                archive.write_bytes(b'replaced')
                invoke()
                self.assertEqual(len(made), 3)
                current = made[-1]
            archive.write_bytes(b'another!')
            with mock.patch.object(graphics_tab, 'build', side_effect=ValueError('failed build')):
                invoke()
            self.assertTrue(current.is_file())


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
        wrapper = script[script.index('startup_python() {'):].split('\n}\n', 1)[0] + '\n}\n'
        setup = '''set -eu
title_id=$1; app_version=$2; game=game; out=out; BB_CONFIG=config
prepare_test() { echo 'archives/update3.pak=graphics.pak'; }
PYTHON=prepare_test
'''
        for title, version, expected in (('CUSA13795', '01.21', 'graphics.pak'),
                                          ('CUSA14876', '01.21', 'graphics.pak'),
                                          ('CUSA14876', '01.00', '')):
            result = subprocess.run(['bash', '-c', setup + wrapper + block +
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
            dialog_name = f'vsc/common/ui/octane_buttons/{quit_confirmation.NAME.lower()}.igz'
            self.assertIn(dialog_name, files)
            self.assertNotIn(quit_confirmation.SOURCE, files)
            dialog = Igz(files[dialog_name])
            for table, kind in dialog.objects():
                if kind == 'igNameList':
                    start = dialog.u64(table + 32)
                    for i in range(dialog.u32(table + 12)):
                        entry = start + i * 16
                        self.assertEqual(dialog.u32(entry + 8), graphics_tab.fnv1a(dialog.string_at(entry)))
            self.assertEqual(dialog.u64(0x1d78 + 0x18), 0x1028)
            self.assertEqual(dialog.u64(0x1d78 + 0x20), 0x1028)
            self.assertEqual(dialog.u64(0x1b28 + 0x30), 0x17f8)
            self.assertEqual(dialog.u64(0x840), 0x5e0)
            self.assertEqual(dialog.u64(0x848), 0x610)
            self.assertEqual(dialog.u64(0x1bc0 + 0x30), 0x1bf8)  # Native control restoration.
            for at, text in ((0x960, 'Quit game'), (0xa38, 'Are you sure you want to quit the game?'),
                             (0xb10, 'Confirm'), (0xbe8, 'Back')):
                self.assertEqual(dialog.types[dialog.u64(at)], 'igVscConstStringAccessor')
                self.assertEqual(dialog.string_at(at + 0x10), text)
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
