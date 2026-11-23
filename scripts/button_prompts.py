#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Button prompts with Xbox buttons (bbport.ini button_prompts=xbox; default playstation).

The game draws every button prompt (Cross, Circle, L1, the sticks...) from one image, "button",
of its fonts fonts/GeekSpeak.igz, fonts/GeekSpeak_Outline.igz and the title font
fonts/Zoinks_Outline.igz (each has its own copy): 512x263 RGBA8 in the PS4's
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
# Zoinks_Outline is the title font, with its own copy of the button image: "Press X to skip" in
# cutscenes and "Press X to continue" on the podium (issue #90).
FONTS = ('fonts/GeekSpeak.igz', 'fonts/GeekSpeak_Outline.igz', 'fonts/Zoinks_Outline.igz')
# The archives with those fonts (the update's copies are the ones the 1.21 game reads; the
# others are patched too, so no screen shows a PlayStation button).
ARCHIVES = ('update.pak', 'languagestartup.pak', 'permanent.pak')
# Options > Controls (issue #256): opening the page, its script asks the controller type
# (CInputNodes::checkControllerType2, one output per type) and each output keeps that platform's
# animation tag for the page to play: the controller diagram and the labels on its lines. The
# page's own pose is the PS4 one, which is what shows when no type matches. The game has the Xbox
# One diagram too (ui_octane_controller_scheme_xb1): with Xbox prompts the node before the check
# goes straight to the XB1 output, so the Xbox diagram plays whatever the controller. Every copy of
# the script is patched: the updates' and the original domain archive's.
CONTROLS_SCRIPT = 'vsc/UI/Octane_Options_Menu_Page/Octane_Options_Menu_Page_Root.igz'
CONTROLS_TAGS = ('AnimationTag_PS4_ControlScheme', 'AnimationTag_XB1_ControlScheme')
CONTROLS_ARCHIVES = ('update2.pak', 'update.pak', 'juicedomain_permanent.pak')
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
# The voice chat's loudspeaker (scripts/assets/voice_glyph.png, tools/make_voice_glyph.py), a
# character of its own: the nickname over a talking racer's kart ends with it
# (src/runtime_replay_play.c), so the game draws it with the name. In the 1.21 fonts' button image
# it takes an unused badge's cell (x0, y0, x1, y1; end exclusive).
SPEAKER_CODE = 0xE013
SPEAKER_CELL = (180, 170, 263, 259)
SPEAKER_ART = Path(__file__).resolve().parent / 'assets' / 'voice_glyph.png'
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


def _alternate_list(blob):
    """(file offset of its pointer array, [(slot file offset, metrics file offset, character)]) of
    the font's alternate character metrics (the ones drawn from the button image)."""
    objects, _strings_list, file_offset = _objects(blob)
    for at, kind in objects:
        if kind != 'igCharMetricsList':
            continue
        count, = struct.unpack_from('<I', blob, at + 12)
        data, = struct.unpack_from('<Q', blob, at + 32)
        array = file_offset(data)
        slots = []
        for i in range(count):
            encoded, = struct.unpack_from('<Q', blob, array + i * 8)
            metrics = file_offset(encoded)
            slots.append((array + i * 8, metrics, struct.unpack_from('<H', blob, metrics + CM_CHAR)[0]))
        if any(char in CODES and char >= 0xE000 for _, _, char in slots):
            return array, slots
    return None, []


