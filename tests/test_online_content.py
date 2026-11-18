"""What a launch plays online with (scripts/online_content.py): public play takes character mods only.

The systemData checks need a USA 1.21 dump: BB_TEST_GAME_DIR=<the game folder> (skipped without one)."""
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import online_content as oc  # noqa: E402


class _Game:
    """A GameIndex stand-in: path -> the game's copies' digests"""

    def __init__(self, files):
        self.files = {k.lower(): {oc.sha(v)} for k, v in files.items()}

    def has(self, path):
        return path.lower() in self.files

    def digests(self, path):
        return self.files.get(path.lower(), set())

    def system_data(self):
        return None


class OnlineContentTests(unittest.TestCase):
    def test_trusted_patches_match_the_repository(self):
        """TRUSTED_PATCHES is the content of the package's own patches/*.xml (update it with patch_digests)"""
        self.assertEqual(oc.patch_digests(ROOT / 'patches'), oc.TRUSTED_PATCHES)

    def test_the_ports_own_patches_pass(self):
        """render/ (internal resolution, DLSS), theme/ and end_race/ are written by patches.py itself"""
        with tempfile.TemporaryDirectory() as d:
            applied = ['render/DLSS quality, 1440p to 2160p', 'theme/christmas', 'end_race/30']
            self.assertEqual(oc.patch_reasons(applied, [d], None, '01.21', 'CUSA13795'), [])
            self.assertTrue(oc.patch_reasons(['MyCheat.xml/Speed x2'], [d], None, '01.21', 'CUSA13795'))

    def test_another_servers_online_key_passes(self):
        """scripts/online_key_local.py: the same key patch with a local server's key; not another write"""
        with tempfile.TemporaryDirectory() as d:
            src = (ROOT / 'patches' / 'CrashTeamRacingOnline.xml').read_text(encoding='utf-8')
            line = src[src.index('Value="'):]
            value = line[7:line.index('"', 7)]
            Path(d, 'CrashTeamRacingOnline.xml').write_text(src.replace(value, 'AB' * (len(value) // 2)), encoding='utf-8')
            key = 'CrashTeamRacingOnline.xml/Online: custom Demonware key'
            self.assertEqual(oc.patch_reasons([key], [ROOT / 'patches', d], None, '01.21', 'CUSA13795'), [])
            moved = src.replace('Address="', 'Address="0x1', 1)
            Path(d, 'CrashTeamRacingOnline.xml').write_text(moved, encoding='utf-8')
            self.assertTrue(oc.patch_reasons([key], [ROOT / 'patches', d], None, '01.21', 'CUSA13795'))

    def test_entry_rules(self):
        game = _Game({'vsc/common/common_driver_vo_component.igz': b'script',
                      'levels/crashcove/track.igz': b'track',
                      'vfx/ctr/share/hurt.igz': b'fx'})
        ok = [('vsc/common/common_driver_vo_component.igz', b'script'),      # the game's own copy
              ('textures/ColorMap,textures@!x`tga,111.igz', b'new'),         # visual, new
              ('vfx/CTR/share/hurt.igz', b'changed fx'),                     # visual, replaced
              ('vsc/common/common_driver_victimfx.igz', b'drawn effect'),    # a visual script
              ('characterlisting.igz', b'list'),
              ('looseData/systemData_mx.lng', b'names')]
        for path, data in ok:
            self.assertEqual(oc.entry_reasons(path, data, game), [], path)
        bad = [('vsc/common/common_driver_vo_component.igz', b'changed script'),
               ('levels/CrashCove/track.igz', b'faster track'),
               ('scripts/items.igz', b'new rules')]
        for path, data in bad:
            self.assertTrue(oc.entry_reasons(path, data, game), path)
        # a systemData with no game copy to compare with is not taken on trust
        self.assertTrue(oc.entry_reasons('looseData/systemData.igz', b'x', game))

    def test_the_ports_own_files_are_not_mods(self):
        """online_mode.py's menus reach the game through the same file map as mods, from out/"""
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / 'out'
            (out / 'online-mode').mkdir(parents=True)
            menu = out / 'online-mode' / 'octane_race_ranking.pak'
            menu.write_bytes(b'not even an archive')
            a, b = b'archives/octane_race_ranking.pak', str(menu).encode()
            fm = out / 'online-mode' / 'cup-files.map'
            fm.write_bytes(b'BBFILE01' + struct.pack('<I', 1) + struct.pack('<II', len(a), len(b)) + a + b)
            game = Path(d) / 'game'
            (game / 'archives').mkdir(parents=True)
            self.assertEqual(oc.mod_reasons(game, game, fm, None, out), ([], 0))
            reasons, looked = oc.mod_reasons(game, game, fm, None, None)     # elsewhere: a mod
            self.assertTrue(reasons and looked == 1)

    def test_file_map_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'map.bin'
            a, b = b'archives/drivermario.pak', b'C:/mods/drivermario.pak'
            path.write_bytes(b'BBFILE01' + struct.pack('<I', 1) + struct.pack('<II', len(a), len(b)) + a + b)
            self.assertEqual(oc.read_file_map(path), [(a.decode(), b.decode())])


GAME = os.environ.get('BB_TEST_GAME_DIR')


@unittest.skipUnless(GAME and Path(GAME, 'archives', 'update2.pak').is_file(), 'needs BB_TEST_GAME_DIR (USA 1.21)')
class SystemDataTests(unittest.TestCase):
    """Racers added to the game's own systemData pass; any other change does not."""

    @classmethod
    def setUpClass(cls):
        cls.game = oc.GameIndex(GAME).system_data()

    def test_the_game_itself_is_clean(self):
        self.assertEqual(oc.system_data_reasons(self.game, self.game), [])

    def test_a_changed_kart_value_is_caught(self):
        canon = oc._Canon(self.game)
        section = canon.igz.sections[1][1]
        for kind in ('CKartBoostData', 'CGlobalKartData', 'CKartMaxSpeedModifierData'):
            off = next(o for o, t in canon.objs if t == kind)
            field = next(f for f in range(8, canon.size_of[kind], 4)
                         if off + f not in canon.refs | canon.strs | canon.mems and
                         0.01 < abs(struct.unpack_from('<f', canon.igz.data, off + f)[0]) < 1e5)
            blob = bytearray(self.game)
            value = struct.unpack_from('<f', blob, section + off + field)[0]
            struct.pack_into('<f', blob, section + off + field, value * 1.5)
            reasons = oc.system_data_reasons(bytes(blob), self.game)
            self.assertTrue(reasons and kind in reasons[0], (kind, reasons))


if __name__ == '__main__':
    unittest.main()
