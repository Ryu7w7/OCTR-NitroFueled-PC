from paths import ROOT
import ast
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import patches


class EUUnlockAllTests(unittest.TestCase):
    KEY = 'CrashTeamRacingUnlockAllEU.xml/Unlock Everything'
    CANONICAL = 'CrashTeamRacingUnlockAll.xml/Unlock Everything'

    def found(self, title='CUSA14876', version='01.21'):
        return [entry for entry in patches.external_patches(ROOT / 'patches', version, title)
                if entry[0] == self.KEY]

    def test_region_version_and_default(self):
        found = self.found()
        self.assertEqual(len(found), 1)
        self.assertEqual(self.found(title='CUSA13795'), [])
        self.assertEqual(self.found(version='01.00'), [])
        self.assertEqual(found[0][2].get('isEnabled'), 'false')
        self.assertEqual(patches.external_selection(found, None, {}), [])
        self.assertEqual(patches.external_selection(found, None, {'patch_unlock_all': '0'}), [])

    def test_ini_and_canonical_json_selection(self):
        found = self.found()
        self.assertEqual(len(found), 1)
        self.assertEqual(patches.selection_key(self.KEY), self.CANONICAL)
        self.assertEqual(patches.external_selection(found, None, {'patch_unlock_all': '1'}), found)
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / 'patches.json'
            config.write_text(json.dumps({'enabled': [self.CANONICAL], 'disabled': []}))
            self.assertEqual(patches.external_selection(found, config), found)
            config.write_text(json.dumps({'enabled': [], 'disabled': [self.CANONICAL]}))
            self.assertEqual(patches.external_selection(found, config), [])
            self.assertEqual(patches.external_selection(found, config, {'patch_unlock_all': '1'}), found)

    def test_all_24_writes_have_matching_instruction_lengths(self):
        found = self.found()
        self.assertEqual(len(found), 1)
        lines = list(found[0][2].iter('Line'))
        usa = next(meta for key, _, meta in patches.external_patches(ROOT / 'patches')
                   if key == self.CANONICAL)
        usa_lines = list(usa.iter('Line'))
        self.assertEqual(len(lines), 24)
        self.assertEqual(len(usa_lines), 24)
        self.assertEqual(len({line.get('Address') for line in lines}), 24)
        for i, (line, original) in enumerate(zip(lines, usa_lines)):
            with self.subTest(index=i):
                delta = 0 if i < 6 else -0x10
                self.assertEqual(int(line.get('Address'), 0), int(original.get('Address'), 0) + delta)
                value = patches.encode(line)
                self.assertEqual(value, patches.encode(original))
                signature = bytes.fromhex(line.get('Signature'))
                offset = int(line.get('SignatureOffset'), 0)
                expected = bytes.fromhex('4180be8100000000' if i == 6 else '80bb8100000000')
                self.assertEqual(signature[offset:offset + len(value)], expected)
                self.assertEqual(len(value), len(expected))

    def test_launcher_mappings_use_shared_ini_key(self):
        tree = ast.parse((ROOT / 'launcher/sacredfire_launcher.py').read_text(encoding='utf-8'))
        mapping = next(ast.literal_eval(node.value) for node in tree.body
                       if isinstance(node, ast.Assign)
                       and any(isinstance(t, ast.Name) and t.id == 'INI_PATCHES' for t in node.targets))
        self.assertEqual(mapping[self.KEY], 'patch_unlock_all')
        windows = (ROOT / 'launcher/windows/launcher.cpp').read_text(encoding='utf-8')
        self.assertIn('{"' + self.KEY + '", "patch_unlock_all"}', windows)

    def test_usa_patch_remains_selected(self):
        found = patches.external_patches(ROOT / 'patches', '01.21', 'CUSA13795')
        with patch.dict('os.environ', {'BB_ONLINE': '0'}):
            selected = patches.external_selection(found, None, {'patch_unlock_all': '1'})
        self.assertEqual([key for key, _, _ in selected], [self.CANONICAL])


if __name__ == '__main__':
    unittest.main()
