#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Copas Adicionales' pictures, drawn from the player's own game: the built-in extra-cups-art mod.

Ultimate Customizer's 25 Copas Adicionales (scripts/builtin_mods/ultimate-customizer, option extra-cups) each
have a picture in the cup menu: the game's 2x2 collage layout (the Wumpa Cup picture's, its gaps
found by their colour) filled with the race select pictures of the cup's four tracks. The pictures
are not in the repository (they are made of the game's images); "Prepare" in the launcher runs
this, which draws them from the player's game files and writes the extra-cups-art package into
mod-state/builtin (scripts/mod_library.py lists it as a built-in mod):

    python scripts/cup_art.py --prepare PORTABLE_ROOT --game GAME_FOLDER

The pictures are stored as the game stores its UI pictures (BC3, PS4 1D-thin tiled 4x4 blocks,
bottom row first), as more named images in the Space Cup picture's file (update2.pak), the file
ultimate-customizer's CupSelect_materials names them in (cup_<id>).

Algorithm and tables by Kyouko8 (Ultimate Customizer's generator: cupart.py, texlib.py, texfile.py,
trackart.py), brought here unchanged so the pictures come out as the author's.
"""
import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

ID = 'extra-cups-art'
UC = 'battle-point-limit-selector'
VERSION = '5.1.0'
HOST = 'GuiStandard_diffuse,textures@!ui!Octane!RaceSelect!octane_ui_raceselect_bg_spacecup`tga,101'
WUMPA = 'GuiStandard_diffuse,textures@!ui!Octane!RaceSelect!octane_ui_raceselect_bg_wumpacup`tga,101'
LEVEL_MATERIALS = 'materialinstances/UI/LevelSelect_materials.igz'
ORDER = ('update2.pak', 'update.pak', 'juicedomain_startscreen.pak')  # newest copy first
GAP_RGB = (231, 247, 255)
IMG_SIZE = 0x58
ALIGN = 0x8000

# Each cup (its id, the four race select pictures, LevelImage_<name>, of its tracks).
CUPS = [
    ('cocoCup', ['CocoPark', 'OutOfTime', 'PrehistoricPlayground', 'Clockwork_Wumpa']),
    ('spyroCup', ['TinyTemple', 'SpyroCircuit', 'BarinRuins', 'Twilighttour']),
    ('koalaCup', ['KongCircus', 'SewerSpeedway', 'MeteorGorge', 'OxideStation']),
    ('rooCup', ['DeepSeaDriving', 'RoosTube', 'InfernoIsland', 'CrashCove']),
    ('tinyCup', ['TinyArena', 'PolarPass', 'OutOfTime', 'ElectronAvenue']),
    ('polarCup', ['BlizzardBluff', 'PolarPass', 'TigerTemple', 'Android_Alley']),
    ('dingodileCup', ['DragonMines', 'DingoCanyon', 'Toxic', 'JungleBoogie']),
    ('pinstripeCup', ['AssemblyLane', 'TurboTrack', 'CrashCove', 'JungleBoogie']),
    ('cortexCup', ['CortexCastle', 'DryLand', 'TinyArena', 'PrehistoricPlayground']),
    ('nTropyCup', ['ThunderStruck', 'Clockwork_Wumpa', 'Toxic', 'ElectronAvenue']),
    ('oxideCup', ['OxideStation', 'ninasnightmare', 'CocoPark', 'PolarPass']),
    ('nginCup', ['HotAirSkyway', 'NGinLab', 'SewerSpeedway', 'MeteorGorge']),
    ('pentaCup', ['Winter', 'BlizzardBluff', 'HotAirSkyway', 'OxideStation']),
    ('crunchCup', ['Twilighttour', 'TinyTemple', 'OutOfTime', 'DingoCanyon']),
    ('nashCup', ['SewerSpeedway', 'DeepSeaDriving', 'BarinRuins', 'Twilighttour']),
    ('babyTCup', ['PrehistoricPlayground', 'Winter', 'SonyStadium', 'MysteryCaves']),
    ('papuCup', ['PapusPyramid', 'TurboTrack', 'ThunderStruck', 'DryLand']),
    ('krunkCup', ['BarinRuins', 'CortexCastle', 'AssemblyLane', 'KongCircus']),
    ('komodoCup', ['MeteorGorge', 'DragonMines', 'SlideColiseum', 'JungleBoogie']),
    ('fakeCrashCup', ['SonyStadium', 'CrashCove', 'Android_Alley', 'Clockwork_Wumpa']),
    ('ninaCup', ['ninasnightmare', 'PapusPyramid', 'SpyroCircuit', 'TigerTemple']),
    ('nBrioCup', ['NGinLab', 'MysteryCaves', 'HyperSpaceway', 'ElectronAvenue']),
    ('nTranceCup', ['HyperSpaceway', 'RoosTube', 'InfernoIsland', 'Toxic']),
    ('gnastyCup', ['SlideColiseum', 'InfernoIsland', 'DryLand', 'TigerTemple']),
    ('tawnaCup', ['ElectronAvenue', 'PapusPyramid', 'AssemblyLane', 'JungleBoogie']),
]


# --- the game's files ---------------------------------------------------------------------------

class Game:
    """The newest copy of a file in the game's archives (ORDER), and where it is."""

    def __init__(self, folder):
        from graphics_tab import Archive
        self.archives = {}
        for name in ORDER:
            path = Path(folder) / 'archives' / name
            if not path.is_file():
                raise ValueError(f'the game has no archives/{name}')
            a = Archive(path)
            self.archives[name] = (a, {a.logical(i).casefold(): i for i in range(len(a.names))})

    def find(self, logical):
        """(archive name, index, bytes)."""
        for name in ORDER:
            a, index = self.archives[name]
            i = index.get(logical.casefold())
            if i is not None:
                return name, i, a.read(i)
        raise ValueError(f'the game has no {logical}')

    def close(self):
        for a, _ in self.archives.values():
            a.f.close()


# --- textures (texlib.py) ----------------------------------------------------------------------

def sections(blob):
    out, at = [], 0x14
    while at + 16 <= 0x800:
        s = struct.unpack_from('<4I', blob, at)
        if s[1] == 0 and s[2] == 0:
            break
        out.append(list(s))
        at += 16
    return out


def fixups(blob):
    secs = sections(blob)
    n = struct.unpack_from('<I', blob, 0x10)[0]
    p, out = secs[0][1], []
    for _ in range(n):
        tag, c, l, st = struct.unpack_from('<4sIII', blob, p)
        out.append([tag.decode(), c, st, bytes(blob[p + st:p + l]), l])
        p += l
    return out


def image_info(blob):
    """(offset, width, height, data bytes) of the file's single igImage2."""
    secs = sections(blob)
    obj = blob[secs[1][1]:secs[1][1] + secs[1][2]]
    for at in range(0, len(obj) - 72, 8):
        size, = struct.unpack_from('<I', obj, at + 56)
        ptr, = struct.unpack_from('<Q', obj, at + 64)
        if ptr == 0x08000000 and 0 < size <= secs[2][2]:
            w, h = struct.unpack_from('<HH', obj, at + 24)
            return at, w, h, bytes(blob[secs[2][1]:secs[2][1] + size])
    raise ValueError('no igImage2')


def tiled_shape(w, h):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    return (bw + 7) // 8 * 8, (bh + 7) // 8 * 8


def _order(w, h):
    """For each block (by, bx) of the padded grid, its index in the tiled data."""
    import numpy as np
    pw, ph = tiled_shape(w, h)
    y, x = np.mgrid[0:ph, 0:pw]
    m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)
    return ((y >> 3) * (pw >> 3) + (x >> 3)) * 64 + m


