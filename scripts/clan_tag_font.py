"""The native font draws a shaped clan tag from antialiased 2x2 tiles. This small,
fixed alphabet lets the live game show new tags without rebuilding any PAK.
Each mask has zero advance; a transparent glyph advances one column.
"""
import struct

from igz_file import Igz

FIRST = 0xE4E0
BANDS = 20
MASKS = 256
ADVANCE = FIRST + BANDS * MASKS
# Both unused badges have space beside the literal brackets. Each tile has
# a one-texel guard containing its closest edge, so bilinear filtering cannot
# introduce transparent seams where adjacent native quads meet.
def cell(mask):
    x0, y0 = (128, 176) if mask < 128 else (224, 96)
    return x0 + (mask % 8)*4, y0 + ((mask % 128)//8)*4


def paint_tiles(linear, width):
    for mask in range(MASKS):
        x, y = cell(mask)
        for dy in range(-1, 3):
            for dx in range(-1, 3):
                bit = 2*min(1, max(0, dy)) + min(1, max(0, dx))
                alpha = 85*((mask >> (bit*2)) & 3)
                at = ((y + dy)*width + x + dx)*4
                linear[at:at+4] = bytes((255, 255, 255, alpha))



def glyphs():
    for band in range(BANDS):
        for mask in range(1, MASKS):
            yield FIRST + band * MASKS + mask, band, mask
    yield ADVANCE, 0, 0


def add_native(blob):
    from button_prompts import _alternate_list, _sections_and_fixups, image_location, tile, untile

    _, slots = _alternate_list(blob)
    if not {91, 93}.issubset(code for _, _, code in slots):
        return blob
    if any(FIRST <= code <= ADVANCE for _, _, code in slots):
        return blob
    sections, _ = _sections_and_fixups(blob)
    if len(sections) != 3:
        return blob
    (_, _), (base, _), (texture, texture_len) = sections
    width, height, image, length = image_location(blob)
    if (width, height) != (512, 263):
        return blob
    data = bytearray(blob[:texture])
    texture_descriptor = bytes(data[0x34:0x44])
    data[0x34:0x44] = bytes(16)
    g = Igz(data)
    template = next(off for _, off, code in slots if code == 91) - base
    char_list = next(off for off, kind in g.objects() if kind == 'igCharMetricsList'
                     and g.u64(off + 32) == slots[0][0] - base)
    pointers = [(code, offset - base) for _, offset, code in slots]
    pixels = bytearray(blob[texture:texture + texture_len])
    linear = bytearray(untile(blob[image:image + length], width, height))
    paint_tiles(linear, width)
    for code, band, mask in glyphs():
        x, y = cell(mask)
        off = g.append(g.data[template:template+72])
        g.add_site('RVTB', off)
        struct.pack_into('<H', g.data, off + 12, code)
        struct.pack_into('<9f', g.data, off + 16, 0, 0, 0 if mask else 4, 0, 9 + band*4,
                         4, 4, 0, 0)
        struct.pack_into('<4f', g.data, off + 52, x/width, y/height, (x+2)/width, (y+2)/height)
        pointers.append((code, off))
    from name_swatch_font import add_names
    pixels[image-texture:image-texture+length] = tile(linear, width, height, blob[image:image+length])
    pixels = add_names(g, pointers, pixels, width, height, image-texture, length)
    pointers.sort()
    count = len(pointers)
    array = g.append(b''.join(struct.pack('<Q', off) for _, off in pointers))
    g.set_u64(char_list + 32, array)
    for offset in (12, 16):
        g.set_u32(char_list + offset, count)
    g.set_u32(char_list + 24, count * 8)
    for i in range(count):
        g.add_site('ROFS', array + i*8)
    result = bytearray(g.build())
    result[0x34:0x44] = texture_descriptor
    struct.pack_into('<II', result, 0x38, len(result), len(pixels))
    return bytes(result + pixels)
