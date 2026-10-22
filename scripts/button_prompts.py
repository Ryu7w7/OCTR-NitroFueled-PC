#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Button prompts with Xbox buttons (bbport.ini button_prompts=xbox; default playstation).

The game draws every button prompt (Cross, Circle, L1, the sticks...) from one image, "button",
of its fonts fonts/GeekSpeak.igz and fonts/GeekSpeak_Outline.igz: 512x263 RGBA8 in the PS4's
1D-thin tiling (8x8 micro tiles, Morton order inside each), referenced by the fonts' alternate
character metrics. For Xbox prompts the PlayStation buttons in it are replaced by ours
(button_prompts/xbox.png, drawn by tools/make_button_prompts.py) and the fonts go to the game
through archive patches (archive_patch.py), the archives themselves unchanged.

Reading the image follows FrankHUN88453's CTR-NitroFueled-PC tools (font_glyphs.py, igz.py;
MIT License): the IGZ pointer encoding (data section << 27 | offset) and the tiling.
"""
import argparse
import struct
import sys
import zlib
from pathlib import Path

import archive_patch
from archive_reader import Archive

STYLES = ('playstation', 'xbox')
FONTS = ('fonts/GeekSpeak.igz', 'fonts/GeekSpeak_Outline.igz')
# The archives with those fonts (the update's copies are the ones the 1.21 game reads; the
# others are patched too, so no screen shows a PlayStation button).
ARCHIVES = ('update.pak', 'languagestartup.pak', 'permanent.pak')
ART = Path(__file__).resolve().parent / 'button_prompts'
IMG_W, IMG_DATA = 24, 56  # igImage2: u16 width, u16 height; u64 size, u64 data pointer
CM_CHAR, CM_UV = 12, 52   # igCharMetrics: u16 character; float u0, v0, u1, v1
# The button characters (Unicode private use area) the prompts use, and the buttons they draw.
CODES = {0xE000: 'Cross', 0xE001: 'Circle', 0xE002: 'Triangle', 0xE003: 'Square', 0xE004: 'L1',
         0xE006: 'R1', 0xE007: 'L2', 0xE008: 'R2', 0xE012: 'Touchpad',
         # the 1.00 fonts (languagestartup.pak, permanent.pak) map them on ASCII characters
         0x298: 'Cross', 0x5E: 'Circle', 0x60: 'Triangle', 0x7E: 'Square', 0x5B: 'L1', 0x5D: 'R1',
         0x3C: 'L2', 0x3E: 'R2'}
MIN_CELL = 50  # a button cell is bigger than this both ways (the font's letters are much smaller)
# Where each button is in our art (button_prompts/<style>.png): the 1.21 font's cells
# (x0, y0, x1, y1; end exclusive). The sticks (L, R) are left as they are.
SHEET = {
    'Cross': (294, 0, 379, 85), 'Circle': (379, 0, 472, 85), 'Triangle': (0, 85, 93, 170),
    'Square': (93, 85, 186, 170), 'L1': (0, 0, 147, 65), 'R1': (147, 0, 294, 65),
    'L2': (263, 170, 364, 263), 'R2': (364, 170, 465, 263), 'Touchpad': (0, 170, 93, 255),
}


def say(message):
    print('Button prompts: ' + message, file=sys.stderr, flush=True)


def _sections_and_fixups(blob):
    magic, _version, _hash, _platform, count = struct.unpack_from('<5I', blob, 0)
    if magic != 0x49475A01:
        raise ValueError('not an IGZ file')
    sections, at = [], 0x14
    while at + 16 <= 0x800:
        _zero, offset, length, _align = struct.unpack_from('<4I', blob, at)
        if offset == 0 and length == 0:
            break
        sections.append((offset, length))
        at += 16
    fixups, at = {}, sections[0][0]
    for _ in range(count):
        tag, n, length, start = struct.unpack_from('<4sIII', blob, at)
        fixups[tag.decode('ascii', 'replace')] = (n, blob[at + start:at + length])
        at += length
    return sections, fixups


def _strings(n, data):
    out, at = [], 0
    for _ in range(n):
        end = data.index(b'\0', at)
        out.append(data[at:end].decode('utf-8', 'replace'))
        at = end + 1 + ((end + 1) & 1)  # each string starts on an even offset
    return out


def _offsets(n, data):
    """A packed offset list: nibbles, 3 value bits and a continuation bit each."""
    nibbles = [v for b in data for v in (b & 15, b >> 4)]
    out, at, previous = [], 0, 0
    for _ in range(n):
        value, shift = 0, 0
        while True:
            nibble = nibbles[at]
            at += 1
            value |= (nibble & 7) << shift
            shift += 3
            if not nibble & 8:
                break
        previous = previous + value * 4 if out or previous else value * 4
        out.append(previous)
    return out


def _objects(blob):
    """[(file offset, type name)] of the font's objects, and the strings."""
    sections, fixups = _sections_and_fixups(blob)
    types = _strings(*fixups['TMET'])

    def file_offset(encoded):
        return sections[1 + (encoded >> 27)][0] + (encoded & 0x7FFFFFF)

    found = []
    for obj in _offsets(*fixups['RVTB']):
        at = file_offset(obj)
        kind, = struct.unpack_from('<Q', blob, at)
        found.append((at, types[kind] if kind < len(types) else '?'))
    return found, _strings(*fixups['TSTR']), file_offset


