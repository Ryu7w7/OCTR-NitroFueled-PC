"""Check all shipped track coverage and HUD-only overlays against a local dump."""
from paths import ROOT  # noqa: F401
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from archive_reader import Archive
from igz_file import Igz
import speed_mode


@unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires native USA/EU dump')
class ItemlessTracks(unittest.TestCase):
    def test_all_tracks_share_original_map_assets(self):
        game = Path(os.environ['BB_TEST_GAME'])
        written = []
        write = speed_mode.write_entries_patch

        def hud_only(target, pak, guest, entries, base):
            archive = Archive(pak)
            try:
                paths = {archive.files[i]['path'] for i in entries}
                self.assertEqual(paths, {speed_mode.WUMPA_HUD, speed_mode.ITEM_HUD})
                written.append(guest)
            finally:
                archive.data.close()
                archive._file.close()
            return write(target, pak, guest, entries, base)

        with tempfile.TemporaryDirectory() as tmp, \
                patch.object(speed_mode, 'write_entries_patch', hud_only):
            normal, itemless, count = speed_mode.prepare(game, Path(tmp), [])
            self.assertEqual(count, 40)
            self.assertEqual(len(normal), len(speed_mode.HUD_ARCHIVES))
            self.assertEqual(len(itemless), len(normal))
            self.assertEqual(len(written), 2 * len(normal))
            self.assertTrue(all(Path(p).name in speed_mode.HUD_ARCHIVES for p in written))

    def test_native_time_trial_layer_policy(self):
        archive = Archive(Path(os.environ['BB_TEST_GAME']) / 'archives/update2.pak')
        try:
            entry = next(f for f in archive.files if f['path'] == 'looseData/systemData.igz')
            igz = Igz(archive.extract(entry))
        finally:
            archive.data.close()
            archive._file.close()
        rules = {}
        for off, kind in igz.objects():
            if kind == 'COctaneGameModeFilter':
                modes = igz.u64(off + 0x18)
                data = igz.u64(modes + 0x20)
                rules[igz.string_at(off + 0x10)] = [
                    igz.u32(data + 4 * i) for i in range(igz.u32(modes + 0xc))]
        self.assertIn(2, rules['RaceSystem'])
        for layer in ('Power_Up', 'Race_Relic', 'Race_Token', 'Race_Crystal',
                      'Boss_Race', 'Cup_Race'):
            self.assertNotIn(2, rules[layer], layer)


if __name__ == '__main__':
    unittest.main()