def untile_blocks(data, w, h):
    import numpy as np
    pw, ph = tiled_shape(w, h)
    blocks = np.frombuffer(data, np.uint8)[:pw * ph * 16].reshape(-1, 16)
    return blocks[_order(w, h)]


def tile_blocks(grid, w, h):
    import numpy as np
    pw, ph = tiled_shape(w, h)
    out = np.zeros((pw * ph, 16), np.uint8)
    out[_order(w, h).reshape(-1)] = grid.reshape(-1, 16)
    return out.tobytes()


def _565(c):
    import numpy as np
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    b = c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.int32)


def decode_bc3(grid):
    """RGB (H, W, 3) of a (bh, bw, 16) BC3 block grid (alpha ignored: the UI pictures are opaque)."""
    import numpy as np
    bh, bw = grid.shape[:2]
    g = grid.astype(np.uint32)
    c0 = g[..., 8] | (g[..., 9] << 8)
    c1 = g[..., 10] | (g[..., 11] << 8)
    idx = g[..., 12] | (g[..., 13] << 8) | (g[..., 14] << 16) | (g[..., 15] << 24)
    p0, p1 = _565(c0), _565(c1)
    pal = np.stack([p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3], -2)
    sel = np.stack([(idx >> (2 * i)) & 3 for i in range(16)], -1)
    px = np.take_along_axis(pal, sel[..., None].astype(np.int64).repeat(3, -1), -2)
    px = px.reshape(bh, bw, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 3)
    return px.astype(np.uint8)


