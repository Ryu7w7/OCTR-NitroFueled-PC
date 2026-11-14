#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A seasonal title screen: the season the online server announces (its dashboard, GET /ryunet/theme:
"christmas"), or bbport.ini title_theme=christmas|off (BB_TITLE_THEME) over it.

The start screen (ui/octane_startscreen_menu.igz and its level, octane_startscreen.pak) draws the logo
over three layers: a blue gradient (ui/octane/background/octane_ui_menu_bg, 860x1120 BC3, in
juicedomain_permanent.pak and legal.pak), a waving checkered board over it, and a pattern of white
icons (ui/octane/background/octane_ui_raceselect_bg_pattern_adventure, 256x256 BC3; the level's copy in
octane_startscreen.pak). The start screen's own colour is the kart customization background's
(ui/octane/customization/octane_ui_custo_bg_color_1p, 128x1024 BGRA8: player 1's blue; 2p is red, 3p green,
4p yellow), and custo_bg_color_event during events. For Christmas the gradient takes the Winter Festival Grand Prix's red and pink,
and so does the customization background in the title's level (the garage keeps its blue),
and the icons become the trees of Gingerbread Joyride (its race select pattern,
octane_ui_raceselect_bg_pattern_luminous in update2.pak, of the same size and format), as the game's
start screen looked in December 2019. The logo stays the game's own. The gradient is the menus' own
background too, so they take the colours as well.

The textures are the player's own, changed when the game starts: archive patches carry them (the
archives stay as they are), made once and kept while the archives are the same. Pure Python, since the
portable package's Python has no image libraries: each 4x4 block of the gradient is decoded, coloured
and encoded again (its alpha kept). The PS4 stores them in 1D-thin tiling: 8x8 blocks, Morton order
inside each.