def with_speaker(blob, glyph):
    """The font with the voice chat's loudspeaker (glyph: width, height, RGBA) as character
    SPEAKER_CODE, or None when the font has no room for it. It takes SPEAKER_CELL of the button
    image, which the font's metrics give a character 0 (one no text can ask for): that metrics
    gets the loudspeaker's code and moves to the end of the alternate list, which stays sorted."""
    width, height, at, length = image_location(blob)
    array, slots = _alternate_list(blob)
    if array is None or any(char == SPEAKER_CODE for _, _, char in slots):
        return None
    chosen = None
    for i, (_slot, metrics, char) in enumerate(slots):
        u0, v0, u1, v1 = struct.unpack_from('<4f', blob, metrics + CM_UV)
        cell = (round(u0 * width), round(v0 * height), round(u1 * width) + 1, round(v1 * height) + 1)
        if char == 0 and cell == SPEAKER_CELL:
            chosen = i
    codes = [char for _, _, char in slots]
    if chosen is None or max(codes) >= SPEAKER_CODE:
        return None
    glyph_width, glyph_height, glyph_rgba = glyph
    x0, y0, x1, y1 = SPEAKER_CELL
    if (glyph_width, glyph_height) != (x1 - x0, y1 - y0):
        raise ValueError(f'the loudspeaker is {glyph_width}x{glyph_height}, its cell {x1 - x0}x{y1 - y0}')
    out = bytearray(blob)
    stored = swap_red_blue(glyph_rgba)
    original = blob[at:at + length]
    linear = bytearray(untile(original, width, height))
    for row in range(y1 - y0):
        dst = ((y0 + row) * width + x0) * 4
        linear[dst:dst + glyph_width * 4] = stored[row * glyph_width * 4:(row + 1) * glyph_width * 4]
    out[at:at + length] = tile(linear, width, height, original)
    struct.pack_into('<H', out, slots[chosen][1] + CM_CHAR, SPEAKER_CODE)
    # the pointers in the array, the chosen one last (each slot stays a pointer: fixups unchanged)
    pointers = [blob[slot:slot + 8] for slot, _, _ in slots]
    pointers.append(pointers.pop(chosen))
    out[array:array + 8 * len(pointers)] = b''.join(pointers)
    return bytes(out)


def with_clan_brackets(blob, outlined):
    """Give the two unused 1.21 alternate glyphs literal square brackets.

    Older fonts use the ASCII codes for shoulder buttons. Only the newer
    Unicode button layout has these two spare cells and accepts this patch.
    """
    array, slots = _alternate_list(blob)
    if array is None or any(code in (91, 93) for _, _, code in slots):
        return blob
    width, height, at, length = image_location(blob)
    cells = {}
    for _, metrics, code in slots:
        if code:
            continue
        u, v, right, bottom = struct.unpack_from('<4f', blob, metrics + CM_UV)
        cell = (round(u * width), round(v * height),
                round(right * width) + 1, round(bottom * height) + 1)
        if cell != SPEAKER_CELL:
            cells[cell] = metrics
    if set(cells) != {(93, 170, 180, 259), (186, 85, 271, 170)}:
        return blob
    out = bytearray(blob)
    linear = bytearray(untile(blob[at:at + length], width, height))
    # Symmetric brackets, including a transparent margin for atlas filtering.
    gw, gh = 28, 76
    def stroke(px, py):
        return 4 <= px < 24 and 4 <= py < gh - 4 and (px < 11 or py < 11 or py >= gh - 11)

    for code, ((x0, y0, _, _), metrics) in zip((91, 93), sorted(cells.items())):
        for y in range(gh):
            for x in range(gw):
                local_x = x if code == 91 else gw - 1 - x
                if stroke(local_x, y):
                    pixel = bytes((255, 255, 255, 255))
                elif outlined and any(stroke(local_x + dx, y + dy) for dx, dy in
                                      ((-2, 0), (2, 0), (0, -2), (0, 2))):
                    pixel = bytes((0, 0, 0, 255))
                else:
                    pixel = bytes(4)
                pos = ((y0 + y) * width + x0 + x) * 4
                linear[pos:pos + 4] = pixel
        struct.pack_into('<H', out, metrics + CM_CHAR, code)
        struct.pack_into('<9f', out, metrics + 16, 0, 0, gw, 0, 12, gw, gh, 0, 0)
        struct.pack_into('<4f', out, metrics + CM_UV, x0 / width, y0 / height,
                         (x0 + gw - 1) / width, (y0 + gh - 1) / height)
    ordered = sorted(slots, key=lambda slot: struct.unpack_from('<H', out, slot[1] + CM_CHAR)[0])
    out[array:array + len(slots) * 8] = b''.join(blob[pos:pos + 8] for pos, _, _ in ordered)
    out[at:at + length] = tile(linear, width, height, blob[at:at + length])
    return bytes(out)


