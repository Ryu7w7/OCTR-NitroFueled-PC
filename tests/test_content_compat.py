"""content_compat.py: full or partial compatibility of a converted track, on synthetic archives."""
import shutil
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

import paths  # noqa: F401
import content_compat as cc
import content_delta as cd
import track_editor as te
from graphics_tab import write_archive


def pak(path, entries):
    write_archive(path, [('t/' + p, p, i << 8, d) for i, (p, d) in enumerate(entries.items())],
                  SimpleNamespace(flags=0))
    return path


class CompatTests(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        (self.dir / 'game' / 'archives').mkdir(parents=True)
        pak(self.dir / 'game' / 'archives' / 'update2.pak',
            {'vsc/common/race/turbo.igz': b'game script', 'ui/hud.igz': b'game hud',
             'textures/rock.igz': b'game rock', 'loosedata/presets.igz': b'game presets'})
        self.game = cd.Game(self.dir / 'game')
        self.addCleanup(self.game.close)

    def track(self, entries):
        return pak(self.dir / 't999_x.pak', dict({'maps/octane/t999_x/t999_x.igz': b'map'}, **entries))

    def test_own_files_and_changed_looks_are_full(self):
        track = self.track({'textures/rock.igz': b'a rock of its own', 'vsc/common/race/turbo.igz': b'game script'})
        self.assertEqual(cc.assess(self.game, [track], te.ALL),
                         {'level': cc.FULL, 'tested': False, 'issues': {}})

    def test_changed_game_scripts_menus_and_data_are_partial(self):
        track = self.track({'vsc/common/race/turbo.igz': b'changed', 'ui/hud.igz': b'changed',
                            'loosedata/presets.igz': b'changed'})
        result = cc.assess(self.game, [track], te.ALL)
        self.assertEqual(result['level'], cc.PARTIAL)
        self.assertEqual(result['issues'], {'game_data': 1, 'game_menus': 1, 'game_scripts': 1})

    def test_another_tool_s_counts_are_partial(self):
        result = cc.assess(self.game, [self.track({})], te.REACHED)
        self.assertEqual((result['level'], result['issues']), (cc.PARTIAL, {'other_tool': 1}))

    def test_a_mod_tried_in_game_keeps_its_result(self):
        track = self.track({'vsc/common/race/turbo.igz': b'changed'})
        self.assertEqual(cc.assess(self.game, [track], te.ALL, 'ctr-editor-backrooms', '1.5.1')['level'], cc.FULL)
        self.assertTrue(cc.assess(self.game, [self.track({})], te.ALL, 'nbc', '1.2.0')['tested'])

    def test_listing_field(self):
        self.assertEqual(cc.field(None), '')
        self.assertEqual(cc.field({'level': 'partial', 'tested': True, 'issues': {'game_scripts': 2}}),
                         'partial;1;game_scripts=2')
        # a package converted before the checks: known only when its mod was tried in game
        self.assertEqual(cc.of_manifest({'convertedFrom': {'id': 'suzuka', 'version': '0.7.2'}})['level'], cc.PARTIAL)
        self.assertIsNone(cc.of_manifest({'convertedFrom': {'id': 'other', 'version': '1'}}))


if __name__ == '__main__':
    unittest.main()