usage: season_art.py GAME --out OUT [--settings bbport.ini] [--theme christmas]
"""
import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import archive_patch  # noqa: E402
import igz_file  # noqa: E402
from archive_reader import Archive  # noqa: E402
from button_prompts import _morton, _offsets, _sections_and_fixups, _strings  # noqa: E402

THEMES = ('christmas',)
GRADIENT = '!octane_ui_menu_bg`tga'
ICONS = '!octane_ui_raceselect_bg_pattern_adventure`tga'
TREES = '!octane_ui_raceselect_bg_pattern_luminous`tga'
# Where each is changed: the gradient wherever the game loads it; the icons only in the title's level,
# so the Adventure menus keep theirs.
GRADIENT_ARCHIVES = ('juicedomain_permanent.pak', 'legal.pak')
ICON_ARCHIVES = ('octane_startscreen.pak',)
# The start screen's colour: the title's level copy of player 1's, and the events' one.
BACKDROPS = {'octane_startscreen.pak': ('!octane_ui_custo_bg_color_1p`tga',),
             'update2.pak': ('!octane_ui_custo_bg_color_event`tga',)}
TREE_ARCHIVES = ('update2.pak',)
# The title's Crash (with his yo-yo) and his materials, in the title's level; the Winter Festival
# Crash (the reindeer outfit) in update2.pak's own archives (bbport.ini title_crash=reindeer, or
# BB_TITLE_CRASH).
CRASHES = ('reindeer',)
CRASH_ARCHIVE = 'octane_startscreen.pak'
CRASH_MODEL = 'actors/octane/cutscene/cutscene_crashmainmenu.igz'
CRASH_MATERIALS = 'materialinstances/actors/octane/characters/crashdriver_materials,cutscene_crashmainmenu,cf.igz'
REINDEER_MODEL_PAK = 'archives/drivercrash_outfit_gp6_p1_cutscene_skins.pak'
REINDEER_MATERIALS_PAK = 'archives/drivercrash_outfit_gp6_p1_materials.pak'
REINDEER_MODEL = 'actors_stripped/drivers/gp6_skin_csh_p1_lod0_stripped.igz'
REINDEER_MATERIALS = 'materialinstances/actors/octane/characters/gp6_skin_csh_p1_materials.igz'
# Christmas decoration: Gingerbread Joyride's cupcakes (update2.pak's T506_Luminous.pak) in the
# title's bottom corners, as entities added to its map (copies of its colour backdrop's entity, a
# model alone) and files added to its level's package.
TITLE_MAP = 'maps/octane/octane_startscreen/octane_startscreen.igz'
TITLE_PACKAGE = 'packages/generated/maps/octane/octane_startscreen/octane_startscreen_pkg.igz'
CUPCAKE_PAK = 'archives/t506_luminous.pak'
CUPCAKES = ('models/levels/octane/t506_luminous/assets/t506_cupcake_01.igz',
            'models/levels/octane/t506_luminous/assets/t506_cupcake_02.igz')
# Places from the logo's place, in world units: x is depth (-x towards the camera), +y is screen
# left, z is up (the base of a cupcake). Set by hand in the game (title_decor.json).
# (cupcake, dx, dy, dz, size, turn in radians)
DECORATION = ((0, 0.0, 180.0, -65.0, 1.8, 0.0), (1, 0.0, -180.0, -65.0, 1.8, 0.0))
# Places set by hand instead (cupcakes and snow), in this file beside bbport.ini: see decoration_layout.
LAYOUT_FILE = 'title_decor.json'
PACKAGE_TYPES = {'textures': 'texture', 'materialinstances': 'material_instances', 'models': 'model',
                 'vfx': 'effect'}
# Snow: the Winter Festival podium's falling snow (in update2.pak itself), a grid of it in front of
# the logo, across the whole view (its flakes do not fall far).
SNOW_FILE = 'vfx/ctr/podium/ctr_h100_pod_gp6_falling_snow.igz'
SNOW_EFFECT = 'ctr_h100_pod_GP6_falling_snow'
SNOW_SPOTS = tuple((-100.0, y, z) for z in (140.0, 70.0, 0.0, -70.0) for y in (-220.0, -110.0, 0.0, 110.0, 220.0))
# Christmas: the gradients' dark ends to a soft red-pink, their light ends to a light pink (by brightness).
DARK, LIGHT = (215, 95, 140), (255, 170, 195)
DARK_AT, LIGHT_AT = 70.0, 160.0   # the blue gradient's brightness (0..255) at its ends


def say(message):
    print('Title theme: ' + message, file=sys.stderr, flush=True)


OFF = ('off', 'none', 'no', '0', 'default')


def setting(config, env, variable, ini_key):
    """The environment's variable, else bbport.ini's ini_key, in lower case ('' when neither)."""
    value = env.get(variable, '')
    if not value and config and Path(config).is_file():
        for line in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
            key, sep, setting_value = line.partition('=')
            if sep and key.strip() == ini_key:
                value = setting_value.strip()
    return value.strip().lower()


def chosen_theme(config, env):
    """The title's theme: the player's (BB_TITLE_THEME, bbport.ini title_theme: a theme, or off), else
    the season the online server announces (BB_THEME, run.py setup_online); '' for the game's own."""
    local = setting(config, env, 'BB_TITLE_THEME', 'title_theme')
    if local in OFF:
        return ''
    if local in THEMES:
        return local
    server = env.get('BB_THEME', '').strip().lower()
    return server if server in THEMES else ''


def chosen_crash(config, env, theme):
    """The title's Crash: the player's (BB_TITLE_CRASH, bbport.ini title_crash: reindeer, or off), else
    the theme's (Christmas: the reindeer outfit)."""
    local = setting(config, env, 'BB_TITLE_CRASH', 'title_crash')
    if local in OFF:
        return ''
    if local in CRASHES:
        return local
    return 'reindeer' if theme == 'christmas' else ''


