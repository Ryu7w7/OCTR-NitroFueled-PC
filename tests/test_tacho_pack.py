"""Pack Tacho built into the port (scripts/tacho_pack.py, scripts/tacho_pack/patches/)."""
import hashlib
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import patches  # noqa: E402
import tacho_pack  # noqa: E402


class TachoPackTests(unittest.TestCase):
    def test_every_game_file_is_present_and_intact(self):
        m = tacho_pack.manifest()
        # no main-menu logo (the game's own menu); 13 for the drift bar's atlas and boost bar (27/11)
        self.assertEqual(len(m['operations']), 21)
        self.assertEqual(len(m['menu_operations']), 1)  # the Options > Game page, always applied
        for op in m['operations'] + m['menu_operations']:
            data = (tacho_pack.FOLDER / op['payload']).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), op['patched_sha256'], op['payload'])

    def test_executable_patches_are_always_applied_and_not_listed(self):
        self.assertEqual(sorted(tacho_pack.manifest()['patches']), sorted(patches.TACHO_PACK_FILES))
        # Not in patches/, which the launchers list with the mods.
        listed = patches.external_patches(ROOT / 'patches')
        self.assertFalse([key for key, _, _ in listed if patches.tacho_pack_patch(key)])
        pack = patches.tacho_pack_patches({})
        self.assertEqual(len(pack), 6)
        # patches.json cannot turn them off, and a copy elsewhere (the old mod) is left out.
        with tempfile.TemporaryDirectory() as folder:
            config = Path(folder) / 'patches.json'
            config.write_text('{"disabled": [%s]}' % ','.join('"%s"' % k for k, _, _ in pack), encoding='utf-8')
            self.assertEqual(len(patches.tacho_pack_patches({})), 6)
            self.assertFalse(patches.external_selection(pack, config, {}))
        for name in patches.TACHO_PACK_FILES:
            for line in ET.parse(patches.TACHO_PACK_DIR / name).getroot().iter('Line'):
                self.assertTrue(line.get('Signature') or line.get('ExpectedSHA256'), name)

    def test_the_launch_applies_its_files(self):
        # run.py must prepare the pack's game files (HUD, leaders list, Options > Game page) and hand
        # N.Sane's view to the game: a merge once dropped both (no options, a crash at race start).
        source = (ROOT / 'run.py').read_text(encoding='utf-8')
        self.assertIn("startup_call('tacho_pack.prepare', tacho_pack.prepare", source)
        self.assertIn("env['BB_NSANE_ARCHIVE_PATCHES'] = ", source)

    def test_the_european_game_has_them_too(self):
        # tools/port_patch_eu.py's copies for CUSA14876 1.21: the same six, each line verified
        pack = patches.tacho_pack_patches({}, '01.21', 'CUSA14876')
        self.assertEqual(sorted(key.partition('/')[0] for key, _, _ in pack), sorted(patches.TACHO_PACK_FILES))
        for name in patches.TACHO_PACK_FILES:
            root = ET.parse(patches.TACHO_PACK_EU_DIR / name).getroot()
            self.assertEqual(root.find('TitleID/ID').text, 'CUSA14876')
            for line in root.iter('Line'):
                self.assertTrue(line.get('Signature') or line.get('ExpectedSHA256'), name)
        self.assertEqual(patches.tacho_pack_patches({}, '01.00', 'CUSA14876'), [])

    def test_no_setting_turns_it_off(self):
        with mock.patch.dict(os.environ, {'BB_TACHO_PACK': '0', 'BB_TACHO_PACK_MENU': '1'}):
            self.assertEqual(len(patches.tacho_pack_patches({'tacho_pack': '0'})), 6)
            self.assertEqual(len(tacho_pack.operations()), 22)  # the Options > Game page always in

    def test_a_changed_file_stops_the_launch(self):
        with tempfile.TemporaryDirectory() as folder:
            copy = Path(folder) / 'tacho_pack'
            shutil.copytree(tacho_pack.FOLDER, copy)
            with mock.patch.object(tacho_pack, 'FOLDER', copy), \
                    mock.patch.object(patches, 'TACHO_PACK_DIR', copy / 'patches'):
                tacho_pack.verify()
                op = tacho_pack.operations()[0]
                original = (copy / op['payload']).read_bytes()
                (copy / op['payload']).write_bytes(b'not the pack')
                with self.assertRaises(tacho_pack.Modified):
                    tacho_pack.payload(op)
                (copy / op['payload']).write_bytes(original)
                xml = copy / 'patches' / 'ItemsMejorados.xml'
                xml.write_bytes(xml.read_bytes().replace(b'PatchVer="1.5.1"', b'PatchVer="9.9.9"'))
                with self.assertRaises(tacho_pack.Modified):
                    patches.tacho_pack_patches({})
                xml.unlink()
                with self.assertRaises(tacho_pack.Modified):
                    tacho_pack.verify()

    def test_its_patches_never_block_another_executable(self):
        for name in patches.TACHO_PACK_FILES:
            self.assertTrue(patches.optional_game_patch(name + '/anything'))
        self.assertFalse(patches.optional_game_patch('SomeMod.xml/anything'))

    def test_a_game_without_the_archives_keeps_its_files(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / 'archives').mkdir()
            made, env = tacho_pack.prepare(folder, Path(folder) / 'out', None, {}, {})
            self.assertEqual((made, env), ({}, {}))
            self.assertEqual(tacho_pack.stream_replacements(), {})


if __name__ == '__main__':
    unittest.main()