def _controls_check(igz):
    """(the controller type check node, its XB1 output's reference, the nodes leading to the
    check) in the Options page's script; None when it is not the script expected.

    igVscCppActionNode: +0x28 the next node, +0x30 an igVscActionNodeList of the outputs (u32
    count at +0x10, references at +0x20). Each output is an igVscAssignNode whose source (+0x10,
    an igVscObjectFieldAccessor) reads an igHandleMetaField (+0x10) whose default (+0x38) is a
    handle: an index into the external names (EXNM) that are not meta types."""
    from igz_file import unpack_offsets
    kind = dict(igz.objects())
    rofs = set(igz.sites['ROFS'])
    try:
        f = igz.fixup('RHND')
        exnm = igz.fixup('EXNM')
    except KeyError:
        return None
    rhnd = set(unpack_offsets(f[3], f[1]))
    pairs = [struct.unpack_from('<II', exnm[3], 8 * k) for k in range(exnm[1])]
    meta = ('metaobject', 'metafunction', 'metafield')
    handles = [igz.strings[a & 0x7FFFFFFF] for a, b in pairs if igz.strings[b & 0x7FFFFFFF] not in meta]

    def ref(at, want=None):
        target = igz.u64(at) & 0x7FFFFFF if at in rofs else None
        return target if want is None or kind.get(target) == want else None

    def assigned_tag(node):
        source = ref(node + 0x10, 'igVscObjectFieldAccessor')
        field = source is not None and ref(source + 0x10, 'igHandleMetaField')
        default = field and ref(field + 0x38)
        if default is None or default not in rhnd:
            return None
        index = igz.u64(default) & 0x7FFFFFFF
        return handles[index] if index < len(handles) else None
    found = []
    for node, t in igz.objects():
        outputs = ref(node + 0x30, 'igVscActionNodeList') if t == 'igVscCppActionNode' else None
        if outputs is None or igz.u32(outputs + 0x10) < 2 or ref(outputs + 0x20) is None:
            continue
        refs = ref(outputs + 0x20) + 8
        first, second = (ref(refs - 8 + 8 * k, 'igVscAssignNode') for k in (0, 1))
        if first and second and (assigned_tag(first), assigned_tag(second)) == CONTROLS_TAGS:
            before = [p for p, pt in igz.objects() if pt == 'igVscCppActionNode' and ref(p + 0x28) == node]
            found.append((node, refs, before))
    return found[0] if len(found) == 1 and found[0][2] else None


