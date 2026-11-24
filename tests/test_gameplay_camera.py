import struct
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import gameplay_camera
import tacho_pack
from igz_file import Igz, unpack_offsets


class GameplayCameraTests(unittest.TestCase):
    def test_options_move_up_one_row_and_leave_the_help_area_clear(self):
        op, = tacho_pack.manifest()['menu_operations']
        original = Igz(tacho_pack.payload(op))
        page = Igz(tacho_pack.localized(op))
        controls = (0x1c90, 0x2910, 0x2a30, 0x2b50, 0x16e30, 0x17360)
        camera = page.u64(controls[-1] + 0x50)

        def y(asset, node):
            return struct.unpack_from('<f', asset.data, node + 0x64)[0]

        pitch = y(original, controls[-1]) - y(original, controls[-2])
        self.assertGreater(pitch, 0)
        for row in controls:
            self.assertEqual(y(page, row), y(original, row) - pitch)
        self.assertEqual(y(page, camera), y(original, controls[-1]))
        for decoration in (0x1ed0, 0x1db0):  # help separator and description
            self.assertEqual(page.data[decoration+0x60:decoration+0x98],
                             original.data[decoration+0x60:decoration+0x98])

    def test_localized_selector_preserves_stock_controls_and_navigation(self):
        op, = tacho_pack.manifest()['menu_operations']
        original = Igz(tacho_pack.payload(op))
        row, nav, toggle = 0x17360, 0x174b8, 0x176d8
        for language, labels in gameplay_camera.TEXT.items():
            with self.subTest(language=language), patch.object(tacho_pack.octr_texts, 'current_column', return_value=language):
                page = Igz(tacho_pack.localized(op))
                new = page.u64(row + 0x50)
                move = new - row
                self.assertEqual(page.u64(new + 0x58), row)
                self.assertEqual(page.u64(new + 0x50), original.u64(row + 0x50))
                self.assertEqual(page.u64(page.u64(new + 0x50) + 0x58), new)
                self.assertEqual(page.u64(nav + 0x118), new)
                self.assertEqual(page.u64(nav + move + 0x110), row)
                self.assertEqual(page.u64(nav + move + 0x118), original.u64(nav + 0x118))
                table = page.u64(page.u64(row + 0x40) + 0x48)
                self.assertEqual(page.u32(table + 0x10), original.u32(table + 0x10) + 1)
                data = page.u64(table + 0x20)
                children = [page.u64(data + 8*i) for i in range(page.u32(table + 0x10))]
                self.assertCountEqual(children, [original.u64(original.u64(table + 0x20) + 8*i)
                                                 for i in range(original.u32(table + 0x10))] + [new])
                controls = {p: page.u32(p + 0xdc) for p, kind in page.objects()
                            if kind == 'CGuiBehaviorGlobalToggleOption'}
                self.assertEqual(controls.pop(toggle + move), gameplay_camera.CAMERA_OPTION)
                self.assertEqual(controls, {p: original.u32(p + 0xdc) for p, kind in original.objects()
                                            if kind == 'CGuiBehaviorGlobalToggleOption'})
                self.assertEqual([page.string_at(p + move) for p in
                                  (nav + 0xe8, toggle + 0xb0, toggle + 0xa8, nav + 0x1b8)], list(labels))
                for kind in ('RVTB', 'RSTT', 'ROFS', 'RHND', 'RPID', 'RNEX'):
                    offsets = set(page.read_list(kind, unpack_offsets))
                    for source in original.read_list(kind, unpack_offsets):
                        if row <= source < 0x17830:
                            self.assertIn(source + move, offsets)


if __name__ == '__main__':
    unittest.main()
