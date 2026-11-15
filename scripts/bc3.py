"""BC3 (DXT5) texture encoding in plain Python, for the art the port puts into the game's UI
(scripts/ctr_tv_logo.py): preparing it needs no ImageMagick, which players have none of.
SPDX-License-Identifier: GPL-2.0-or-later
"""
import struct


def _rgb565(r, g, b):
    return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | (b * 31 + 127) // 255


def _rgb888(c):
    r, g, b = c >> 11, (c >> 5) & 63, c & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def bc3_encode(width, height, rgba, flip=False):
    """RGBA8 (rows top first; flip=True: bottom first, as the game samples) -> BC3/DXT5 blocks, in
    the standard linear order. In Python, so preparing the online menus needs no ImageMagick
    (players have none): a bounding-box fit per 4x4 block, plenty for these flat UI patterns."""
    if width % 4 or height % 4:
        raise ValueError('BC3 needs sizes that are multiples of 4')
    row = width * 4
    out = bytearray()
    for by in range(0, height, 4):
        for bx in range(0, width, 4):
            px = []
            for y in range(4):
                sy = height - 1 - (by + y) if flip else by + y
                at = sy * row + bx * 4
                px.extend(tuple(rgba[at + x * 4:at + x * 4 + 4]) for x in range(4))
            # alpha: two endpoints and six steps between (a0 > a1)
            alphas = [p[3] for p in px]
            a0, a1 = max(alphas), min(alphas)
            if a0 == a1:
                out += bytes((a0, a1)) + bytes(6)
            else:
                levels = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                bits = 0
                for i, a in enumerate(alphas):
                    code = min(range(8), key=lambda k: abs(levels[k] - a))
                    bits |= code << (3 * i)
                out += bytes((a0, a1)) + bits.to_bytes(6, 'little')
            # colour: the box's corners, inset a little, as two RGB565 ends (c0 > c1: four colours)
            lo = [min(p[c] for p in px) for c in range(3)]
            hi = [max(p[c] for p in px) for c in range(3)]
            inset = [(h - l) // 16 for l, h in zip(lo, hi)]
            c0 = _rgb565(*(min(255, h - i) for h, i in zip(hi, inset)))
            c1 = _rgb565(*(max(0, l + i) for l, i in zip(lo, inset)))
            if c0 < c1:
                c0, c1 = c1, c0
            if c0 == c1:
                out += struct.pack('<HHI', c0, c1, 0)
                continue
            e0, e1 = _rgb888(c0), _rgb888(c1)
            palette = [e0, e1, tuple((2 * a + b) // 3 for a, b in zip(e0, e1)),
                       tuple((a + 2 * b) // 3 for a, b in zip(e0, e1))]
            bits = 0
            for i, p in enumerate(px):
                code = min(range(4), key=lambda k: sum((palette[k][c] - p[c]) ** 2 for c in range(3)))
                bits |= code << (2 * i)
            out += struct.pack('<HHI', c0, c1, bits)
    return bytes(out)