def image(blob):
    """(width, height, data offset, data size, mip levels) of the texture's igImage2."""
    sections, fixups = _sections_and_fixups(blob)
    types = _strings(*fixups['TMET'])

    def file_offset(encoded):
        return sections[1 + (encoded >> 27)][0] + (encoded & 0x7FFFFFF)

    for obj in _offsets(*fixups['RVTB']):
        at = file_offset(obj)
        kind, = struct.unpack_from('<Q', blob, at)
        if kind < len(types) and types[kind] == 'igImage2':
            width, height, _depth, levels = struct.unpack_from('<HHHH', blob, at + 24)
            size, = struct.unpack_from('<I', blob, at + 56)
            pointer, = struct.unpack_from('<Q', blob, at + 64)
            return width, height, file_offset(pointer), size, max(1, levels)
    raise ValueError('no image in the texture')


def block_grid(width, height):
    """A level's blocks, padded to whole 8x8 tiles: (blocks across, blocks down)."""
    return ((width + 3) // 4 + 7) // 8 * 8, ((height + 3) // 4 + 7) // 8 * 8


def tiled_index(x, y, across):
    return ((y >> 3) * (across // 8) + (x >> 3)) * 64 + _morton(x & 7, y & 7)


def rgb565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def colour_palette(c0, c1):
    a, b = rgb565(c0), rgb565(c1)
    if c0 > c1:
        return [a, b, tuple((2 * p + q) // 3 for p, q in zip(a, b)), tuple((p + 2 * q) // 3 for p, q in zip(a, b))]
    return [a, b, tuple((p + q) // 2 for p, q in zip(a, b)), (0, 0, 0)]


def decode_colours(colour_block):
    c0, c1, bits = struct.unpack_from('<HHI', colour_block, 0)
    palette = colour_palette(c0, c1)
    return [palette[(bits >> (2 * i)) & 3] for i in range(16)]


def to565(c):
    r, g, b = (max(0, min(255, int(v))) for v in c)
    return ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | (b * 31 + 127) // 255


def encode_colours(pixels):
    """A colour block (4-colour mode) for 16 RGB pixels: the darkest and the brightest as its ends."""
    lum = [p[0] * 3 + p[1] * 6 + p[2] for p in pixels]
    c0, c1 = to565(pixels[lum.index(max(lum))]), to565(pixels[lum.index(min(lum))])
    if c0 == c1:
        return struct.pack('<HHI', c0, c1, 0)
    if c0 < c1:
        c0, c1 = c1, c0
    palette = colour_palette(c0, c1)
    bits = 0
    for i, p in enumerate(pixels):
        best = min(range(4), key=lambda k: (palette[k][0] - p[0]) ** 2 + (palette[k][1] - p[1]) ** 2 + (palette[k][2] - p[2]) ** 2)
        bits |= best << (2 * i)
    return struct.pack('<HHI', c0, c1, bits)


def christmas_gradient(blob):
    """The menus' blue gradient (BC3) in red and pink, by each pixel's brightness."""
    width, height, data, size, levels = image(blob)
    across, down = block_grid(width, height)
    if levels != 1 or across * down * 16 != size:
        raise ValueError('unexpected gradient layout')
    out = bytearray(blob)
    span = [LIGHT[k] - DARK[k] for k in range(3)]
    for by in range((height + 3) // 4):
        for bx in range((width + 3) // 4):
            colour = data + tiled_index(bx, by, across) * 16 + 8
            pixels = []
            for r, g, b in decode_colours(blob[colour:colour + 8]):
                v = min(1.0, max(0.0, (0.3 * r + 0.59 * g + 0.11 * b - DARK_AT) / (LIGHT_AT - DARK_AT)))
                pixels.append(tuple(DARK[k] + span[k] * v for k in range(3)))
            out[colour:colour + 8] = encode_colours(pixels)
    return bytes(out)


def christmas_backdrop(blob):
    """The customization background's gradient (BGRA8) in red and pink, each pixel by its own brightness
    (so its tiling does not matter)."""
    width, height, data, size, levels = image(blob)
    if levels != 1 or size != width * height * 4:
        raise ValueError('unexpected backdrop layout')
    pixels = blob[data:data + size]
    lum = [0.3 * pixels[i + 2] + 0.59 * pixels[i + 1] + 0.11 * pixels[i] for i in range(0, size, 4)]
    low, high = min(lum), max(lum)
    span = [LIGHT[k] - DARK[k] for k in range(3)]
    out = bytearray(blob)
    for n, v in enumerate(lum):
        t = (v - low) / (high - low) if high > low else 1.0
        r, g, b = (int(DARK[k] + span[k] * t) for k in range(3))
        at = data + n * 4
        out[at:at + 3] = bytes((b, g, r))
    return bytes(out)


def with_image_of(blob, source):
    """blob with its image data replaced by source's, of the same size and format."""
    w, h, data, size, levels = image(blob)
    sw, sh, sdata, ssize, slevels = image(source)
    if (w, h, size, levels) != (sw, sh, ssize, slevels):
        raise ValueError('the trees and the icons differ in size')
    return blob[:data] + source[sdata:sdata + size] + blob[data + size:]


def with_strings_renamed(blob, old, new):
    """blob with old changed to new in its string table (TSTR), new no longer than old: the table
    keeps its size (zeros after its last string), so nothing after it moves."""
    if len(new) > len(old):
        raise ValueError('a longer name does not fit')
    _magic, _version, _hash, _platform, fixups = struct.unpack_from('<5I', blob, 0)
    sections, _ = _sections_and_fixups(blob)
    at = sections[0][0]
    for _ in range(fixups):
        tag, n, length, start = struct.unpack_from('<4sIII', blob, at)
        if tag == b'TSTR':
            strings = _strings(n, blob[at + start:at + length])
            table = bytearray()
            for s in strings:
                table += s.replace(old, new).encode('utf-8') + b'\0'
                if len(table) & 1:
                    table += b'\0'
            if len(table) > length - start:
                raise ValueError('the string table grew')
            out = bytearray(blob)
            out[at + start:at + length] = bytes(table).ljust(length - start, b'\0')
            return bytes(out)
        at += length
    raise ValueError('no string table')


def reindeer_crash(update2, archive):
    """{index: payload} and {path: payload} that put the Winter Festival Crash (the reindeer outfit,
    DriverCrash_Outfit_GP6_P1, in update2.pak's own archives) in the title's place: his model, with
    the title's material names, his materials in place of the title Crash's, and his own textures
    (the shared Crash ones are in the title's level already)."""
    model_entry, materials_entry = find(archive, CRASH_MODEL), find(archive, CRASH_MATERIALS)
    if not model_entry or not materials_entry:
        raise ValueError("the title's Crash is not in its level")
    outer = Archive(update2)
    pieces = {}
    for name in (REINDEER_MODEL_PAK, REINDEER_MATERIALS_PAK):
        entry = find(outer, name)
        if not entry:
            raise ValueError('the Winter Festival Crash is not in the archives')
        with outer.nested(entry) as inner:
            for f in inner.files:
                low = f['path'].replace('\\', '/').lower()
                if low == REINDEER_MODEL or low == REINDEER_MATERIALS or (
                        low.startswith('textures/') and 'gp6_skin_csh_p1' in low):
                    pieces[f['path'].replace('\\', '/')] = bytes(inner.extract(f))
    model = next((v for k, v in pieces.items() if k.lower() == REINDEER_MODEL), None)
    materials = next((v for k, v in pieces.items() if k.lower() == REINDEER_MATERIALS), None)
    textures = {k: v for k, v in pieces.items() if k.lower().startswith('textures/')}
    if model is None or materials is None or len(textures) != 7:
        raise ValueError('the Winter Festival Crash is incomplete')
    model = with_strings_renamed(model, 'GP6_Skin_CSH_P1_materials,GP6_Skin_CSH_P1_Lod0,1ef',
                                 'CrashDriver_materials,Cutscene_CrashMainMenu,cf')
    return {model_entry['index']: model, materials_entry['index']: materials}, textures


def dependencies(blob):
    """The archive paths an IGZ file loads first (its TDEP: textures, materials, models)."""
    _sections, fixups = _sections_and_fixups(blob)
    if 'TDEP' not in fixups:
        return []
    count, raw = fixups['TDEP']
    paths = [s.decode('utf-8') for s in raw.split(b'\0')][1:2 * count:2]
    out = []
    for path in paths:
        kind, sep, rest = path.partition(':/')
        kind = {'materials': 'materialinstances'}.get(kind, kind)
        if sep and kind in PACKAGE_TYPES:
            out.append(f'{kind}/{rest}'.lower())
    return out


def cupcake_files(update2, title):
    """[(path, payload)] the cupcakes need that the title's archive lacks: textures, then materials,
    then the models (the order the level's package loads them in). Shared textures (permanent.pak's)
    are left out."""
    have = {f['path'].replace('\\', '/').lower() for f in title.files}
    outer = Archive(update2)
    entry = find(outer, CUPCAKE_PAK)
    if not entry:
        raise ValueError("Gingerbread Joyride's cupcakes are not in the archives")
    found = {}
    with outer.nested(entry) as inner:
        by_path = {f['path'].replace('\\', '/').lower(): f for f in inner.files}
        todo = list(CUPCAKES)
        while todo:
            path = todo.pop(0)
            if path in found or path in have or path not in by_path:
                continue
            f = by_path[path]
            found[path] = (f['path'].replace('\\', '/'), bytes(inner.extract(f)))
            todo += dependencies(found[path][1])
    if not all(path in found for path in CUPCAKES):
        raise ValueError("Gingerbread Joyride's cupcakes are incomplete")
    order = list(PACKAGE_TYPES)
    return [found[p] for p in sorted(found, key=lambda p: (order.index(p.split('/')[0]), p))]


def with_package_files(blob, paths):
    """The level's package (its igStreamingChunkInfo: (kind, path) pairs, loaded in order) with paths
    added before its maps."""
    g = igz_file.Igz(blob)
    chunk = next(off for off, kind in g.objects() if kind == 'igStreamingChunkInfo')
    count, items = g.u32(chunk + 0x28), g.u64(chunk + 0x38)
    pairs = [(g.u64(items + 16 * i), g.u64(items + 16 * i + 8)) for i in range(count)]
    maps = next(i for i, (kind, _path) in enumerate(pairs) if g.strings[kind] == 'igx_entities')
    known = {g.strings[p] for _kind, p in pairs}
    new = [(g.string_index(PACKAGE_TYPES[p.split('/')[0]]), g.string_index(p.lower()))
           for p in paths if p.lower() not in known]
    pairs = pairs[:maps] + new + pairs[maps:]
    raw = b''.join(struct.pack('<QQ', kind, path) for kind, path in pairs)
    at = g.append(raw)
    for i in range(2 * len(pairs)):
        g.add_site('RSTT', at + 8 * i)
    g.set_u32(chunk + 0x28, len(pairs))
    g.set_u64(chunk + 0x30, (g.u64(chunk + 0x30) & ~0xFFFFFFFF) | len(raw))
    g.set_u64(chunk + 0x38, at)
    return g.build()


def name_hash(name):
    h = 0x811C9DC5
    for c in name.lower().encode('utf-8'):
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h


def with_decoration(blob, models, snow=None, layout=None):
    """The title's map with an entity for each DECORATION, copies of its colour backdrop's entity
    (an entity, its data, component table and list, and transform: a model alone) with the model,
    place, turn and size changed, and, with snow (an effect's name), a copy of GP7's embers entity
    (a looping effect alone) showing it. Copies are made visible (the originals are hidden until
    their Grand Prix: bit 0 of the entity's flags). Places are offsets in world units from
    the logo's place."""
    g = igz_file.Igz(blob)
    g.sites['RHND'] = g.read_list('RHND', igz_file.unpack_offsets)
    objects = g.objects()
    kinds = dict(objects)
    starts = sorted(kinds)
    root = next(o for o, kind in objects if kind == 'igObjectList')
    names = next(o for o, kind in objects if kind == 'igNameList')
    listed = [g.u64(g.u64(root + 0x20) + 8 * i) for i in range(g.u32(root + 0x0C))]
    named = [bytes(g.data[g.u64(names + 0x20) + 16 * i:g.u64(names + 0x20) + 16 * i + 16])
             for i in range(g.u32(names + 0x0C))]
    name_of = {o: g.strings[struct.unpack_from('<Q', n)[0]] for o, n in zip(listed, named)}
    entity_of = {name: o for o, name in name_of.items()}

    def model_of(entity, field):
        return g.string_at(g.u64(entity + 0x40) + field).replace('\\', '/').lower()

    def copy(start, end, label):
        """A copy of the objects in [start, end) (fixups, inner references and root list names
        with it), visible: the copy's start."""
        span = bytes(g.data[start:end])
        while len(g.data) % 16 != start % 16:
            g.data.append(0)
        at = g.append(span)
        moved = at - start
        for tag in ('RVTB', 'RSTT', 'ROFS', 'RPID', 'RHND'):
            for site in [s for s in g.sites[tag] if start <= s < end]:
                g.add_site(tag, site + moved)
        for site in [s for s in g.sites['ROFS'] if at <= s < at + len(span)]:
            if start <= g.u64(site) < end:
                g.set_u64(site, g.u64(site) + moved)
        prefix = name_of[start]
        for o in [o for o in listed if start <= o < end]:
            name = label + name_of[o][len(prefix):]
            listed.append(o + moved)
            named.append(struct.pack('<QII', g.string_index(name), name_hash(name), 0))
        g.set_u64(at + 0x38, g.u64(at + 0x38) | 1)
        return at

    entities = [o for o, kind in objects if kind == 'CGameEntity']
    template = next(e for e in entities if model_of(e, 0x48).endswith('/octane_ui_custo_bg_color_1p'))
    logo = next(e for e in entities if model_of(e, 0x50).endswith('/crash_mainmenu'))
    transform = g.u64(template + 0x30)
    end = transform + g.sizes[g.types.index('igEntityTransform')]
    inside = [kinds[o] for o in starts if template <= o < end]
    if inside != ['CGameEntity', 'CGameEntityData', 'igComponentDataTable', 'igComponentList', 'igEntityTransform']:
        raise ValueError('unexpected backdrop entity in the title map')
    target = struct.unpack_from('<3f', g.data, logo + 0x20)
    cupcakes, snow_spots = layout or (DECORATION, SNOW_SPOTS)
    for n, (model, dx, dy, dz, size, turn) in enumerate(cupcakes, 1):
        at = copy(template, end, f'Christmas_Decoration_{n}')
        place = (target[0] + dx, target[1] + dy, target[2] + dz)
        struct.pack_into('<3f', g.data, at + 0x20, *place)
        moved_transform = g.u64(at + 0x30)
        struct.pack_into('<3f', g.data, moved_transform + 0x60, 0.0, 0.0, turn)
        struct.pack_into('<3f', g.data, moved_transform + 0x6C, size, size, size)
        g.set_u64(g.u64(at + 0x40) + 0x48,
                  g.string_index('\\'.join(models[model].split('/')[1:]).rsplit('.', 1)[0]))
    if snow:
        # GP7's embers: an entity, its data, component table, looping effect and component list,
        # up to the next object outside them; the effect by its external id (EXID: the hashes of
        # its name and namespace, both the effect file's name).
        embers = entity_of['gp7_embers_vfx']
        after = [o for o in starts if o > embers and kinds[o] in ('CGameEntity', 'CPlayerStartEntity', 'CWorldEntity')]
        end = after[0] if after else len(g.data)
        effects = [o for o in starts if embers <= o < end and kinds[o] == 'CLoopingVfxComponentData']
        if len(effects) != 1:
            raise ValueError('unexpected embers entity in the title map')
        exid = g.fixup('EXID')
        index = exid[1]
        exid[3] += struct.pack('<II', name_hash(snow), name_hash(snow))
        exid[1] += 1
        exid[4] = exid[2] + len(exid[3])
        exid[4] += (-exid[4]) % 4
        for n, (dx, dy, dz) in enumerate(snow_spots, 1):
            at = copy(embers, end, f'Christmas_Snow_{n}')
            struct.pack_into('<3f', g.data, at + 0x20, target[0] + dx, target[1] + dy, target[2] + dz)
            g.set_u64(effects[0] - embers + at + 0x18, index)
    for lst, raw, step, tag in ((root, b''.join(struct.pack('<Q', o) for o in listed), 8, 'ROFS'),
                                (names, b''.join(named), 16, 'RSTT')):
        at = g.append(raw)
        for i in range(len(raw) // step):
            g.add_site(tag, at + step * i)
        g.set_u32(lst + 0x0C, len(raw) // step)
        g.set_u32(lst + 0x10, len(raw) // step)
        g.set_u64(lst + 0x18, (g.u64(lst + 0x18) & ~0xFFFFFFFF) | len(raw))
        g.set_u64(lst + 0x20, at)
    return g.build()


def find(archive, name):
    for f in archive.files:
        if name in f['path'].replace('\\', '/').lower():
            return f
    return None


def decoration_layout(config):
    """((cupcakes), (snow spots)) from LAYOUT_FILE beside bbport.ini when it is there (to place them
    by hand), else the built-in ones; and the file's text (to make the patches again when it
    changes). A broken file is reported and the built-in places are used."""
    path = Path(config).parent / LAYOUT_FILE if config else None
    if not path or not path.is_file():
        return None, ''
    text = path.read_text(encoding='utf-8', errors='replace')
    try:
        data = json.loads(text)
        cupcakes = tuple((int(c.get('cupcake', 1)) - 1, float(c['x']), float(c['y']), float(c['z']),
                          float(c.get('size', 1.0)), float(c.get('turn', 0.0)))
                         for c in data.get('cupcakes', []))
        if any(not 0 <= c[0] < len(CUPCAKES) or not 0 < c[4] <= 20 for c in cupcakes):
            raise ValueError('cupcake must be 1 or 2, size between 0 and 20')
        snow = tuple((float(x), float(y), float(z)) for x, y, z in data.get('snow', SNOW_SPOTS))
        if len(cupcakes) + len(snow) > 64:
            raise ValueError('at most 64 cupcakes and snow spots')
        return (cupcakes, snow), text
    except (ValueError, TypeError, KeyError, AttributeError) as error:
        say(f'{LAYOUT_FILE} could not be read ({error}); the built-in places')
        return None, text


def prepare(game, out, config, env, base):
    """{guest archive: patch file} of the chosen title theme (none for the game's own), each keeping
    the patch base ({guest: patch file}) has for the same archive."""
    theme = chosen_theme(config, env)
    crash = chosen_crash(config, env, theme)
    if not theme and not crash:
        return {}
    from mod_file_paths import resolve
    folder = Path(out) / 'title_theme'
    names = sorted(set(GRADIENT_ARCHIVES + ICON_ARCHIVES + TREE_ARCHIVES + tuple(BACKDROPS) + (CRASH_ARCHIVE,)))
    archives = {name: resolve(Path(game) / 'archives' / name) for name in names}
    archives = {name: path for name, path in archives.items() if path.is_file()}
    layout, layout_text = decoration_layout(config)
    stamp_info = {'theme': theme, 'crash': crash, 'layout': layout_text,
                  'script': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  'archive_patch': hashlib.sha256(Path(archive_patch.__file__).read_bytes()).hexdigest(),
                  'archives': {n: [p.stat().st_size, p.stat().st_mtime_ns] for n, p in archives.items()},
                  'base': {g: hashlib.sha256(Path(p).read_bytes()).hexdigest() for g, p in sorted(base.items())}}
    stamp_file = folder / 'stamp.json'
    try:
        made = json.loads(stamp_file.read_text(encoding='utf-8'))
        if made['stamp'] == stamp_info and all(Path(p).is_file() for p in made['patches'].values()):
            return {g: Path(p) for g, p in made['patches'].items()}
    except (OSError, ValueError, KeyError):
        pass
    trees = None
    for name in TREE_ARCHIVES if theme else ():
        if name in archives:
            archive = Archive(archives[name])
            entry = find(archive, TREES)
            if entry:
                trees = bytes(archive.extract(entry))
    if theme and trees is None:
        raise ValueError('the Gingerbread Joyride trees are not in the archives')
    # The cupcakes' files, added to the title's archive (whose index is written again, so it may
    # have no other patch); its map and package then list them.
    decoration = None
    title_guest = f'/app0/archives/{CRASH_ARCHIVE}'
    if theme and 'update2.pak' in archives and CRASH_ARCHIVE in archives and title_guest not in base:
        try:
            decoration = cupcake_files(archives['update2.pak'], Archive(archives[CRASH_ARCHIVE]))
        except ValueError as error:
            say(f'no decoration: {error}')
    folder.mkdir(parents=True, exist_ok=True)
    for old in folder.glob('*.bbpatch'):
        old.unlink()
    patches = {}
    for name, path in archives.items():
        archive = Archive(path)
        entries, added = {}, {}
        guest = f'/app0/archives/{name}'
        if theme and name in GRADIENT_ARCHIVES:
            entry = find(archive, GRADIENT)
            if entry:
                entries[entry['index']] = christmas_gradient(bytes(archive.extract(entry)))
        if theme and name in ICON_ARCHIVES:
            entry = find(archive, ICONS)
            if entry:
                entries[entry['index']] = with_image_of(bytes(archive.extract(entry)), trees)
        for texture in BACKDROPS.get(name, ()) if theme else ():
            entry = find(archive, texture)
            if entry:
                entries[entry['index']] = christmas_backdrop(bytes(archive.extract(entry)))
        if name == CRASH_ARCHIVE and (crash or theme):
            # Its index is written again, so no other patch may share this archive.
            if guest in base:
                say(f'{name} has another patch already; the title keeps its Crash and decoration')
            elif 'update2.pak' not in archives:
                say("update2.pak is missing; the title keeps its Crash and decoration")
            else:
                if crash:
                    more, added = reindeer_crash(archives['update2.pak'], archive)
                    entries.update(more)
                added.update(dict(decoration or ()))
        if decoration and name == 'update2.pak':
            # The title's map and package as 1.21 loads them (update2.pak's).
            try:
                models = {i: next(p for p, _ in decoration if p.lower() == c) for i, c in enumerate(CUPCAKES)}
                world, package = find(archive, TITLE_MAP), find(archive, TITLE_PACKAGE)
                snow = find(archive, SNOW_FILE)
                loads = [p for p, _ in decoration]
                if snow:
                    loads += dependencies(bytes(archive.extract(snow))) + [SNOW_FILE]
                entries[world['index']] = with_decoration(bytes(archive.extract(world)), models,
                                                          SNOW_EFFECT if snow else None, layout)
                entries[package['index']] = with_package_files(bytes(archive.extract(package)), loads)
            except (ValueError, KeyError, StopIteration, TypeError) as error:
                say(f'no decoration: {error}')
        if not entries:
            continue
        target = folder / (path.stem + '.bbpatch')
        if added:
            archive_patch.write_grown_patch(target, path, guest, entries, added)
        else:
            archive_patch.write_entries_patch(target, path, guest, entries, base.get(guest))
        patches[guest] = target
    stamp_file.write_text(json.dumps({'stamp': stamp_info, 'patches': {g: str(p) for g, p in patches.items()}}),
                          encoding='utf-8')
    say(', '.join(filter(None, [theme and f'{theme} title screen', crash and f'{crash} Crash']))
        + f' in {len(patches)} archives')
    return patches


def main():
    p = argparse.ArgumentParser(description='Archive patches for a seasonal title screen.')
    p.add_argument('game')
    p.add_argument('--out', required=True)
    p.add_argument('--settings', help='bbport.ini (title_theme=)')
    p.add_argument('--theme', choices=THEMES, help='instead of the settings')
    p.add_argument('--crash', choices=CRASHES, help='instead of the settings')
    a = p.parse_args()
    env = dict(os.environ, **({'BB_TITLE_THEME': a.theme} if a.theme else {}),
               **({'BB_TITLE_CRASH': a.crash} if a.crash else {}))
    for patch in prepare(a.game, a.out, a.settings, env, {}).values():
        print(patch)


if __name__ == '__main__':
    main()
