"""Xbox button prompts (scripts/button_prompts.py): the font's button image found through its
objects, the buttons copied to the cells its character metrics give, the PS4 tiling, the art PNG.
Synthetic fonts only; no game data."""
import os
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

    def test_font_without_alternate_list_gets_no_speaker(self):
        blob = font({code: inclusive(bp.SHEET[name]) for code, name in bp.CODES.items() if code >= 0xE000}, self.tiled)
        self.assertIsNone(bp.with_speaker(blob, bp.read_png(bp.SPEAKER_ART)))

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'needs the game (BB_TEST_GAME)')
    def test_update_fonts_get_the_speaker(self):
        from archive_reader import Archive
        archive = Archive(Path(os.environ['BB_TEST_GAME']) / 'archives' / 'update.pak')
        files = {f['path'].replace('\\', '/').lower(): f for f in archive.files}
        glyph = bp.read_png(bp.SPEAKER_ART)
        for name in bp.FONTS:
            blob = bytes(archive.extract(files[name.lower()]))
            made = bp.with_speaker(blob, glyph)
            self.assertIsNotNone(made, name)
            self.assertEqual(len(made), len(blob))
            _, slots = bp._alternate_list(made)
            codes = [char for _, _, char in slots]
            self.assertEqual(codes[-1], bp.SPEAKER_CODE)
            self.assertEqual(codes, sorted(codes))           # still sorted, the new one last
            w, h, at, length = bp.image_location(made)
            linear = bp.untile(made[at:at + length], w, h)
            x0, y0 = bp.SPEAKER_CELL[:2]
            middle = (40 * glyph[0] + 20) * 4                  # inside the loudspeaker's box: white
            i = ((y0 + 40) * w + x0 + 20) * 4
            self.assertEqual(linear[i:i + 4], bytes((glyph[2][middle + 2], glyph[2][middle + 1], glyph[2][middle],
                                                      glyph[2][middle + 3])))
            self.assertIsNone(bp.with_speaker(made, glyph))   # only once

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'needs the game (BB_TEST_GAME)')
    def test_clan_brackets_in_game_fonts(self):
        from archive_reader import Archive
        archive = Archive(Path(os.environ['BB_TEST_GAME']) / 'archives' / 'update.pak')
        files = {f['path'].lower(): f for f in archive.files}
        for font_name in bp.FONTS:
            with self.subTest(font=font_name):
                blob = bytes(archive.extract(files[font_name.lower()]))
                original = bp.with_speaker(blob, bp.read_png(bp.SPEAKER_ART))
                made = bp.with_clan_brackets(original, 'outline' in font_name.lower())
                self.assertEqual(len(original), len(made))
                self.assertEqual(bp.with_clan_brackets(made, False), made)
                _, slots = bp._alternate_list(made)
                codes = [code for _, _, code in slots]
                self.assertEqual(codes, sorted(codes))
                self.assertEqual(set(codes), {code for _, _, code in bp._alternate_list(original)[1] if code} | {91, 93})
                w, h, at, size = bp.image_location(made)
                pixels = bp.untile(made[at:at + size], w, h)
                before = bp.untile(original[at:at + size], w, h)
                restored = bytearray(pixels)
                for _, metrics, code in slots:
                    if code not in (91, 93):
                        continue
                    u, v, right, bottom = struct.unpack_from('<4f', made, metrics + bp.CM_UV)
                    x, y = round(u * w), round(v * h)
                    gw, gh = round(right * w) - x + 1, round(bottom * h) - y + 1
                    def alpha(dx, dy):
                        return pixels[((y + dy) * w + x + dx) * 4 + 3]
                    self.assertEqual((gw, gh), (28, 76))
                    self.assertEqual((alpha(14, 6), alpha(14, gh - 6)), (255, 255))
                    self.assertEqual((alpha(6, 38), alpha(21, 38)), (255, 0) if code == 91 else (0, 255))
                    for row in range(gh):
                        offset = ((y + row) * w + x) * 4
                        restored[offset:offset + gw * 4] = before[offset:offset + gw * 4]
                self.assertEqual(restored, before)  # all other art, including the voice icon

    def test_clan_tile_edges_and_coverage(self):
        import clan_tag_font as clan
        pixels = bytearray(W * H * 4)
        clan.paint_tiles(pixels, W)
        for mask in range(clan.MASKS):
            x, y = clan.cell(mask)
            for dy in range(-1, 3):
                for dx in range(-1, 3):
                    bit = 2 * min(1, max(0, dy)) + min(1, max(0, dx))
                    expected = ((mask >> (2*bit)) & 3) * 85
                    self.assertEqual(pixels[((y+dy)*W+x+dx)*4+3], expected)

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'needs the game (BB_TEST_GAME)')
    def test_clan_tiles_in_game_fonts(self):
        import clan_tag_font as clan
        from archive_reader import Archive
        archive = Archive(Path(os.environ['BB_TEST_GAME']) / 'archives' / 'update.pak')
        files = {f['path'].lower(): f for f in archive.files}
        for name in bp.FONTS:
            with self.subTest(font=name):
                original = bytes(archive.extract(files[name.lower()]))
                base = bp.with_clan_brackets(bp.with_speaker(original, bp.read_png(bp.SPEAKER_ART)), 'outline' in name.lower())
                made = clan.add_native(base)
                slots = bp._alternate_list(made)[1]
                codes = [c for _, _, c in slots]
                self.assertEqual(codes, sorted(codes))
                self.assertEqual(set(codes), {c for _, _, c in bp._alternate_list(base)[1]} |
                                 {c for c, _, _ in clan.glyphs()} | set(range(0xe050,0xe050+18*64)))
                self.assertEqual(clan.add_native(made), made)
                offsets = {c: off for _, off, c in slots}
                self.assertEqual(struct.unpack_from('<f', made, offsets[clan.FIRST+1]+24)[0], 0)
                self.assertEqual(struct.unpack_from('<f', made, offsets[clan.ADVANCE]+24)[0], 4)
                from name_swatch_font import ALPHABET, COLORS, FIRST
                objs, names, file_offset = bp._objects(base)
                font = next(p for p,k in objs if k == 'igImage2' and
                            names[struct.unpack_from('<Q',base,p+16)[0]] == 'font')
                fw, fh = struct.unpack_from('<HH',base,font+24)
                start = file_offset(struct.unpack_from('<Q',base,font+64)[0])
                a = next(p for p,k in objs if k == 'igCharMetrics' and
                         struct.unpack_from('<H',base,p+12)[0] == ord('A'))
                def bounds(data, p, w, h):
                    return [round(v*d) for v,d in zip(struct.unpack_from('<4f',data,p+52),(w,h,w,h))]
                def pixel(data, start, w, x, y):
                    p = start + (((y//8)*(w//8) + x//8)*64 + bp._morton(x%8,y%8))*4
                    return data[p:p+4]
                l,t,r,b = bounds(base,a,fw,fh)
                x,y = next((x,y) for y in range(t,b) for x in range(l,r)
                           if pixel(base,start,fw,x,y) == bytes([255]*4))
                w,h,img,_ = bp.image_location(made)
                self.assertLessEqual(max(w, h), 16384, name)
                # The title screen and startup video use the alternate Cross
                # from Zoinks Outline. Check its full rectangle after repacking.
                original_icons = {code: off for _, off, code in bp._alternate_list(original)[1]}
                iw, ih, old_image, _ = bp.image_location(original)
                icon = 0xe000
                rect = bounds(original, original_icons[icon], iw, ih)
                self.assertEqual(bounds(made, offsets[icon], w, h), rect)
                left, top, right, bottom = rect
                for py in range(top, bottom+1):
                    for px in range(left, right+1):
                        self.assertEqual(pixel(made, img, w, px, py),
                                         pixel(original, old_image, iw, px, py))

                for i,rgb in enumerate(COLORS):
                    painted = offsets[FIRST+i*len(ALPHABET)]
                    self.assertEqual(made[painted+16:painted+52], base[a+16:a+52])
                    px,py,_,_ = bounds(made,painted,w,h)
                    self.assertEqual(pixel(made,img,w,px+x-l,py+y-t),
                                     bytes((rgb & 255,(rgb>>8)&255,rgb>>16,255)))
                # Moving the original brackets into the larger sheet preserves their ink.
                old_alt = {c:o for _,o,c in bp._alternate_list(base)[1]}
                ow,oh,oldimg,_ = bp.image_location(base)
                l,t,r,b = bounds(base,old_alt[91],ow,oh)
                self.assertEqual(bounds(made,offsets[91],w,h),[l,t,r,b])
                for x in range(l,r):
                    for y in range(t,b):
                        self.assertEqual(pixel(made,img,w,x,y),pixel(base,oldimg,ow,x,y))

    def test_xbox_controls_leaves_other_files_alone(self):
        self.assertIsNone(bp.xbox_controls(b'not an igz'))

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'needs the game (BB_TEST_GAME)')
    def test_every_options_script_goes_to_the_xbox_diagram(self):
        """Issue #256: the Options page's script skips the controller type check for its XB1
        output (the one assigning AnimationTag_XB1_ControlScheme); nothing else changes"""
        from archive_reader import Archive
        from igz_file import Igz
        seen = 0
        for name in bp.CONTROLS_ARCHIVES:
            path = Path(os.environ['BB_TEST_GAME']) / 'archives' / name
            if not path.is_file():
                continue
            files = {f['path'].replace('\\', '/').lower(): f for f in Archive(path).files}
            blob = bytes(Archive(path).extract(files[bp.CONTROLS_SCRIPT.lower()]))
            igz = Igz(blob)
            check, xb1_ref, before = bp._controls_check(igz)
            made = bp.xbox_controls(blob)
            self.assertIsNotNone(made, name)
            changed = [i for i in range(len(blob)) if blob[i] != made[i]]
            data = igz.sections[1][1]
            self.assertTrue(changed and all(data + before[0] + 0x28 <= i < data + before[0] + 0x30 for i in changed))
            after = Igz(made)
            self.assertEqual(after.u64(before[0] + 0x28), igz.u64(xb1_ref))       # straight to XB1
            self.assertEqual(igz.u64(before[0] + 0x28) & 0x7FFFFFF, check)        # was the check
            self.assertIsNone(bp._controls_check(after))                         # nothing leads to it now
            seen += 1
        self.assertTrue(seen)

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
