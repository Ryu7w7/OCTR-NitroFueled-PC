"""Dumps the button glyph part of an Alchemy bitmap font (igBitmapFont): the "button" igImage2
as PNG and the alternate character metrics (char code -> UV rectangle in that image) as JSON.

Works for the PS4 fonts of CTR (1D-thin tiled RGBA8) and the PC fonts of the Crash N. Sane
Trilogy (linear RGBA8).

usage: python font_glyphs.py <font.igz> <out prefix> [tiled]
"""
import json
import struct
import sys
import zlib

sys.path.insert(0, __import__("os").path.dirname(__file__))
from igz import Igz  # noqa: E402

IMG_W, IMG_H, IMG_DATA = 24, 26, 56  # igImage2 field offsets
CM_CHAR, CM_UVMIN, CM_UVMAX, CM_SIZE = 12, 52, 60, 36  # igCharMetrics


def morton(x, y):
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)


def button_image(igz_bytes, tiled=False):
    """(width, height, RGBA bytes) of the font's "button" image."""
    z = Igz(igz_bytes)
    names = {}
    for o, t in z.objects():
        if t == "igImage2":
            name_ptr = z.u64(o + 16)
            names[o] = z.strings[name_ptr] if name_ptr < len(z.strings) else "?"
    btn = next(o for o, n in names.items() if n == "button")
    fo = z.file_offset(btn)
    w, h = struct.unpack_from("<HH", z.b, fo + IMG_W)
    _size, dptr = struct.unpack_from("<QQ", z.b, fo + IMG_DATA)
    # tiled images are stored with their size padded to whole 8x8 micro tiles
    nbytes = ((w + 7) // 8 * 8) * ((h + 7) // 8 * 8) * 4 if tiled else w * h * 4
    data = z.b[z.file_offset(dptr):z.file_offset(dptr) + nbytes]
    if tiled:
        pitch = (w + 7) // 8 * 8
        lin = bytearray(w * h * 4)
        for y in range(h):
            for x in range(w):
                o = (((y >> 3) * (pitch >> 3) + (x >> 3)) * 64 + morton(x & 7, y & 7)) * 4
                lin[(y * w + x) * 4:(y * w + x) * 4 + 4] = data[o:o + 4]
        data = bytes(lin)
    return w, h, bytes(data)


def char_metrics(igz_bytes):
    z = Igz(igz_bytes)
    glyphs = []
    for o, t in z.objects():
        if t != "igCharMetrics":
            continue
        f = z.file_offset(o)
        ch = struct.unpack_from("<H", z.b, f + CM_CHAR)[0]
        u0, v0 = struct.unpack_from("<2f", z.b, f + CM_UVMIN)
        u1, v1 = struct.unpack_from("<2f", z.b, f + CM_UVMAX)
        glyphs.append({"char": ch, "uv": [u0, v0, u1, v1]})
    return glyphs


def write_png(path, w, h, rgba):
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main(path, prefix, tiled=None):
    b = open(path, "rb").read()
    w, h, data = button_image(b, bool(tiled))
    write_png(prefix + ".png", w, h, data)
    glyphs = char_metrics(b)
    json.dump({"image": [w, h], "glyphs": glyphs}, open(prefix + ".json", "w"), indent=1)
    print(f"button image {w}x{h}, {len(glyphs)} char metrics -> {prefix}.png/.json")


if __name__ == "__main__":
    main(*sys.argv[1:4])