def encode_bc3(rgb):
    """(bh, bw, 16) BC3 blocks of an RGB image (H, W multiples of 4), opaque. Endpoints: the
    extremes along each block's principal axis, refined once by least squares."""
    import numpy as np
    H, W, _ = rgb.shape
    bh, bw = H // 4, W // 4
    px = rgb.reshape(bh, 4, bw, 4, 3).transpose(0, 2, 1, 3, 4).reshape(bh, bw, 16, 3).astype(np.float32)
    mean = px.mean(2, keepdims=True)
    d = px - mean
    cov = np.einsum('abki,abkj->abij', d, d)
    v = np.ones((bh, bw, 3), np.float32)
    for _ in range(8):
        v = np.einsum('abij,abj->abi', cov, v)
        v /= np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-6)
    t = np.einsum('abki,abi->abk', d, v)
    lo, hi = t.min(2), t.max(2)
    e0 = mean[:, :, 0] + v * hi[..., None]
    e1 = mean[:, :, 0] + v * lo[..., None]

    def quant(e):
        e = np.clip(np.rint(e), 0, 255).astype(np.int32)
        return ((e[..., 0] * 31 + 127) // 255 << 11) | ((e[..., 1] * 63 + 127) // 255 << 5) | \
            ((e[..., 2] * 31 + 127) // 255)

    for it in range(2):
        q0, q1 = quant(e0), quant(e1)
        swap = q0 < q1  # the 4-colour mode needs c0 > c1
        q0, q1 = np.where(swap, q1, q0), np.where(swap, q0, q1)
        same = q0 == q1
        q1 = np.where(same & (q0 > 0), q0 - 1, q1)
        q0 = np.where(same & (q0 == 0), 1, q0)
        p0, p1 = _565(q0).astype(np.float32), _565(q1).astype(np.float32)
        pal = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], -2)
        dist = ((px[:, :, :, None, :] - pal[:, :, None, :, :]) ** 2).sum(-1)
        sel = dist.argmin(-1)
        if it == 1:
            break
        wgt = np.array([1, 0, 2 / 3, 1 / 3], np.float32)[sel]
        a, b = wgt, 1 - wgt
        aa, bb, ab = (a * a).sum(2), (b * b).sum(2), (a * b).sum(2)
        ax = np.einsum('abk,abki->abi', a, px)
        bx = np.einsum('abk,abki->abi', b, px)
        det = aa * bb - ab * ab
        ok = np.abs(det) > 1e-6
        n0 = (ax * bb[..., None] - bx * ab[..., None]) / np.where(ok, det, 1)[..., None]
        n1 = (bx * aa[..., None] - ax * ab[..., None]) / np.where(ok, det, 1)[..., None]
        e0 = np.where(ok[..., None], n0, p0)
        e1 = np.where(ok[..., None], n1, p1)
    idx = np.zeros((bh, bw), np.uint32)
    for i in range(16):
        idx |= sel[..., i].astype(np.uint32) << (2 * i)
    out = np.zeros((bh, bw, 16), np.uint8)
    out[..., 0] = 0xff
    out[..., 1] = 0xff
    out[..., 8] = q0 & 255
    out[..., 9] = q0 >> 8
    out[..., 10] = q1 & 255
    out[..., 11] = q1 >> 8
    for i in range(4):
        out[..., 12 + i] = (idx >> (8 * i)) & 255
    return out


