"""16:10 HUD geometry contracts; native asset checks require BB_TEST_GAME."""
from paths import ROOT
import os
import struct
import unittest
from pathlib import Path

from archive_reader import Archive
import aspect_hud
from widescreen_startup import IGZ


ADVENTURE = 'ui/Octane_Adventure_HUD.igz'
BOOST = 'ui/GuiSharedAssets/Octane_NFBoostBar.igz'
ASSETS = (
    ('update.pak', ADVENTURE),
    ('juicedomain_hub.pak', ADVENTURE),
    ('update.pak', BOOST),
    ('juicedomain_hub.pak', BOOST),
    ('juicedomain_race.pak', BOOST),
    ('juicedomain_race_flareday.pak', BOOST),
    ('juicedomain_race_flarenight.pak', BOOST),
)
EFFECTS = (
    'Effect_Octane_HUD_Relic_Silver_vfx001',
    'Effect_Octane_HUD_Relic_Gold_vfx001',
    'Effect_Octane_HUD_Relic_Blue_vfx',
    'Effect_Octane_HUD_Keys_vfx',
    'Effect_Octane_HUD_Trophy_vfx',
)
OUTPUTS = ((1280, 800), (1920, 1200), (2560, 1600))


def named(ig, name):
    return next(o for o in ig.objects if o.get('name') == name)


def floats(data, obj, field, count=1):
    return struct.unpack_from('<' + str(count) + 'f', data, obj['offset'] + field)


def array(ig, pointer, count):
    at = ig.offset(pointer)
    return struct.unpack_from('<' + str(count) + 'Q', ig.data, at)


def parent_affine(ig, owner):
    """Compose the authored parent translations, pivot offsets and X scales."""
    by_ptr = {o['ptr']: o for o in ig.objects}
    pointer = ig.q(owner['offset'] + 0x40)
    chain = []
    while pointer in by_ptr and by_ptr[pointer]['type'] == 'igGuiPlaceable':
        obj = by_ptr[pointer]
        chain.append(obj)
        pointer = ig.q(obj['offset'] + 0x40)
    translation, scale = 0.0, 1.0
    for obj in reversed(chain):
        x = floats(ig.data, obj, 0x60)[0]
        pivot = floats(ig.data, obj, 0x6c)[0]
        width = floats(ig.data, obj, 0x90)[0]
        local_scale = floats(ig.data, obj, 0x84)[0]
        translation += scale * (x - local_scale * pivot * width)
        scale *= local_scale
    return translation, scale


