"""Xbox button prompts (scripts/button_prompts.py): the font's button image found through its
objects, the buttons copied to the cells its character metrics give, the PS4 tiling, the art PNG.
Synthetic fonts only; no game data."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import button_prompts as bp  # noqa: E402

W, H = 512, 263


def packed(offsets):
    """A packed offset list (igz RVTB): deltas in 4-byte units, 3 bits a nibble, low first."""
    nibbles, previous = [], 0
    for offset in offsets:
        value = (offset - previous) // 4
        previous = offset
        while True:
            nibble = value & 7
            value >>= 3
            nibbles.append(nibble | (8 if value else 0))
            if not value:
                break
    if len(nibbles) % 2:
        nibbles.append(0)
    return bytes(nibbles[i] | nibbles[i + 1] << 4 for i in range(0, len(nibbles), 2))


def strings(names):
    out = b''
    for name in names:
        out += name.encode() + b'\0'
        if len(out) % 2:
            out += b'\0'
    return out


def fixup(tag, count, data):
    return tag.encode() + struct.pack('<III', count, 16 + len(data), 16) + data


def font(cells, pixels):
    """An IGZ font: an igImage2 "button" (W x H, tiled pixels) and an igCharMetrics per
    {character: (x0, y0, x1, y1)} (end inclusive, as the game's metrics)."""
    objects = bytearray(0x60 + 0x48 * len(cells))
    struct.pack_into('<Q', objects, 0, 0)                 # type 0: igImage2
    struct.pack_into('<Q', objects, 16, 0)                # name: string 0, "button"
    struct.pack_into('<HH', objects, bp.IMG_W, W, H)
    struct.pack_into('<QQ', objects, bp.IMG_DATA, len(pixels), 1 << 27)  # section 2, offset 0
    starts = [0]
    for i, (char, (x0, y0, x1, y1)) in enumerate(cells.items()):
        at = 0x60 + 0x48 * i
        starts.append(at)
        struct.pack_into('<Q', objects, at, 1)            # type 1: igCharMetrics
        struct.pack_into('<H', objects, at + bp.CM_CHAR, char)
        struct.pack_into('<4f', objects, at + bp.CM_UV, x0 / W, y0 / H, x1 / W, y1 / H)
    fixups = (fixup('TMET', 2, strings(['igImage2', 'igCharMetrics'])) + fixup('TSTR', 1, strings(['button']))
              + fixup('RVTB', len(starts), packed(starts)))
    header = bytearray(0x800)
    sections = [(0x800, len(fixups)), (0x800 + len(fixups), len(objects)),
                (0x800 + len(fixups) + len(objects), len(pixels))]
    struct.pack_into('<5I', header, 0, 0x49475A01, 10, 0, 0, 3)
    for i, (offset, length) in enumerate(sections):
        struct.pack_into('<4I', header, 0x14 + 16 * i, 0, offset, length, 0)
    return bytes(header) + fixups + bytes(objects) + pixels


def inclusive(cell):
    x0, y0, x1, y1 = cell
    return x0, y0, x1 - 1, y1 - 1


class ButtonPromptTests(unittest.TestCase):
    def setUp(self):
        # The game's image: every texel (x, y, 7, 255) in memory order; the art: (200, y, x % 256, 255)
        self.linear = bytes(b for y in range(H) for x in range(W) for b in (x & 255, y & 255, 7, 255))
        self.tiled = bp.tile(self.linear, W, H, bytes(((W + 7) // 8 * 8) * ((H + 7) // 8 * 8) * 4))
        self.art = (W, H, bytes(b for y in range(H) for x in range(W) for b in (200, y & 255, x & 255, 255)))

    def test_tiling_round_trip(self):
        self.assertEqual(bp.untile(self.tiled, W, H), self.linear)
        self.assertNotEqual(self.tiled[:W * 4], self.linear[:W * 4])  # really tiled

    def check(self, codes, layout):
        """The art's buttons land in the font's cells, stored BGRA; nothing else changes."""
        blob = font({code: inclusive(layout[name]) for code, name in codes.items()}, self.tiled)
        made = bp.restyled_font(blob, self.art)
        self.assertEqual(len(made), len(blob))
        w, h, at, length = bp.image_location(made)
        self.assertEqual(made[:at], blob[:at])
        linear = bp.untile(made[at:at + length], w, h)
        art = self.art[2]
        replaced = set()
        for name, (x0, y0, x1, y1) in layout.items():
            if name not in codes.values():
                continue
            sx0, sy0 = bp.SHEET[name][:2]
            for y in (y0, y1 - 1):
                for x in (x0, x1 - 1):
                    i, j = (y * W + x) * 4, ((sy0 + y - y0) * W + sx0 + x - x0) * 4
                    self.assertEqual(linear[i:i + 4], bytes((art[j + 2], art[j + 1], art[j], art[j + 3])))
            replaced |= {(x, y) for y in range(y0, y1) for x in range(x0, x1)}
        untouched = [(x, y) for (x, y) in ((500, 5), (200, 100), (480, 250)) if (x, y) not in replaced]
        for x, y in untouched:
            i = (y * W + x) * 4
            self.assertEqual(linear[i:i + 4], self.linear[i:i + 4])
        return linear

    def test_update_fonts(self):
        codes = {c: n for c, n in bp.CODES.items() if c >= 0xE000}
        self.check(codes, bp.SHEET)

    def test_original_fonts_have_other_codes_and_cells(self):
        # The 1.00 fonts: ASCII characters, no touchpad, L2/R2 further left.
        codes = {c: n for c, n in bp.CODES.items() if c < 0xE000}
        layout = dict(bp.SHEET, L2=(170, 170, 271, 263), R2=(271, 170, 372, 263))
        del layout['Touchpad']
        self.check(codes, layout)

    def test_letters_with_button_codes_are_not_buttons(self):
        # '[' is also a letter of the font: a small cell is not the L1 button.
        cells = {code: inclusive(bp.SHEET[name]) for code, name in bp.CODES.items() if code >= 0xE000}
        cells[0x5B] = (10, 10, 30, 40)
        blob = font(cells, self.tiled)
        found = bp.button_cells(blob, W, H)
        self.assertEqual(found['L1'], bp.SHEET['L1'])

    def test_font_without_buttons_is_refused(self):
        blob = font({0x41: (0, 0, 20, 20)}, self.tiled)
        with self.assertRaises(ValueError):
            bp.restyled_font(blob, self.art)

    def test_png_round_trip_and_art(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'a.png'
            rgba = bytes(range(256)) * (16 * 3 * 4 // 256 + 1)
            rgba = rgba[:16 * 3 * 4]
            bp.write_png(path, 16, 3, rgba)
            self.assertEqual(bp.read_png(path), (16, 3, rgba))
        w, h, art = bp.read_png(bp.ART / 'xbox.png')  # the art we ship: the font's size, buttons drawn
        self.assertEqual((w, h), (W, H))
        for name, (x0, y0, x1, y1) in bp.SHEET.items():
            middle = (((y0 + y1) // 2) * W + (x0 + x1) // 2) * 4
            self.assertEqual(art[middle + 3], 255, name)

    def test_chosen_style(self):
        with tempfile.TemporaryDirectory() as tmp:
            ini = Path(tmp) / 'bbport.ini'
            ini.write_text('show_fps=1\nbutton_prompts = Xbox\n')
            self.assertEqual(bp.chosen_style(ini, {}), 'xbox')
            self.assertEqual(bp.chosen_style(ini, {'BB_BUTTON_PROMPTS': 'playstation'}), 'playstation')
            ini.write_text('button_prompts=switch\n')
            self.assertEqual(bp.chosen_style(ini, {}), 'playstation')
            self.assertEqual(bp.prepare(Path(tmp), Path(tmp) / 'out', ini, {}, {}), {})


if __name__ == '__main__':
    unittest.main()