def resize(rgb, w, h):
    """Area-averaging resample to (h, w)."""
    import numpy as np
    H, W, _ = rgb.shape
    src = rgb.astype(np.float32)
    ys = np.linspace(0, H, h + 1)
    xs = np.linspace(0, W, w + 1)
    ii = np.zeros((H + 1, W + 1, 3), np.float64)
    ii[1:, 1:] = src.cumsum(0).cumsum(1)

    def S(y, x):
        y0 = np.clip(np.floor(y).astype(int), 0, H - 1)
        x0 = np.clip(np.floor(x).astype(int), 0, W - 1)
        fy = (y - y0)[:, None, None]
        fx = (x - x0)[None, :, None]
        a = ii[y0][:, x0]
        b = ii[y0][:, x0 + 1]
        c = ii[y0 + 1][:, x0]
        d = ii[y0 + 1][:, x0 + 1]
        return a * (1 - fy) * (1 - fx) + b * (1 - fy) * fx + c * fy * (1 - fx) + d * fy * fx
    tot = S(ys[1:], xs[1:]) - S(ys[:-1], xs[1:]) - S(ys[1:], xs[:-1]) + S(ys[:-1], xs[:-1])
    area = np.outer(np.diff(ys), np.diff(xs))[..., None]
    return np.clip(np.rint(tot / area), 0, 255).astype(np.uint8)


def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b''.join(b'\0' + rgb[y].tobytes() for y in range(h))

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


# --- the collage (cupart.py) -------------------------------------------------------------------

def display(blob):
    """RGB (H, W, 3) of a UI texture file, top row first (the files store it bottom-up)."""
    _, w, h, data = image_info(blob)
    return decode_bc3(untile_blocks(data, w, h))[:h, :w][::-1]


def _fit(points):
    """Least-squares line a*t + b through (t, v) points, refitted without the outliers."""
    import numpy as np
    t, v = np.array(points, float).T
    for _ in range(3):
        a, b = np.polyfit(t, v, 1)
        r = np.abs(v - (a * t + b))
        keep = r <= max(3.0, 2.5 * np.median(r))
        t, v = t[keep], v[keep]
    return np.polyfit(t, v, 1)


def _runs(line, lo, hi):
    """(start, end) of the longest run of gap pixels in line[lo:hi]."""
    import numpy as np
    m = np.all(np.abs(line[lo:hi].astype(int) - np.array(GAP_RGB)) <= 4, -1)
    best, cur, start = None, 0, 0
    for i, g in enumerate(m):
        if g:
            if cur == 0:
                start = i
            cur += 1
            if best is None or cur > best[1] - best[0]:
                best = (start + lo, start + lo + cur)
        else:
            cur = 0
    return best if best and best[1] - best[0] >= 6 else None


def layout(template):
    """Panel masks (TL, TR, BL, BR) and the gap mask of the template's layout."""
    import numpy as np
    H, W, _ = template.shape
    edges = {}
    for name, rows, axis in (('up', range(10, 420, 4), 0), ('down', range(620, H - 10, 4), 0),
                             ('left', range(10, 480, 4), 1), ('right', range(680, W - 10, 4), 1)):
        a, b = [], []
        for k in rows:
            line = template[k] if axis == 0 else template[:, k]
            r = _runs(line, 380, 760) if axis == 0 else _runs(line, 380, 680)
            if r:
                a.append((k, r[0]))
                b.append((k, r[1]))
        edges[name] = (_fit(a), _fit(b))
    y, x = np.mgrid[0:H, 0:W].astype(float)

    def at(f, t):
        return f[0] * t + f[1]
    mid_y = (at(edges['left'][0], W / 2) + at(edges['right'][1], W / 2)) / 2
    vlo = np.where(y < mid_y, at(edges['up'][0], y), at(edges['down'][0], y))
    vhi = np.where(y < mid_y, at(edges['up'][1], y), at(edges['down'][1], y))
    mid_x = (at(edges['up'][0], mid_y) + at(edges['down'][1], mid_y)) / 2
    hlo = np.where(x < mid_x, at(edges['left'][0], x), at(edges['right'][0], x))
    hhi = np.where(x < mid_x, at(edges['left'][1], x), at(edges['right'][1], x))
    left, right = x < vlo, x >= vhi
    top, bottom = y < hlo, y >= hhi
    panels = [left & top, right & top, left & bottom, right & bottom]
    gap = ~(panels[0] | panels[1] | panels[2] | panels[3])
    return panels, gap


