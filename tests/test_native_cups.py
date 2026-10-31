"""Read-only checks of the actual native CUP data; no game data is distributed."""
from paths import ROOT
import os
import unittest

from igz_file import Igz
import inspect_native_cups as cups


@unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires a local CTR 1.21 dump')
class NativeCupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.blob = cups.read_game(os.environ['BB_TEST_GAME'])

    def test_native_arcade_cups_and_points(self):
        report = cups.inspect(self.blob)
        self.assertEqual(report['position_points'], [9, 6, 3, 1])
        arcade = [cup for cup in report['cups'] if not cup['gem_cup']]
        self.assertEqual(len(arcade), 11)
        self.assertEqual(len({cup['name'] for cup in arcade}), 11)
        self.assertEqual(len({cup['tracks'][0].lower() for cup in arcade}), 11)
        for cup in arcade:
            self.assertEqual(len(cup['tracks']), 4)
            self.assertEqual(len(set(cup['tracks'])), 4)
        self.assertEqual(arcade[0]['name'], 'wumpaCup')
        self.assertEqual([t.rsplit('/', 1)[-1].lower() for t in arcade[0]['tracks']],
                         ['t111_crash_cove', 't132_tiger_temple', 't141_blizzard_bluff',
                          't131_coco_park'])

    def test_rejects_broken_native_cup_reference(self):
        g = Igz(self.blob)
        system = next(o for o, t in g.objects() if t == 'CSystemData')
        g.set_u64(system + 0x188, 0)
        with self.assertRaisesRegex(ValueError, 'COctaneCupSelectDataListList'):
            cups.inspect(g.build())

    def test_rejects_non_four_track_cup(self):
        g = Igz(self.blob)
        cup = next(o for o, t in g.objects() if t == 'COctaneCupSelectDataList')
        g.set_u32(cup + 12, 3)
        with self.assertRaisesRegex(ValueError, 'four tracks'):
            cups.inspect(g.build())
