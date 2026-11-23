"""Fixed-colour nickname glyphs, copied from each game font with its own metrics.
The alternate font sheet supports coloured icons. Only the nickname uses these
glyphs, so suffixes, brackets and voice icons keep their normal appearance.
"""
from pathlib import Path
import re
import struct

PALETTE = Path(__file__).with_name('tacho_pack') / 'name_swatches.inc'
FIRST = 0xe050
ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-'
COLORS = [int(rgb, 16) for rgb in re.findall(r'0x([0-9A-F]{6})\)', PALETTE.read_text('ascii'))]


def align8(value):
    return (value + 7) & ~7


def add_names(g, pointers, images, old_width, old_height, button_offset, button_size):
    """Return the enlarged texture section; all offsets in g are data-relative."""
    objects = g.objects()
    descriptors = {g.string_at(p+16): p for p, kind in objects if kind == 'igImage2'}
    primary, button = descriptors['font'], descriptors['button']
    source_width, source_height = struct.unpack_from('<HH', g.data, primary+24)
    start = g.u64(primary+64) & 0x07ffffff
    if source_width % 8 or old_width % 8 or button_offset+button_size != len(images):
        raise ValueError('Unexpected native font texture layout')
    primary_list, = [p for p, k in objects if k == 'igCharMetricsList' and
                     g.u64(g.u64(p+32)) != pointers[0][1]]
    chars = {}
    for i in range(g.u32(primary_list+16)):
        off = g.u64(g.u64(primary_list+32)+8*i)
        chars[g.u32(off+12) & 65535] = off

    # Copy whole 8x8 Morton-order tiles. This keeps the antialiasing, outline,
    # bearings and advance of the original glyph at its full resolution.
    # Zoinks Outline has a 512-wide primary sheet. Packing the full palette
    # at that width put its button image beyond the 16,384 texel limit.
    # Source UVs and tile rows still use the original primary image stride.
    width = max(2048, source_width, old_width)
    atlas = bytearray()
    def reserve(height):
        size = width * align8(height) * 4
        if len(atlas) < size:
            atlas.extend(bytes(size-len(atlas)))

    reserve(old_height)
    for ty in range(align8(old_height)//8):
        src = button_offset + ty*old_width*32
        dest = ty*width*32
        atlas[dest:dest+old_width*32] = images[src:src+old_width*32]
    rects = [(off, tuple(v*d for v,d in zip(struct.unpack_from('<4f',g.data,off+52),
                                              (old_width,old_height,old_width,old_height))))
             for _,off in pointers]
    x, y, line = 0, align8(old_height), 0
    for index, rgb in enumerate(COLORS):
        # BGRA8: preserve the original ink, shadow and alpha.
        tables = [bytes((n*((rgb>>shift)&255)+127)//255 for n in range(256)) for shift in (0,8,16)]
        for letter, char in enumerate(ALPHABET):
            orig = chars[ord(char)]
            left, top, right, bottom = (round(v*d) for v,d in zip(
                struct.unpack_from('<4f',g.data,orig+52), (source_width,source_height,source_width,source_height)))
            sx, sy = left & ~7, top & ~7
            w, h = align8(right+1)-sx, align8(bottom+1)-sy
            if x+w > width:
                y, x, line = y+line, 0, 0
            reserve(y+h)
            for dy in range(0,h,8):
                pos = start + ((sy+dy)*source_width + sx*8)*4
                tile = images[pos:pos+w*32]
                for channel, lut in enumerate(tables):
                    tile[channel::4] = tile[channel::4].translate(lut)
                dest = ((y+dy)*width + x*8)*4
                atlas[dest:dest+len(tile)] = tile
            code = FIRST + index*len(ALPHABET) + letter
            off = g.append(g.data[orig:orig+72])
            g.add_site('RVTB',off)
            struct.pack_into('<H',g.data,off+12,code)
            rects.append((off,(x+left-sx,y+top-sy,x+right-sx,y+bottom-sy)))
            pointers.append((code,off))
            x, line = x+w, max(line,h)
    height = len(atlas)//(width*4)
    if max(width, height) > 16384:
        raise ValueError(f'Button font atlas exceeds the texture limit: {width}x{height}')
    for off, rect in rects:
        struct.pack_into('<4f',g.data,off+52,*(v/d for v,d in zip(rect,(width,height,width,height))))
    struct.pack_into('<HH',g.data,button+24,width,height)
    g.set_u32(button+56,len(atlas))
    return images[:button_offset]+atlas