def compose(panels, gap, images):
    """The collage of four RGB images (each scaled to cover its panel, centred)."""
    import numpy as np
    H, W = gap.shape
    out = np.zeros((H, W, 3), np.uint8)
    out[gap] = GAP_RGB
    for mask, img in zip(panels, images):
        ys, xs = np.nonzero(mask)
        y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
        bh, bw = y1 - y0, x1 - x0
        ih, iw, _ = img.shape
        s = max(bw / iw, bh / ih)
        sw, sh = max(bw, round(iw * s)), max(bh, round(ih * s))
        scaled = resize(img, sw, sh)
        oy, ox = (sh - bh) // 2, (sw - bw) // 2
        tile = scaled[oy:oy + bh, ox:ox + bw]
        sub = mask[y0:y1, x0:x1]
        region = out[y0:y1, x0:x1]
        region[sub] = tile[sub]
    return out


# --- the track pictures (trackart.py) ----------------------------------------------------------

def texture_of(materials, name):
    """The texture a picture material names (material +0x58 object set, its first item an
    igGraphicsTexture with the image handle at +0x28)."""
    m = dict(materials.roots()[2])['LevelImage_' + name]
    tex = materials.ref(materials.ref(materials.ref(m + 0x58) + 0x20))
    if materials.typed[tex] != 'igGraphicsTexture':
        raise ValueError(f'LevelImage_{name}: no texture')
    image, space = materials.handle(materials.u64(tex + 0x28))
    if image != 'image':
        raise ValueError(f'LevelImage_{name}: unexpected texture handle')
    return space


# --- the texture file (texfile.py) -------------------------------------------------------------

def encode(rgb):
    """Tiled BC3 bytes of a display-oriented RGB image the size of the UI pictures."""
    import numpy as np
    h, w, _ = rgb.shape
    pw, ph = tiled_shape(w, h)
    pad = np.pad(rgb[::-1], ((0, ph * 4 - h), (0, pw * 4 - w), (0, 0)), mode='edge')
    return tile_blocks(encode_bc3(pad), w, h)


