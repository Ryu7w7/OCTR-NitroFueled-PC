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

    def test_packages_with_one_file_name_and_different_contents_clash(self):
        def package(ident, entries):
            folder = self.dir / ident / 'archives'
            folder.mkdir(parents=True)
            path = pak(folder / f'{ident}.pak', entries)
            m = {'id': ident, 'files': [{'file': f'archives/{ident}.pak', 'sha256': ident + '-sum'}]}
            return m, self.dir / ident
        road = 'textures/road.igz'
        a = package('t901_a', {road: b'road of a', 'textures/same.igz': b'same', 'maps/a.igz': b'a'})
        b = package('t902_b', {road: b'road of b', 'textures/same.igz': b'same', 'maps/b.igz': b'b'})
        c = package('t903_c', {road: b'road of a', 'maps/c.igz': b'c'})
        cache = self.dir / 'hashes.json'
        expected = {('t901_a', 't902_b'): 1, ('t902_b', 't903_c'): 1}
        self.assertEqual(cc.clashes([a, b, c], cache), expected)
        self.assertTrue(cache.is_file())
        self.assertEqual(cc.clashes([a, b, c], cache), expected)  # from the cache
        self.assertEqual(cc.clashes([a, c], cache), {})

    def test_listing_field(self):
        self.assertEqual(cc.field(None), '')
        self.assertEqual(cc.field({'level': 'partial', 'tested': True, 'issues': {'game_scripts': 2}}),
                         'partial;1;game_scripts=2')
        # a package converted before the checks: known only when its mod was tried in game
        self.assertEqual(cc.of_manifest({'convertedFrom': {'id': 'suzuka', 'version': '0.7.2'}})['level'], cc.PARTIAL)
        self.assertIsNone(cc.of_manifest({'convertedFrom': {'id': 'other', 'version': '1'}}))


if __name__ == '__main__':
    unittest.main()