def xbox_controls(blob):
    """The Options page's script going from the node before the controller type check straight to
    its XB1 output; None when the script is not the one expected."""
    from igz_file import Igz
    try:
        igz = Igz(blob)
        check = _controls_check(igz)
    except (ValueError, KeyError, IndexError, struct.error):
        return None
    if check is None:
        return None
    _node, xb1_ref, before = check
    data = igz.sections[1][1]
    out = bytearray(blob)
    for node in before:
        out[data + node + 0x28:data + node + 0x30] = blob[data + xb1_ref:data + xb1_ref + 8]
    return bytes(out)


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
    """{guest archive: patch file} of the fonts: the chosen style's buttons (the game's own for
    the PlayStation ones) and the voice chat's loudspeaker character (with_speaker; env gets
    BB_VOICE_GLYPH=1 when the fonts have it), with Xbox prompts also the Options page's Xbox
    controller diagram (xbox_controls), each keeping the patch base ({guest: patch file})
    has for the same archive. Kept between launches while the archives, the art and the base
    patches are the same."""
    import hashlib
    import json
    style = chosen_style(config, env)
    env.pop('BB_VOICE_GLYPH', None)
    art_path = ART / f'{style}.png' if style != 'playstation' else None
    folder = Path(out) / 'button_prompts'
    from mod_file_paths import resolve
    names = list(ARCHIVES) + [name for name in CONTROLS_ARCHIVES if name not in ARCHIVES]
    archives = [resolve(Path(game) / 'archives' / name) for name in names if resolve(Path(game) / 'archives' / name).is_file()]
    import clan_tag_font
    import name_swatch_font
    stamp = {'swatch_font': hashlib.sha256(Path(name_swatch_font.__file__).read_bytes() +
                                          name_swatch_font.PALETTE.read_bytes()).hexdigest(),
             'clan_font': hashlib.sha256(Path(clan_tag_font.__file__).read_bytes()).hexdigest(), 'code': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
             'style': style, 'art': hashlib.sha256(art_path.read_bytes()).hexdigest() if art_path else '',
             'speaker': hashlib.sha256(SPEAKER_ART.read_bytes()).hexdigest(), 'fonts': list(FONTS),
             'archives': {a.name: [a.stat().st_size, a.stat().st_mtime_ns] for a in archives},
             'base': {g: hashlib.sha256(Path(p).read_bytes()).hexdigest() for g, p in sorted(base.items())}}
    stamp_file = folder / 'stamp.json'
    made = {}
    try:
        made = json.loads(stamp_file.read_text(encoding='utf-8'))
        if made['stamp'] == stamp and all(Path(p).is_file() for p in made['patches'].values()):
            if made.get('clan_tiles'):
                env['BB_CLAN_TILES'] = '1'
            if made.get('speaker'):
                env['BB_VOICE_GLYPH'] = '1'
            return {g: Path(p) for g, p in made['patches'].items()}
    except (OSError, ValueError, KeyError, TypeError):
        pass
    from cache_cleanup import discard_files
    if isinstance(made, dict) and isinstance(made.get('patches'), dict):
        discard_files(folder, made['patches'].values())
    art = read_png(art_path) if art_path else None
    glyph = read_png(SPEAKER_ART)
    folder.mkdir(parents=True, exist_ok=True)
    patches, speakers, diagrams = {}, 0, 0
    tiled_fonts = set()
    for path in archives:
        archive = Archive(path)
        entries = {}
        for f in archive.files:
            entry = f['path'].replace('\\', '/').lower()
            if style == 'xbox' and path.name in CONTROLS_ARCHIVES and entry == CONTROLS_SCRIPT.lower():
                script = xbox_controls(bytes(archive.extract(f)))
                if script is not None:
                    entries[f['index']] = script
                    diagrams += 1
                continue
            if path.name in ARCHIVES and entry in [name.lower() for name in FONTS]:
                blob = bytes(archive.extract(f))
                styled = restyled_font(blob, art) if art else blob
                spoken = with_speaker(styled, glyph)
                speakers += spoken is not None
                styled = with_clan_brackets(spoken or styled, 'outline' in entry)
                if spoken:
                    tiled = clan_tag_font.add_native(styled)
                    if tiled != styled:
                        tiled_fonts.add(entry)
                    styled = tiled
                if styled != blob:
                    entries[f['index']] = styled
        if not entries:
            continue
        guest = f'/app0/archives/{path.name}'
        target = folder / (path.stem + '.bbpatch')
        archive_patch.write_entries_patch(target, path, guest, entries, base.get(guest))
        patches[guest] = target
    clan_tiles = tiled_fonts == {font.lower() for font in FONTS}
    if clan_tiles:
        env['BB_CLAN_TILES'] = '1'
    stamp_file.write_text(json.dumps({'stamp': stamp, 'patches': {g: str(p) for g, p in patches.items()},
                                      'speaker': speakers > 0, 'clan_tiles': clan_tiles}), encoding='utf-8')
    if speakers:
        env['BB_VOICE_GLYPH'] = '1'
    say(f'{style} buttons, the loudspeaker in {speakers} fonts, ' +
        (f'the Xbox controls diagram in {diagrams} scripts, ' if style == 'xbox' else '') + f'{len(patches)} archives')
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