def position_keys(ig, owner):
    """Read the owner's position-animation keys independently of the transform helper."""
    by_ptr = {o['ptr']: o for o in ig.objects}
    table_ptr = ig.q(owner['offset'] + 0xf0)
    if not table_ptr:
        return []
    table = by_ptr[table_ptr]
    byte_count = struct.unpack_from('<I', ig.data, table['offset'] + 0x10)[0]
    animations = array(ig, ig.q(table['offset'] + 0x18), byte_count // 8)
    keys = []
    for pointer in set(animations) - {0}:
        animation = by_ptr[pointer]
        tracks = by_ptr[ig.q(animation['offset'] + 0x18)]
        count = struct.unpack_from('<I', ig.data, tracks['offset'] + 12)[0]
        for track_ptr in array(ig, ig.q(tracks['offset'] + 0x20), count):
            track = by_ptr[track_ptr]
            if track['type'] != 'igGuiVec3Track' or ig.q(track['offset'] + 0x18) != 2:
                continue
            key_list = by_ptr[ig.q(track['offset'] + 0x10)]
            count = struct.unpack_from('<I', ig.data, key_list['offset'] + 12)[0]
            for key_ptr in array(ig, ig.q(key_list['offset'] + 0x20), count):
                key = by_ptr[key_ptr]
                if key['type'] != 'igGuiVec3LinearKeyframe':
                    raise AssertionError('Unexpected native idle-position key type')
                keys.append(key)
    return keys


class InvalidHudTests(unittest.TestCase):
    def test_malformed_assets_fail_without_changing_the_input(self):
        # These are deliberately invalid headers, not excerpts from a game asset.
        for transform in (aspect_hud.correct_adventure_hud, aspect_hud.correct_boost_bar):
            for source in (b'', b'not an IGZ asset', bytes(256),
                           struct.pack('<5I', 0x49475a01, 10, 0, 0, 0)):
                with self.subTest(transform=transform.__name__, length=len(source)):
                    data = bytearray(source)
                    with self.assertRaises(ValueError):
                        transform(data)
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

    def test_collectibles_restore_authored_pivots_at_every_output(self):
        for (archive, target), source in self.assets.items():
            if target != ADVENTURE:
                continue
            with self.subTest(archive=archive):
                input_data = bytearray(source)
                result = aspect_hud.correct_adventure_hud(input_data)
                self.assertEqual(input_data, source)
                old, new = IGZ(source), IGZ(result)
                center = floats(source, named(old, 'Root_placeable'), 0x90)[0] / 2
                changed_fields = set()
                key_count = 0
                for name in EFFECTS:
                    owner, corrected = named(old, name), named(new, name)
                    parent, scale = parent_affine(old, owner)
                    self.assertAlmostEqual(scale, 1)
                    points = [(owner, corrected, 0x60)]
                    old_keys, new_keys = position_keys(old, owner), position_keys(new, corrected)
                    self.assertEqual(len(old_keys), len(new_keys))
                    self.assertEqual(len(old_keys), 0 if 'Silver' in name or 'Gold' in name else 3)
                    points += [(a, b, 0x18) for a, b in zip(old_keys, new_keys)]
                    key_count += len(old_keys)
                    for old_obj, new_obj, field in points:
                        original_x = parent + scale * floats(source, old_obj, field)[0]
                        corrected_x = parent + scale * floats(result, new_obj, field)[0]
                        changed_fields.add(old_obj['offset'] + field)
                        for width, height in OUTPUTS:
                            camera_x_scale = (16 / 9) / (width / height)
                            projected_x = center + camera_x_scale * (corrected_x - center)
                            # Compare in output pixels: the modified 3D icon must return to
                            # the same authored pivot as its independently drawn 2D count.
                            self.assertAlmostEqual(projected_x / (2 * center) * width,
                                                   original_x / (2 * center) * width, places=3)
                self.assertEqual(key_count, 9)
                self.assertEqual(len(changed_fields), 14)
                self.assert_only_float_fields_changed(source, result, changed_fields)
                size = old.fix['MTSZ'][old.fix['TMET'].index('igGuiPlaceable')]
                counts = [o for o in old.objects if o.get('name', '').startswith(
                    ('Text_CountMarker', 'Text_CountText'))]
                self.assertEqual(len(counts), 6)
                for obj in counts:
                    at = obj['offset']
                    self.assertEqual(source[at:at + size], result[at:at + size])

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
            (ADVENTURE, 'Root_placeable', 0x90, 1900),
            (ADVENTURE, 'Null_Asset_TopItems', 0x84, float('nan')),
            (ADVENTURE, 'Effect_Octane_HUD_Keys_vfx', 0x80, 5),
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
                transform = aspect_hud.correct_adventure_hud if target == ADVENTURE else aspect_hud.correct_boost_bar
                with self.assertRaises(ValueError):
                    transform(data)
                self.assertEqual(data, snapshot)
        source = self.assets[('update.pak', ADVENTURE)]
        ig = IGZ(source)
        key = position_keys(ig, named(ig, 'Effect_Octane_HUD_Trophy_vfx'))[0]
        data = bytearray(source)
        struct.pack_into('<f', data, key['offset'] + 0x18, float('nan'))
        snapshot = bytes(data)
        with self.assertRaises(ValueError):
            aspect_hud.correct_adventure_hud(data)
        self.assertEqual(data, snapshot)


if __name__ == '__main__':
    unittest.main()
