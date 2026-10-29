"""Builds the Xbox button glyph atlas for CrashTeamRacingNitroFueled.exe: CTR's button atlas (the
"button" image of fonts/GeekSpeak.igz, 512x263, cells referenced by the font's alternate char
metrics) with the PlayStation buttons replaced by the Xbox ones of the Crash Bandicoot N. Sane
Trilogy PC font. Output: raw texels in the memory order of the original (B, G, R, A: the game's
T# swizzles them back), premultiplied, embedded as RCDATA; plus a PNG preview next to it.

Without the N. Sane Trilogy files the output is empty (the game then keeps the PS4 glyphs).

usage: python make_glyph_atlas.py <CTR game dir> <N. Sane Trilogy dir> <out.rgba>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from font_glyphs import button_image, write_png  # noqa: E402
from iga import Iga  # noqa: E402

# CTR atlas cells and the N. Sane glyph boxes (alpha bounding boxes in its 256x587 image)
CTR_CELLS = {
    "L1": (0, 0, 146, 64), "R1": (147, 0, 293, 64), "Cross": (294, 0, 378, 84),
    "Circle": (379, 0, 471, 84), "Triangle": (0, 85, 92, 169), "Square": (93, 85, 185, 169),
    "LStick": (271, 85, 355, 169), "RStick": (356, 85, 440, 169), "L2": (170, 170, 270, 262),
    "R2": (271, 170, 371, 262),
}
NSANE = {
    "LB": (1, 1, 122, 47), "RB": (125, 1, 246, 47), "A": (1, 268, 77, 343), "B": (80, 268, 157, 344),
    "Y": (160, 268, 236, 343), "X": (79, 346, 155, 422), "LS": (151, 192, 225, 265),
    "RS": (83, 117, 157, 189), "LT": (1, 504, 55, 585), "RT": (58, 504, 112, 585),
}
XBOX = {"L1": "LB", "R1": "RB", "Cross": "A", "Circle": "B", "Triangle": "Y", "Square": "X",
        "LStick": "LS", "RStick": "RS", "L2": "LT", "R2": "RT"}
FONT = "fonts/GeekSpeak.igz"


def read_font(pak):
    a = Iga(pak)
    for i, (logical, _phys) in enumerate(a.names):
        if logical.replace("\\", "/").lower().endswith(FONT.lower()):
            return a.read(i)
    raise FileNotFoundError(f"{FONT} not in {pak}")


def swap_rb(px):
    out = bytearray(px)
    out[0::4], out[2::4] = px[2::4], px[0::4]
    return out


def resample(src, sw, box, dw, dh):
    """Bilinear resize of the `box` region of an RGBA image (premultiplied texels)."""
    x0, y0, x1, y1 = box
    bw, bh = x1 - x0, y1 - y0
    out = bytearray(dw * dh * 4)
    for y in range(dh):
        fy = min(max((y + 0.5) * bh / dh - 0.5, 0), bh - 1)
        iy = int(fy)
        ty = fy - iy
        iy1 = min(iy + 1, bh - 1)
        for x in range(dw):
            fx = min(max((x + 0.5) * bw / dw - 0.5, 0), bw - 1)
            ix = int(fx)
            tx = fx - ix
            ix1 = min(ix + 1, bw - 1)
            for c in range(4):
                def px(xx, yy):
                    return src[((y0 + yy) * sw + x0 + xx) * 4 + c]
                v = (px(ix, iy) * (1 - tx) + px(ix1, iy) * tx) * (1 - ty) + \
                    (px(ix, iy1) * (1 - tx) + px(ix1, iy1) * tx) * ty
                out[(y * dw + x) * 4 + c] = int(v + 0.5)
    return out


def main(ctr_dir, nsane_dir, out):
    nsane_pak = os.path.join(nsane_dir, "archives", "essentialui.pak")
    if not os.path.isfile(nsane_pak):
        open(out, "wb").close()
        print(f"no {nsane_pak}: empty Xbox glyph atlas (PS4 glyphs stay)")
        return
    w, h, ctr = button_image(read_font(os.path.join(ctr_dir, "archives", "languagestartup.pak")), tiled=True)
    nw, _nh, src = button_image(read_font(nsane_pak))
    atlas = bytearray(ctr)
    for cell, glyph in XBOX.items():
        x0, y0, x1, y1 = CTR_CELLS[cell]
        cw, ch = x1 - x0, y1 - y0
        gx0, gy0, gx1, gy1 = NSANE[glyph]
        pad = 2
        s = min((cw - 2 * pad) / (gx1 - gx0), (ch - 2 * pad) / (gy1 - gy0))
        dw, dh = max(1, round((gx1 - gx0) * s)), max(1, round((gy1 - gy0) * s))
        g = swap_rb(resample(src, nw, NSANE[glyph], dw, dh))  # N. Sane: RGBA in memory
        ox, oy = x0 + (cw - dw) // 2, y0 + (ch - dh) // 2
        for y in range(ch):
            atlas[((y0 + y) * w + x0) * 4:((y0 + y) * w + x1) * 4] = bytes(cw * 4)
        for y in range(dh):
            atlas[((oy + y) * w + ox) * 4:((oy + y) * w + ox + dw) * 4] = g[y * dw * 4:(y + 1) * dw * 4]
    with open(out, "wb") as f:
        f.write(atlas)
    write_png(out.rsplit(".", 1)[0] + "_preview.png", w, h, swap_rb(atlas))
    print(f"Xbox glyph atlas {w}x{h} -> {out}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