def build(blob, extra):
    """The texture file `blob` with the images extra = [(name, source path, tiled bytes)] after
    its own, each like its own: same size, format and fields."""
    from igz_file import pack_offsets, read_strings, unpack_offsets, write_strings
    from graphics_tab import fnv1a
    secs = sections(blob)
    fx = {f[0]: f for f in fixups(blob)}
    order = [f[0] for f in fixups(blob)]
    if order != ['TSTR', 'TMET', 'MTSZ', 'EXID', 'RVTB', 'RSTT', 'ROFS', 'REXT', 'ROOT', 'ONAM']:
        raise ValueError('the Space Cup picture file has an unexpected layout')
    obj = blob[secs[1][1]:secs[1][1] + secs[1][2]]
    if unpack_offsets(fx['RVTB'][3], fx['RVTB'][1]) != [0, 0x30, 0x88] or len(obj) != 0xc0:
        raise ValueError('the Space Cup picture file has an unexpected layout')
    strings = read_strings(fx['TSTR'][3], fx['TSTR'][1])
    image0 = bytes(obj[0x30:0x30 + IMG_SIZE])
    size0 = struct.unpack_from('<I', image0, 0x38)[0]
    pixels0 = blob[secs[2][1]:secs[2][1] + size0]
    images = [(0, pixels0, 1)]  # (string of its path, pixels, string of its name)
    for name, path, data in extra:
        if len(data) != size0:
            raise ValueError(f'{name}: {len(data)} bytes, the pictures have {size0}')
        strings += [path, name]
        images.append((len(strings) - 2, data, len(strings) - 1))
    n = len(images)
    out = bytearray()
    rvtb, rstt, rofs, rext = [], [], [], []
    lst = bytearray(obj[0:0x28])
    struct.pack_into('<II', lst, 0xc, n, n)
    struct.pack_into('<Q', lst, 0x18, (struct.unpack_from('<Q', obj, 0x18)[0] & ~0xffffffff) | 8 * n)
    struct.pack_into('<Q', lst, 0x20, 0x28)
    out += lst
    rvtb.append(0)
    rofs.append(0x20)
    at_images = 0x28 + 8 * n
    at_images += -at_images % 16
    for i in range(n):
        out += struct.pack('<Q', 0)  # filled below
        rofs.append(0x28 + 8 * i)
    out += bytes(at_images - len(out))
    data_at, pix = [], bytearray()
    for _, data, _ in images:
        pix += bytes(-len(pix) % ALIGN)
        data_at.append(len(pix))
        pix += data
    starts = []
    for i, (path, _, _) in enumerate(images):
        o = len(out)
        o += -o % 16
        out += bytes(o - len(out))
        starts.append(o)
        im = bytearray(image0)
        struct.pack_into('<Q', im, 0x10, path)
        struct.pack_into('<Q', im, 0x40, 0x08000000 | data_at[i])
        out += im
        rvtb.append(o)
        rstt.append(o + 0x10)
        rext.append(o + 0x28)
        rofs.append(o + 0x40)
    for i, o in enumerate(starts):
        struct.pack_into('<Q', out, 0x28 + 8 * i, o)
    o = len(out)
    o += -o % 8
    out += bytes(o - len(out))
    nl = bytearray(obj[0x88:0x88 + 0x28])
    struct.pack_into('<II', nl, 0xc, n, n)
    struct.pack_into('<Q', nl, 0x18, (struct.unpack_from('<Q', obj, 0xa0)[0] & ~0xffffffff) | 16 * n)
    struct.pack_into('<Q', nl, 0x20, o + 0x28)
    out += nl
    rvtb.append(o)
    rofs.append(o + 0x20)
    names_at = o + 0x28
    onam = o
    for i, (_, _, name) in enumerate(images):
        out += struct.pack('<QII', name, fnv1a(strings[name]), 0)
        rstt.append(names_at + 16 * i)
    blocks = {'TSTR': (len(strings), write_strings(strings)), 'RVTB': (len(rvtb), pack_offsets(rvtb)),
              'RSTT': (len(rstt), pack_offsets(sorted(rstt))), 'ROFS': (len(rofs), pack_offsets(sorted(rofs))),
              'REXT': (len(rext), pack_offsets(rext)), 'ROOT': (1, struct.pack('<I', 0)),
              'ONAM': (1, struct.pack('<I', onam))}
    fixup_bytes = bytearray()
    for tag in order:
        f = fx[tag]
        count, raw = blocks.get(tag, (f[1], f[3]))
        if tag in blocks:
            raw = raw + bytes(-(f[2] + len(raw)) % 4)
        fixup_bytes += struct.pack('<4sIII', tag.encode(), count, f[2] + len(raw), f[2]) + bytes(f[2] - 16) + raw
    head = bytearray(blob[:0x800])
    obj_at = 0x800 + len(fixup_bytes)
    obj_at += -obj_at % 8
    fixup_bytes += bytes(obj_at - 0x800 - len(fixup_bytes))
    pix_at = obj_at + len(out)
    struct.pack_into('<4I', head, 0x14, secs[0][0], 0x800, len(fixup_bytes), secs[0][3])
    struct.pack_into('<4I', head, 0x24, secs[1][0], obj_at, len(out), secs[1][3])
    struct.pack_into('<4I', head, 0x34, secs[2][0], pix_at, len(pix), secs[2][3])
    return bytes(head + fixup_bytes + out + pix)


# --- the package -------------------------------------------------------------------------------