def button_cells(blob, width, height):
    """{button: (x0, y0, x1, y1)} of the PlayStation buttons in the font's button image, from its
    character metrics (the layout differs between the game's versions)."""
    cells = {}
    objects, _strings_list, _offset = _objects(blob)
    for at, kind in objects:
        if kind != 'igCharMetrics':
            continue
        char, = struct.unpack_from('<H', blob, at + CM_CHAR)
        if char not in CODES:
            continue
        u0, v0, u1, v1 = struct.unpack_from('<4f', blob, at + CM_UV)
        cell = (round(u0 * width), round(v0 * height), round(u1 * width) + 1, round(v1 * height) + 1)
        if cell[2] - cell[0] > MIN_CELL and cell[3] - cell[1] > MIN_CELL:  # not the letter itself
            cells[CODES[char]] = cell
    return cells


def image_location(blob):
    """(width, height, file offset, byte length) of the font's "button" image (tiled)."""
    sections, fixups = _sections_and_fixups(blob)
    types = _strings(*fixups['TMET'])
    strings = _strings(*fixups['TSTR'])

    def file_offset(encoded):
        return sections[1 + (encoded >> 27)][0] + (encoded & 0x7FFFFFF)

    for obj in _offsets(*fixups['RVTB']):
        at = file_offset(obj)
        kind, = struct.unpack_from('<Q', blob, at)
        if kind >= len(types) or types[kind] != 'igImage2':
            continue
        name, = struct.unpack_from('<Q', blob, at + 16)
        if name >= len(strings) or strings[name] != 'button':
            continue
        width, height = struct.unpack_from('<HH', blob, at + IMG_W)
        _size, data = struct.unpack_from('<QQ', blob, at + IMG_DATA)
        length = (width + 7) // 8 * 8 * ((height + 7) // 8 * 8) * 4
        return width, height, file_offset(data), length
    raise ValueError('the font has no button image')


def _morton(x, y):
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)


def untile(data, width, height):
    """Linear texels (memory order, 4 bytes each) of a 1D-thin tiled image."""
    pitch = (width + 7) // 8
    out = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            at = (((y >> 3) * pitch + (x >> 3)) * 64 + _morton(x & 7, y & 7)) * 4
            out[(y * width + x) * 4:(y * width + x) * 4 + 4] = data[at:at + 4]
    return bytes(out)


def tile(linear, width, height, original):
    """The tiled bytes of linear texels; the padding texels keep the original's."""
    pitch = (width + 7) // 8
    out = bytearray(original)
    for y in range(height):
        for x in range(width):
            at = (((y >> 3) * pitch + (x >> 3)) * 64 + _morton(x & 7, y & 7)) * 4
            out[at:at + 4] = linear[(y * width + x) * 4:(y * width + x) * 4 + 4]
    return bytes(out)


def read_png(path):
    """(width, height, RGBA bytes) of an 8-bit RGBA, non-interlaced PNG."""
    blob = Path(path).read_bytes()
    if blob[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path} is not a PNG')
    at, idat, width = 8, b'', 0
    while at < len(blob):
        length, = struct.unpack_from('>I', blob, at)
        tag = blob[at + 4:at + 8]
        body = blob[at + 8:at + 8 + length]
        if tag == b'IHDR':
            width, height, depth, color, _c, _f, interlace = struct.unpack('>IIBBBBB', body)
            if (depth, color, interlace) != (8, 6, 0):
                raise ValueError(f'{path}: only 8-bit RGBA, not interlaced')
        elif tag == b'IDAT':
            idat += body
        at += 12 + length
    raw, stride = zlib.decompress(idat), width * 4
    out, previous = bytearray(), bytearray(stride)
    for y in range(height):
        kind, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            left = line[i - 4] if i >= 4 else 0
            up, corner = previous[i], previous[i - 4] if i >= 4 else 0
            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        out += line
        previous = line
    return width, height, bytes(out)


