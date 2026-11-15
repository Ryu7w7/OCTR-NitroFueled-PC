"""Pack Tacho built into the port (scripts/tacho_pack.py, patches/)."""
import hashlib
import os
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import patches  # noqa: E402
import tacho_pack  # noqa: E402


class TachoPackTests(unittest.TestCase):
    def test_every_game_file_is_present_and_intact(self):
        m = tacho_pack.manifest()
        self.assertEqual(len(m['operations']), 22)
        self.assertEqual(len(m['menu_operations']), 1)
        for op in m['operations'] + m['menu_operations']:
            data = (tacho_pack.FOLDER / op['payload']).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), op['patched_sha256'], op['payload'])

    def test_executable_patches_are_bundled_on_by_default(self):
        self.assertEqual(sorted(tacho_pack.manifest()['patches']), sorted(patches.TACHO_PACK_FILES))
        found = patches.external_patches(ROOT / 'patches')
        pack = [key for key, _, _ in found if patches.tacho_pack_patch(key)]
        self.assertEqual(len(pack), 6)
        chosen = {key for key, _, _ in patches.external_selection(found, None, {})}
        self.assertTrue(set(pack) <= chosen)
        for name in patches.TACHO_PACK_FILES:
            for line in ET.parse(ROOT / 'patches' / name).getroot().iter('Line'):
                self.assertTrue(line.get('Signature') or line.get('ExpectedSHA256'), name)

    def test_the_pack_turns_off_as_a_whole(self):
        found = patches.external_patches(ROOT / 'patches')
        chosen = patches.external_selection(found, None, {'tacho_pack': '0'})
        self.assertFalse([key for key, _, _ in chosen if patches.tacho_pack_patch(key)])
        with tempfile.TemporaryDirectory() as folder:
            ini = Path(folder) / 'bbport.ini'
            ini.write_text('tacho_pack=0\n', encoding='utf-8')
            self.assertFalse(tacho_pack.wanted(ini, {}))
            self.assertEqual(tacho_pack.prepare(folder, folder, ini, {}, {}), ({}, {}))
            ini.write_text('tacho_pack_menu=1\n', encoding='utf-8')
            self.assertTrue(tacho_pack.wanted(ini, {}))
            self.assertEqual(len(tacho_pack.operations(ini, {})), 23)
        self.assertEqual(len(tacho_pack.operations(None, {})), 22)

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