def draw(game_folder, previews=None, log=print):
    """(operation, texture file bytes): every cup's picture added to the Space Cup's file."""
    from igz_merge import File
    game = Game(game_folder)
    try:
        archive, index, host = game.find('textures/' + HOST + '.igz')
        template = display(game.find('textures/' + WUMPA + '.igz')[2])
        levels = File(game.find(LEVEL_MATERIALS)[2])
        panels, gap = layout(template)
        pictures, extra = {}, []
        for n, (ident, names) in enumerate(CUPS, 1):
            log(f'Copas Adicionales: {n}/{len(CUPS)} {ident}')
            for name in names:
                if name not in pictures:
                    pictures[name] = display(game.find('textures/' + texture_of(levels, name) + '.igz')[2])
            art = compose(panels, gap, [pictures[name] for name in names])
            if previews:
                Path(previews).mkdir(parents=True, exist_ok=True)
                write_png(Path(previews) / (ident + '.png'), resize(art, 644, 540))
            extra.append(('cup_' + ident, 'data/Octane/textures/ui/Octane/CupSelect/octane_ui_cupselect_%s.tga'
                          % ident.lower(), encode(art)))
        out = build(host, extra)
    finally:
        game.close()
    op = {'kind': 'archive_asset', 'archive': archive, 'asset': 'textures/' + HOST + '.igz', 'index': index,
          'original_sha256': hashlib.sha256(host).hexdigest(), 'patched_sha256': hashlib.sha256(out).hexdigest(),
          'payload': 'payloads/cupselect-art.igz'}
    return op, out


def package(folder, op, data):
    """The extra-cups-art format 2 package, written whole into `folder` (replaced)."""
    import shutil
    import mod_format
    folder = Path(folder)
    staging = folder.with_name(folder.name + '.building')
    if staging.exists():
        shutil.rmtree(staging)
    (staging / 'payloads').mkdir(parents=True)
    files = {'octr-mod.required': b'OCTR Mod Package 2\n', op['payload']: data}
    for path, blob in files.items():
        (staging / path).write_bytes(blob)
    manifest = {
        'schemaVersion': 2, 'id': ID, 'name': 'Copas Adicionales pictures (Ultimate Customizer)', 'version': VERSION,
        'author': 'Kyouko8',
        'description': "The pictures of the 25 Copas Adicionales: collages of each cup's four tracks, drawn from your "
                       'game files by "Prepare". Used with Ultimate Customizer\'s Copas Adicionales.',
        'requiresCapabilities': ['archive-assets'], 'compatibility': {'online': 'allowed'},
        'operations': [op],
        'files': [{'file': p, 'sha256': hashlib.sha256(b).hexdigest(), 'bytes': len(b)} for p, b in sorted(files.items())]}
    (staging / 'mod.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    mod_format.validate_folder(staging)
    if folder.exists():
        shutil.rmtree(folder)
    staging.rename(folder)
    return folder


def target(port_root):
    """Where "Prepare" puts the package: mod-state/builtin/extra-cups-art beside the mods folder."""
    import mod_library
    return mod_library.generated_builtin(Path(port_root) / 'mods') / ID


def prepare(port_root, game_folder, log=print):
    try:
        import numpy  # noqa: F401
    except ImportError:
        raise ValueError('Preparing the Copas Adicionales pictures needs numpy in the launcher\'s Python') from None
    op, data = draw(game_folder, log=log)
    folder = package(target(port_root), op, data)
    log(f'Copas Adicionales pictures: ready ({len(data) // (1024 * 1024)} MB, {len(CUPS)} pictures)')
    return folder


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--prepare', type=Path, metavar='PORTABLE_ROOT', help='write the package')
    parser.add_argument('--game', type=Path, required=True, help='the game folder (CUSA13795 1.21)')
    parser.add_argument('--previews', type=Path, help='also write a PNG of each picture here')
    parser.add_argument('--out', type=Path, help='write only the texture file here')
    args = parser.parse_args(argv)
    try:
        if args.prepare:
            prepare(args.prepare, args.game, log=lambda m: print(m, flush=True))
        else:
            op, data = draw(args.game, args.previews, log=lambda m: print(m, flush=True))
            if args.out:
                args.out.write_bytes(data)
            print(json.dumps(op, indent=2))
    except (ValueError, OSError) as error:
        print(f'Copas Adicionales pictures: {error}', flush=True)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