def write_png(path, width, height, rgba):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    raw = b''.join(b'\0' + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def swap_red_blue(rgba):
    out = bytearray(rgba)
    out[0::4], out[2::4] = rgba[2::4], rgba[0::4]
    return bytes(out)


def restyled_font(blob, art):
    """The font with the PlayStation buttons of its button image replaced by art's (art: width,
    height, RGBA, laid out as SHEET), each copied into the cell the font's metrics give that
    button. The image is stored BGRA (the texture's swizzle turns it back)."""
    width, height, at, length = image_location(blob)
    cells = button_cells(blob, width, height)  # the 1.00 fonts have no touchpad button
    if not {'Cross', 'Circle', 'Triangle', 'Square'} <= set(cells):
        raise ValueError('the font has no face buttons')
    art_width, _art_height, art_rgba = art
    stored = swap_red_blue(art_rgba)
    original = blob[at:at + length]
    linear = bytearray(untile(original, width, height))
    for name, (sx0, sy0, sx1, sy1) in SHEET.items():
        if name not in cells:
            continue
        x0, y0, x1, y1 = cells[name]
        if (x1 - x0, y1 - y0) != (sx1 - sx0, sy1 - sy0):
            raise ValueError(f'{name}: the font cell is {x1 - x0}x{y1 - y0}, the art {sx1 - sx0}x{sy1 - sy0}')
        for row in range(y1 - y0):
            src = ((sy0 + row) * art_width + sx0) * 4
            dst = ((y0 + row) * width + x0) * 4
            linear[dst:dst + (x1 - x0) * 4] = stored[src:src + (x1 - x0) * 4]
    return blob[:at] + tile(linear, width, height, original) + blob[at + length:]


def chosen_style(config, env):
    """BB_BUTTON_PROMPTS, else bbport.ini button_prompts; playstation when neither or unknown."""
    value = env.get('BB_BUTTON_PROMPTS', '')
    if not value and config and Path(config).is_file():
        for line in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
            key, sep, setting = line.partition('=')
            if sep and key.strip() == 'button_prompts':
                value = setting.strip()
    value = value.strip().lower()
    return value if value in STYLES else 'playstation'


def prepare(game, out, config, env, base):
    """{guest archive: patch file} of the chosen style's fonts (none for the PlayStation buttons),
    each keeping the patch base ({guest: patch file}) has for the same archive. Kept between
    launches while the archives, the art and the base patches are the same."""
    style = chosen_style(config, env)
    if style == 'playstation':
        return {}
    import hashlib
    import json
    art_path = ART / f'{style}.png'
    folder = Path(out) / 'button_prompts'
    from mod_file_paths import resolve
    archives = [resolve(Path(game) / 'archives' / name) for name in ARCHIVES if resolve(Path(game) / 'archives' / name).is_file()]
    stamp = {'style': style, 'art': hashlib.sha256(art_path.read_bytes()).hexdigest(),
             'archives': {a.name: [a.stat().st_size, a.stat().st_mtime_ns] for a in archives},
             'base': {g: hashlib.sha256(Path(p).read_bytes()).hexdigest() for g, p in sorted(base.items())}}
    stamp_file = folder / 'stamp.json'
    try:
        made = json.loads(stamp_file.read_text(encoding='utf-8'))
        if made['stamp'] == stamp and all(Path(p).is_file() for p in made['patches'].values()):
            return {g: Path(p) for g, p in made['patches'].items()}
    except (OSError, ValueError, KeyError):
        pass
    art = read_png(art_path)
    folder.mkdir(parents=True, exist_ok=True)
    patches = {}
    for path in archives:
        archive = Archive(path)
        entries = {}
        for f in archive.files:
            if f['path'].replace('\\', '/').lower() in [name.lower() for name in FONTS]:
                entries[f['index']] = restyled_font(bytes(archive.extract(f)), art)
        if not entries:
            continue
        guest = f'/app0/archives/{path.name}'
        target = folder / (path.stem + '.bbpatch')
        archive_patch.write_entries_patch(target, path, guest, entries, base.get(guest))
        patches[guest] = target
    stamp_file.write_text(json.dumps({'stamp': stamp, 'patches': {g: str(p) for g, p in patches.items()}}),
                          encoding='utf-8')
    say(f'{style} buttons in {sum(1 for _ in patches)} archives')
    return patches


def main():
    """run.sh's launch: the patch files made, one a line (each keeping the --base patch of its
    archive, so it takes that one's place)."""
    import os
    p = argparse.ArgumentParser(description='Archive patches for Xbox button prompts.')
    p.add_argument('game')
    p.add_argument('--out', required=True)
    p.add_argument('--settings', help='bbport.ini (button_prompts=)')
    p.add_argument('--style', choices=STYLES, help='instead of the settings')
    p.add_argument('--base', action='append', default=[], help='an archive patch made before (repeatable)')
    a = p.parse_args()
    env = dict(os.environ, **({'BB_BUTTON_PROMPTS': a.style} if a.style else {}))
    base = {archive_patch.read(path)[1]: Path(path) for path in a.base}
    try:
        made = prepare(a.game, a.out, a.settings, env, base)
    except (OSError, ValueError) as error:
        say(f'{error}; PlayStation buttons')
        made = {}
    for patch in made.values():
        print(patch)


if __name__ == '__main__':
    main()
