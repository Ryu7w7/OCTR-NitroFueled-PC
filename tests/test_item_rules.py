"""scripts/item_rules.py on the game's own systemData.igz: BB_TEST_GAME names a game folder
(CUSA13795 1.21); without it the tests are skipped."""
from paths import ROOT  # noqa: F401  (scripts/ on the path)
import json
import os
import tempfile
import unittest
from pathlib import Path

import archive_patch
import item_rules
from archive_reader import Archive

GAME = os.environ.get('BB_TEST_GAME', '')


@unittest.skipUnless(GAME and Path(GAME, 'archives', item_rules.ARCHIVE).is_file(), 'BB_TEST_GAME not set')
class ItemRulesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        archive = Archive(Path(GAME, 'archives', item_rules.ARCHIVE))
        cls.blob = bytes(archive.extract(item_rules.entry_of(archive)))
        cls.original = item_rules.read_rules(cls.blob)

    def test_the_games_odds(self):
        self.assertEqual(len(self.original['race']), 8)
        self.assertEqual(self.original['race'][0]['Beaker'], 42)          # 1st place: mostly potions
        self.assertEqual(self.original['race'][6]['Invincibility Mask'], 50)  # 7th: masks
        self.assertEqual(self.original['cooldowns'], {"N'Tropy Clock": 40, 'Warp Orb': 40})
        self.assertIn('Super Engine', self.original['battle'])

    def test_rules_are_written_in_place(self):
        turbo = {name: (100 if name == 'Turbo Boost' else 0) for name in self.original['race'][0]}
        changed, notes = item_rules.apply(self.blob, {'race': [turbo] * 8, 'cooldowns': {'Warp Orb': 5},
                                                      'bacon': {'Bomb': 0, 'Nothing': 3}})
        self.assertEqual(len(changed), len(self.blob))
        self.assertEqual(notes, ['Nothing cannot be added to the bacon table'])
        back = item_rules.read_rules(changed)
        self.assertEqual(back['race'], [turbo] * 8)
        self.assertEqual(back['cooldowns'], {"N'Tropy Clock": 40, 'Warp Orb': 5})
        self.assertEqual(back['bacon']['Bomb'], 0)
        self.assertEqual(back['battle'], self.original['battle'])  # untouched

    def test_an_item_the_table_lacks_is_added(self):
        changed, notes = item_rules.apply(self.blob, {'race': [{'Super Engine': 50, 'Invisibility': 0}] + [{}] * 7})
        self.assertEqual(notes, ['1 item(s) added to the position 1 table (experimental)'])
        back = item_rules.read_rules(changed)
        self.assertEqual(back['race'][0]['Super Engine'], 50)
        self.assertEqual(back['race'][0]['Beaker'], 42)  # the others stay
        self.assertNotIn('Super Engine', back['race'][1])
        self.assertNotIn('Invisibility', back['race'][0])  # weight 0: nothing added
        self.assertEqual(len(changed), len(self.blob))

    def test_no_rules_change_nothing(self):
        self.assertEqual(item_rules.apply(self.blob, {})[0], self.blob)

    def test_bad_values_are_refused(self):
        for bad in ({'cooldowns': {'Warp Orb': -1}}, {'race': [{'Bomb': 'x'}]}, {'race': [{}] * 9},
                    {'writes': [{'sha256': self.original['sha256'], 'offset': 3, 'f32': 1}]}):
            with self.assertRaises(ValueError, msg=bad):
                item_rules.apply(self.blob, bad)

    def test_raw_writes_only_for_this_entry(self):
        races = item_rules.Tables(self.blob).sections()['race']
        at = item_rules.Tables(self.blob).weights(races[0])['Bomb']
        changed, notes = item_rules.apply(self.blob, {'writes': [
            {'sha256': self.original['sha256'], 'offset': at, 'f32': 77},
            {'sha256': '0' * 64, 'offset': at, 'f32': 1}]})
        self.assertEqual(item_rules.read_rules(changed)['race'][0]['Bomb'], 77)
        self.assertEqual(len(notes), 1)

    def test_patch_keeps_the_base_patch(self):
        with tempfile.TemporaryDirectory() as out:
            rules = {'cooldowns': {'Warp Orb': 10}}
            self.assertEqual(item_rules.prepare(GAME, out, None, {}), {})
            first = item_rules.prepare(GAME, out, rules, {})
            guest = f'/app0/archives/{item_rules.ARCHIVE}'
            size, patched_guest, ranges = archive_patch.read(first[guest])
            self.assertEqual(patched_guest, guest)
            self.assertTrue(ranges)
            stamp = Path(out, 'item_rules', 'stamp.json').read_text(encoding='utf-8')
            self.assertEqual(item_rules.prepare(GAME, out, rules, {}), first)  # kept
            self.assertEqual(json.loads(stamp), json.loads(Path(out, 'item_rules', 'stamp.json').read_text(encoding='utf-8')))


if __name__ == '__main__':
    unittest.main()
