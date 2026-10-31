"""16:10 HUD geometry contracts; native asset checks require BB_TEST_GAME."""
from paths import ROOT
import os
import struct
import unittest
from pathlib import Path

from archive_reader import Archive
import aspect_hud
from widescreen_startup import IGZ


BOOST = 'ui/GuiSharedAssets/Octane_NFBoostBar.igz'
ASSETS = (
    ('update.pak', BOOST),
    ('juicedomain_hub.pak', BOOST),
    ('juicedomain_race.pak', BOOST),
    ('juicedomain_race_flareday.pak', BOOST),
    ('juicedomain_race_flarenight.pak', BOOST),
)
OUTPUTS = ((1280, 800), (1920, 1200), (2560, 1600))


def named(ig, name):
    return next(o for o in ig.objects if o.get('name') == name)


def floats(data, obj, field, count=1):
    return struct.unpack_from('<' + str(count) + 'f', data, obj['offset'] + field)


class InvalidHudTests(unittest.TestCase):
    def test_malformed_assets_fail_without_changing_the_input(self):
        # These are deliberately invalid headers, not excerpts from a game asset.
        for source in (b'', b'not an IGZ asset', bytes(256),
                       struct.pack('<5I', 0x49475a01, 10, 0, 0, 0)):
            with self.subTest(length=len(source)):
                data = bytearray(source)
                with self.assertRaises(ValueError):
                    aspect_hud.correct_boost_bar(data)
                self.assertEqual(data, source)


@unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires a local USA/EU 1.21 game')
class NativeAspectHudTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        game = Path(os.environ['BB_TEST_GAME'])
        cls.assets = {}
        for archive_name in sorted({name for name, _ in ASSETS}):
            archive = Archive(game / 'archives' / archive_name)
            try:
                for name, target in ASSETS:
                    if name != archive_name:
                        continue
                    entry = next(e for e in archive.files if e['path'].casefold() == target.casefold())
                    cls.assets[(archive_name, target)] = archive.extract(entry)
            finally:
                archive.data.close()
                archive._file.close()

    def assert_only_float_fields_changed(self, before, after, allowed):
        self.assertIsInstance(after, bytes)
        self.assertEqual(len(before), len(after))
        allowed_bytes = {i for at in allowed for i in range(at, at + 4)}
        changed = {i for i, (a, b) in enumerate(zip(before, after)) if a != b}
        self.assertTrue(changed)
        self.assertFalse(changed - allowed_bytes)
        for at in allowed:
            self.assertNotEqual(before[at:at + 4], after[at:at + 4])
        old, new = IGZ(before), IGZ(after)
        self.assertEqual(old.chunks, new.chunks)
        self.assertEqual(old.fix, new.fix)  # all reference/fixup tables, strings and metadata
        self.assertEqual(old.objects, new.objects)
        self.assertEqual(before[:old.chunks[1][0]], after[:new.chunks[1][0]])

    def test_shared_boost_bar_keeps_its_bottom_and_pixel_proportions(self):
        for (archive, target), source in self.assets.items():
            if target != BOOST:
                continue
            with self.subTest(archive=archive):
                input_data = bytearray(source)
                result = aspect_hud.correct_boost_bar(input_data)
                self.assertEqual(input_data, source)
                old, new = IGZ(source), IGZ(result)
                before = named(old, 'Null_Asset_BarMaster')
                after = named(new, 'Null_Asset_BarMaster')
                old_y = floats(source, before, 0x64)[0]
                new_y = floats(result, after, 0x64)[0]
                old_sy = floats(source, before, 0x88)[0]
                sx, sy = floats(result, after, 0x84, 2)
                pivot_y = floats(source, before, 0x70)[0]
                height = floats(source, before, 0x94)[0]
                self.assertAlmostEqual(old_y + old_sy * height * (1 - pivot_y),
                                       new_y + sy * height * (1 - pivot_y), places=5)
                for width, output_height in OUTPUTS:
                    self.assertAlmostEqual(sx * width / 1920,
                                           sy * output_height / 1080, places=6)
                charging = named(new, 'Effect_ctr_gui_boost_bar_charging_vfx')
                self.assertEqual(floats(result, charging, 0x88)[0], .25)
                self.assertEqual(floats(result, charging, 0x94)[0], 2.5)
                self.assert_only_float_fields_changed(source, result, {
                    before['offset'] + 0x64, before['offset'] + 0x88,
                    charging['offset'] + 0x88, charging['offset'] + 0x94,
                })

    def test_unexpected_native_transforms_are_rejected_without_input_mutation(self):
        cases = (
            (BOOST, 'Null_Asset_BarMaster', 0x88, float('nan')),
            (BOOST, 'Octane_NFBoostBar_asset_root', 0x90, 480),
        )
        for target, name, field, value in cases:
            with self.subTest(target=target, name=name, field=field):
                source = self.assets[('update.pak', target)]
                obj = named(IGZ(source), name)
                data = bytearray(source)
                struct.pack_into('<f', data, obj['offset'] + field, value)
                snapshot = bytes(data)
                with self.assertRaises(ValueError):
                    aspect_hud.correct_boost_bar(data)
                self.assertEqual(data, snapshot)


if __name__ == '__main__':
    unittest.main()
