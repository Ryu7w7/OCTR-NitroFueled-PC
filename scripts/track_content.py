#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""New race tracks as the port's own content: added to the game's track lists, not over another track.

A track package (content/tracks/<id>/, track.json beside its files) brings the level's own archive
(archives/t<NNN>_<name>.pak, the level as the author built it) and the archive its author shipped to
register it (archives/update3.pak in Neo Bowser City's mod). That archive is not used as it is: it
carries whole copies of game files (the track lists, the menus, sound banks, common race scripts)
that would hide the port's own changes, those of every other track, and the Options menu's Graphics
tab (scripts/graphics_tab.py serves the port's update3.pak; a mod's own update3.pak turns it off).
From it this takes only:

  - the track's entries in the game's three track lists, copied object by object into the player's
    own newest copy of each (update2.pak, so USA and EU alike); in track select the new tracks have
    their own area, "Power Charged", after the game's race areas (its title in every language):
      loosedata/guisystemdata.igz          a CGuiOctaneLevelData (the level's name)
      ui/octane_levelselect_proto.igz      a CGuiOctaneLevelSelectData (its button in track select)
      packages/generated/maps/chunkinfos_pkg.igz   the files the menus load for it (its zone info...)
  - the files track.json lists ("files"): the zone info, its menu picture, data its scripts read, and
    any menu script the track needs (Neo Bowser City has no preview movie: its own track select
    script shows a picture instead);
  - its name in the game's languages: zone texts of its own (<zone>_zoneInfo_<lang>.lng), copies of
    the zone it was built from ("name_source") with "names" in them, and its zone info pointed at
    them. Built from another track's zone, it otherwise shows that track's name everywhere (menus,
    the Discord presence, the server's records).

The result is a list of archive entries that graphics_tab.py puts in the port's update3.pak (the
game loads it after update2.pak); scripts/mods.py adds each package's level archive to the game's
view. Pure Python, the player's own files: Windows and Linux, USA and EU 1.21 alike.

track.json:
  {"id": "neo_bowser_city", "level": "octane/t929_neo_bowser_city/t929_neo_bowser_city",
   "archives": ["archives/t929_neo_bowser_city.pak"], "registration": "archives/update3.pak",
   "files": [...logical paths in the registration archive...],
   "name_source": "t114_sewer_speedway", "names": {"en": "...", "mx": "...", "fr": "...", "pt": "..."}}

usage: track_content.py GAME --check   (prints what would be added)
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from graphics_tab import Archive  # noqa: E402
from igz_file import Igz  # noqa: E402
from racer_registry import Copier, File  # noqa: E402

GUI = 'loosedata/guisystemdata.igz'
SELECT = 'ui/octane_levelselect_proto.igz'
CHUNKS = 'packages/generated/maps/chunkinfos_pkg.igz'
# Where the game's newest copy of each is (the first archive that has it).
NEWEST = ('update2.pak', 'update.pak', 'juicedomain_permanent.pak', 'chunkinfos.pak')
LISTS = {GUI: ('CGuiOctaneLevelData', 'CGuiOctaneAreaLevelList'),
         SELECT: ('CGuiOctaneLevelSelectData', 'CGuiOctaneLevelSelectDataList')}
PREFIX = 'temporary/octane/data/ps4/output/'
ID = re.compile(r'[a-z0-9_]{1,40}\Z')


def content_root():
    """The port's content folder: BB_CONTENT_DIR, else content/ beside scripts/."""
    return Path(os.environ.get('BB_CONTENT_DIR') or Path(__file__).resolve().parent.parent / 'content')


def packages(root=None):
    """[(folder, manifest)] of the track packages, in name order; a broken one is skipped (said)."""
    found = []
    tracks = Path(root or content_root()) / 'tracks'
    if not tracks.is_dir():
        return found
    for folder in sorted(p for p in tracks.iterdir() if p.is_dir()):
        try:
            manifest = json.loads((folder / 'track.json').read_text(encoding='utf-8'))
            check(folder, manifest)
            found.append((folder, manifest))
        except (OSError, ValueError, KeyError, TypeError) as error:
            print(f'Tracks: {folder.name} skipped ({error})')
    return found


def check(folder, m):
    if not ID.match(m['id']):
        raise ValueError('bad id')
    if not re.fullmatch(r'octane/t\d{3}_[a-z0-9_]+/t\d{3}_[a-z0-9_]+', m['level']):
        raise ValueError('bad level')
    for path in list(m['archives']) + [m['registration']] + [m[k] for k in ('icon', 'background', 'preview') if m.get(k)]:
        if '..' in Path(path).parts or not (folder / path).is_file():
            raise ValueError(f'missing {path}')


def zone(m):
    """The level's zone name (t929_neo_bowser_city)."""
    return m['level'].split('/')[-1]


def archive_entries(path):
    """{logical (lower case): (full, logical, bytes)} of an archive."""
    archive = Archive(Path(path))
    try:
        return {archive.logical(i).lower(): (*archive.names[i], archive.read(i))
                for i in range(len(archive.names))}
    finally:
        archive.f.close()


def newest(game, wanted):
    """{logical (lower case): (full, logical, bytes)}: the game's newest copy of each wanted file
    (wanted: a set of lower-case logical names, or a pattern they match)."""
    if hasattr(wanted, 'match'):
        pattern = wanted
        wanted = type('Matching', (), {'__contains__': lambda _self, low: bool(pattern.match(low))})()
    found = {}
    from mod_file_paths import extra_updates
    for name in list(reversed(extra_updates(game))) + list(NEWEST):   # a track mod's own lists first
        path = Path(game) / 'archives' / name
        if not path.is_file():
            continue
        archive = Archive(path)
        try:
            for i in range(len(archive.names)):
                low = archive.logical(i).lower()
                if low in wanted and low not in found:
                    found[low] = (*archive.names[i], archive.read(i))
        finally:
            archive.f.close()
    return found


def level_objects(z, kind, level):
    return [o for o, t in z.typed.items() if t == kind and z.string_at(o + 0x10).lower() == level]


def lists_of(z, kind):
    return [o for o in z.starts if z.typed.get(o) == kind]


# The new tracks' own area in track select, after the game's race areas, in both lists:
# (list of areas' type, its owner's type and field, an area's type, the area's name). The menu shows an
# area's title by its name (CGuiOctaneLevelSelectDataList::_categoryName in the menu's text files).
AREA_TITLE = 'Power Charged'
AREAS = {GUI: ('CGuiOctaneAreaLevelListList', 'COctaneGuiSystemData', 0x2E0, 'CGuiOctaneAreaLevelList',
               'GuiSystemData_raceAreaLevelDataList_PowerCharged'),
         SELECT: ('CGuiOctaneLevelSelectDataListList', 'CGuiListItemPopulatorOctaneLevelSelect', 0x18,
                  'CGuiOctaneLevelSelectDataList', 'LevelList_listItemPopulator_levelData_PowerCharged')}
MENU_TEXTS = re.compile(r'ui/octane_levelselect_proto_([a-z]+)\.lng\Z')
# The same area's title elsewhere (the leaderboards' Zone): guisystemdata's own texts, by its area name.
GUI_TEXTS = re.compile(r'loosedata/guisystemdata_([a-z]+)\.lng\Z')
TITLE_FIELDS = {SELECT: 'CGuiOctaneLevelSelectDataList::_categoryName', GUI: 'CGuiOctaneAreaLevelList::_areaName'}


def copy_level(t, source, level, item):
    """The level's object of source (bytes) copied into t (a File): its offset in t."""
    s = File(source)
    found = level_objects(s, item, level)
    if len(found) != 1:
        raise ValueError(f'{level}: {len(found)} {item} in the registration')
    return Copier(s, t).object(found[0])


def add_area(t, path, items):
    """t (a File) with the new tracks' area (AREAS[path]) holding items, after the race areas."""
    listlist_kind, owner_kind, field, area_kind, name = AREAS[path]
    if name in {n for n, _o in t.roots()[2]}:
        raise ValueError(f'{path} has a {name} already')
    owner = t.find(owner_kind)
    areas = t.ref(owner + field)
    if t.typed.get(areas) != listlist_kind:
        raise ValueError(f'{path}: no {listlist_kind} at {owner_kind}+{field:#x}')
    template = t.items(areas)[0]
    size = t.size(template)
    new = t.append(bytes(t.data[template:template + size]))
    t.add_site('RVTB', new)
    t.typed[new] = area_kind
    t.set_u32(new + 0xC, 0)
    t.set_u32(new + 0x10, 0)
    t.set_u64(new + 0x18, t.u64(new + 0x18) & ~0xFFFFFFFF)
    t.set_u64(new + 0x20, t.append(b''))
    t.add_site('ROFS', new + 0x20)
    t.grow(new, items)
    t.grow(areas, [new])
    root, names, _ = t.roots()
    t.grow(root, [new])
    t.grow(names, [name], 16)


def with_area_title(lng, title_text=AREA_TITLE, path=SELECT):
    """A text file with the new area's title (a copy of another area's, renamed): the track select's
    (path SELECT) or guisystemdata's (GUI, the leaderboards' Zone)."""
    z = File(lng)
    lst = z.find('igLocalizedStringDataList')
    name = AREAS[path][4]
    titles = [o for o in z.items(lst) if z.handle(z.u32(o + 0x20))[0] == TITLE_FIELDS[path]]
    if not titles:
        raise ValueError('no area titles in the track select texts')
    if any(z.handle(z.u32(o + 0x18))[0] == name for o in titles):
        return lng
    template = titles[0]
    size = z.size(template)
    new = z.append(bytes(z.data[template:template + size]))
    z.add_site('RVTB', new)
    for tag, site in list(z.sites_in(template, size)):
        z.add_site(tag, new + site - template)
    z.set_u64(new + 0x10, z.string_index(title_text))
    value = z.u32(template + 0x18)
    _old, space = z.handle(value)
    z.set_u32(new + 0x18, z.handle_of(name, space, bool(value & 0x80000000)))
    z.grow(lst, [new])
    return z.build()


def chunk_pairs(blob):
    g = Igz(blob)
    chunk = next(off for off, kind in g.objects() if kind == 'igStreamingChunkInfo')
    count, items = g.u32(chunk + 0x28), g.u64(chunk + 0x38)
    return g, chunk, [(g.strings[g.u64(items + 16 * i)], g.strings[g.u64(items + 16 * i + 8)]) for i in range(count)]


def add_chunks(base, pairs):
    """The global package with (kind, path) pairs added at its end, those it already has skipped."""
    g, chunk, have = chunk_pairs(base)
    known = {p.lower() for _k, p in have}
    new = [(k, p.lower()) for k, p in pairs if p.lower() not in known]
    if not new:
        return base
    count, items = g.u32(chunk + 0x28), g.u64(chunk + 0x38)
    raw = bytes(g.data[items:items + 16 * count])
    raw += b''.join(struct.pack('<QQ', g.string_index(k), g.string_index(p)) for k, p in new)
    at = g.append(raw)
    for i in range(2 * (count + len(new))):
        g.add_site('RSTT', at + 8 * i)
    g.set_u32(chunk + 0x28, count + len(new))
    g.set_u64(chunk + 0x30, (g.u64(chunk + 0x30) & ~0xFFFFFFFF) | len(raw))
    g.set_u64(chunk + 0x38, at)
    return g.build()


def title(zone_name):
    """A zone's name as the game spells its folders and text keys: T929_Neo_Bowser_City."""
    return '_'.join(w.upper() if re.fullmatch(r't\d{3}', w) else w.capitalize() for w in zone_name.split('_'))


def lng_path(zone_name, lang, source_logical):
    """The zone's text file, spelled as the source track's is (maps/Octane/T114_.../T114_..._zoneInfo_en.lng)."""
    head = source_logical.rsplit('/', 2)[0]
    return f'{head}/{title(zone_name)}/{title(zone_name)}_zoneInfo_{lang}.lng'


def renamer(old, new):
    """Zone text names only (strings with "zoneInfo": the text files' keys, names and paths), old -> new;
    the source track's other names (its minimap, icons) stay."""
    def rename(s):
        return re.sub(re.escape(old), new, s, flags=re.I) if 'zoneinfo' in s.lower() else s
    return rename


def with_name(lng, name, rename):
    """A zone text file with its display name (its first string) replaced and its key renamed."""
    g = Igz(lng)
    if not g.strings or g.strings[0].strip() in ('', 'Default'):
        raise ValueError('a zone text file without a name')
    g.strings = [rename(s) for s in g.strings]
    g.strings[0] = name
    return g.build()


def with_texts(zoneinfo, rename, texts):
    """The zone info pointed at its own text files: its strings renamed, and the text files it loads
    (TDEP) those of the player's edition (texts: their logical paths; the author's edition may have had
    other languages)."""
    z = File(zoneinfo)
    z.strings = [rename(s) for s in z.strings]
    pairs = [(z.depends[i], z.depends[i + 1]) for i in range(0, len(z.depends) - 1, 2)]
    kept = [(n, p) for n, p in pairs if not (n.lower().find('_zoneinfo_') >= 0 and p.lower().endswith('.lng'))]
    own = [(path.rsplit('/', 1)[1][:-4], f'<build>project:/{PREFIX}{path}') for path in texts]
    z.depends = [s for pair in kept + own for s in pair]
    return z.build()


def visual_dependencies(blob):
    """The textures and materials an IGZ file loads first (its TDEP), as archive paths (lower case)."""
    try:
        depends = File(blob).depends
    except (ValueError, KeyError, IndexError, struct.error):
        return []
    out = []
    for path in depends[1::2]:
        kind, sep, rest = path.partition(':/')
        kind = {'materials': 'materialinstances'}.get(kind, kind)
        if sep and kind in ('textures', 'materialinstances'):
            out.append(f'{kind}/{rest}'.lower())
    return out


def read_png(path):
    """(width, height, RGBA bytes) of an 8-bit RGBA or RGB PNG (not interlaced)."""
    import zlib
    d = Path(path).read_bytes()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{Path(path).name} is not a PNG')
    at, idat, head = 8, b'', None
    while at + 8 <= len(d):
        n, = struct.unpack('>I', d[at:at + 4])
        kind, body = d[at + 4:at + 8], d[at + 8:at + 8 + n]
        if kind == b'IHDR':
            head = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        at += 12 + n
    if head is None:
        raise ValueError(f'{Path(path).name}: no PNG header')
    w, h, depth, ctype, _c, _f, interlace = head
    if depth != 8 or ctype not in (2, 6) or interlace:
        raise ValueError(f'{Path(path).name}: an 8-bit RGBA PNG, not interlaced, please')
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride, out, prev = w * bpp, bytearray(), bytearray(w * bpp)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b, c = prev[x], (prev[x - bpp] if x >= bpp else 0)
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        out += line if bpp == 4 else b''.join(bytes(line[i:i + 3]) + b'\xff' for i in range(0, stride, 3))
    return w, h, bytes(out)


def bc3_icon(width, height, rgba):
    """A white icon's BC3 blocks in the PS4's 1D-thin tiling (8x8 blocks, Morton order inside), as
    the game's track icons are: the colour white, the PNG's alpha kept (8 levels per 4x4 block), its
    rows from the bottom up."""
    import season_art
    across, down = season_art.block_grid(width, height)
    out = bytearray(across * down * 16)
    white = struct.pack('<HHI', 0xFFFF, 0xFFFF, 0)
    for by in range((height + 3) // 4):
        for bx in range((width + 3) // 4):
            alphas = []
            for i in range(16):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                # the game's textures run from the bottom row up (the first icon came out upside down)
                alphas.append(rgba[((height - 1 - y) * width + x) * 4 + 3] if x < width and y < height else 0)
            hi, lo = max(alphas), min(alphas)
            if hi == lo:
                table, block = [hi] * 8, bytes([hi, lo])
            else:
                table = [hi, lo] + [((7 - i) * hi + i * lo) // 7 for i in range(1, 7)]
                block = bytes([hi, lo])
            bits = 0
            for i, a in enumerate(alphas):
                bits |= min(range(8), key=lambda k: abs(table[k] - a)) << (3 * i)
            at = season_art.tiled_index(bx, by, across) * 16
            out[at:at + 16] = block + bits.to_bytes(6, 'little') + white
    return bytes(out)


def with_icon(files, m, folder):
    """files with the track's own track-select icon (track.json "icon": a 100x100 white PNG on
    transparency): a copy of the icon its menu material used (another track's), its picture and name
    changed, and that material pointed at it. ([('texture', path)] for the global package.)"""
    import season_art
    png = folder / m['icon']
    width, height, rgba = read_png(png)
    for low, (full, logical, data) in list(files.items()):
        if not low.startswith('materialinstances/'):
            continue
        mat = File(data)
        old = next((d for d in mat.depends[0::2] if '_raceselect_ico_' in d.lower()), None)
        if old is None:
            continue
        source = next((k for k in files if k.startswith('textures/') and old.lower() in k), None)
        if source is None:
            raise ValueError(f'{m["id"]}: the icon {old} is not among its files')
        track = re.sub(r'[^a-z0-9]', '', m['id'].lower())
        icon_name = re.sub(r'(_raceselect_ico_)[a-z0-9]+', lambda g: g.group(1) + track, old, flags=re.I)
        tfull, tlogical, tdata = files[source]
        w, h, at, size, levels = season_art.image(tdata)
        if (w, h, levels) != (width, height, 1):
            raise ValueError(f'{m["id"]}: the icon must be {w}x{h} (it is {width}x{height})')
        pixels = bc3_icon(width, height, rgba)
        if len(pixels) != size:
            raise ValueError(f'{m["id"]}: unexpected icon format')
        texture = tdata[:at] + pixels + tdata[at + size:]
        src = re.search(r'_raceselect_ico_([a-z0-9]+)', old, re.I).group(1)
        if len(track) <= len(src):   # its source file's name, a label only: renamed when it fits
            texture = season_art.with_strings_renamed(texture, src, track)
        path = re.sub(r'(_raceselect_ico_)[a-z0-9]+', lambda g: g.group(1) + track, tlogical, flags=re.I)
        files[path.lower()] = (PREFIX + path, path, texture)
        mat.strings = [s.replace(old, icon_name) for s in mat.strings]
        mat.depends = [s.replace(old, icon_name) for s in mat.depends]
        files[low] = (full, logical, mat.build())
        return files, [('texture', path.lower())]
    raise ValueError(f'{m["id"]}: no menu material with an icon')


def bc1_block(pixels):
    """A colour block (4-colour mode) for 16 RGB pixels: the ends from their bounding box, pulled in a
    sixteenth (the usual fast DXT fit), each pixel the nearest of the 4 colours."""
    import season_art
    lo = [min(p[c] for p in pixels) for c in range(3)]
    hi = [max(p[c] for p in pixels) for c in range(3)]
    inset = [(h - l) / 16 for l, h in zip(lo, hi)]
    c0 = season_art.to565([h - i for h, i in zip(hi, inset)])
    c1 = season_art.to565([l + i for l, i in zip(lo, inset)])
    if c0 == c1:
        return struct.pack('<HHI', c0, c1, 0)
    if c0 < c1:
        c0, c1 = c1, c0
    palette = season_art.colour_palette(c0, c1)
    bits = 0
    for i, p in enumerate(pixels):
        best = min(range(4), key=lambda k: (palette[k][0] - p[0]) ** 2 + (palette[k][1] - p[1]) ** 2
                   + (palette[k][2] - p[2]) ** 2)
        bits |= best << (2 * i)
    return struct.pack('<HHI', c0, c1, bits)


def bc3_picture(width, height, rgba):
    """An opaque picture's BC3 blocks in the PS4's 1D-thin tiling (8x8 blocks, Morton order inside),
    rows from the bottom up, as the game's track select pictures are (octane_ui_raceselect_bg_*: Crash
    Cove's sky is in its last rows)."""
    import season_art
    across, down = season_art.block_grid(width, height)
    out = bytearray(across * down * 16)
    opaque = bytes([255, 255]) + bytes(6)
    for by in range((height + 3) // 4):
        for bx in range((width + 3) // 4):
            pixels = []
            for i in range(16):
                x, y = min(bx * 4 + i % 4, width - 1), min(by * 4 + i // 4, height - 1)
                at = ((height - 1 - y) * width + x) * 4
                pixels.append(rgba[at:at + 3])
            at = season_art.tiled_index(bx, by, across) * 16
            out[at:at + 16] = opaque + bc1_block(pixels)
    return bytes(out)


BACKGROUND = re.compile(r'textures/guistandard_diffuse,textures@!ui!octane!raceselect!'
                        r'octane_ui_raceselect_bg_([a-z0-9]+)`tga,101\.igz\Z')
BACKGROUND_ARCHIVES = ('juicedomain_startscreen.pak', 'juicedomain_onlinemenus.pak')


def game_background(game, m):
    """The game's own track select picture of the track this one was built from (track.json
    "name_source": Sewer Speedway's octane_ui_raceselect_bg_sewerspeedway), as a template."""
    want = re.sub(r'^t\d{3}_|_', '', m.get('name_source', '').lower())
    for name in BACKGROUND_ARCHIVES:
        path = Path(game) / 'archives' / name
        if not path.is_file():
            continue
        archive = Archive(path)
        try:
            for i in range(len(archive.names)):
                g = BACKGROUND.match(archive.logical(i).lower())
                if g and g[1] == want:
                    return archive.read(i)
        finally:
            archive.f.close()
    raise ValueError(f'{m["id"]}: the game has no track select picture of {m.get("name_source")}')


def with_background(files, m, folder, game):
    """files with the track select's picture (track.json "background": a PNG of the game's size,
    1288x1080, also the online room's picture of the track) put in place of the one its files have.
    Written as the game's own are (BC3, rows from the bottom up, in a copy of the source track's
    picture): Neo Bowser City's author stored it from the top down (the online room showed it upside
    down), as uncompressed RGBA in a BC3 picture's file."""
    import season_art
    width, height, rgba = read_png(folder / m['background'])
    template = game_background(game, m)
    w, h, at, size, levels = season_art.image(template)
    if (w, h) != (width, height):
        raise ValueError(f'{m["id"]}: the background must be a {w}x{h} PNG (it is {width}x{height})')
    pixels = bc3_picture(width, height, rgba)
    if len(pixels) != size or levels != 1:
        raise ValueError(f'{m["id"]}: unexpected track select picture format')
    for low, (full, logical, data) in list(files.items()):
        if low.startswith('textures/') and '_raceselect_bg_' in low and '_pattern_' not in low:
            files[low] = (full, logical, template[:at] + pixels + template[at + size:])
            return files
    raise ValueError(f'{m["id"]}: no track select background among its files')


MOVIE = re.compile(r'octane_ui_race_select_[a-z0-9_]+', re.I)
ZONE_MOVIE = 0x78   # CZoneInfo: its track select movie's name (archives/<name>.bk2)


def movie_name(m):
    """The track's preview movie's name (its file archives/<name>.bk2), from track.json "preview"."""
    return Path(m['preview']).stem if m.get('preview') else None


def with_movie(zoneinfo, name):
    """The zone info's preview movie (the track select's looping video, archives/<name>.bk2) renamed:
    a track built from another zone plays that zone's otherwise."""
    z = File(zoneinfo)
    old = next((s for s in z.strings if MOVIE.fullmatch(s)), None)
    if old is not None:
        z.strings = [s.replace(old, name) for s in z.strings]
        z.depends = [s.replace(old, name) for s in z.depends]
    # CZoneInfo +0x78 names the movie the track select plays (Sewer Speedway's
    # "octane_ui_race_select_sewer_speedway"); Neo Bowser City's author left it empty for a still
    # picture, and then the game asks for no movie at all.
    info = z.find('CZoneInfo')
    if ('RSTT', info + ZONE_MOVIE) not in set(z.sites_in(info + ZONE_MOVIE, 8)):
        raise ValueError('unexpected zone info (no movie name)')
    z.set_u64(info + ZONE_MOVIE, z.string_index(name))
    return z.build()


def zone_info_path(m):
    return f'maps/octane/{zone(m)}/{zone(m)}_zoneinfo.igz'


# The leaderboards a track's records go to: its zone info's three CStatisticData (+0xc), the Time Trial
# time and lap (server boards 1000 + id) and the Ring Challenge (id). A track built from another zone
# keeps that zone's (Neo Bowser City: Sewer Speedway's 7, 8, 92: its times went on Sewer Speedway's
# board). Fixed here, not in track.json, so a package cannot post on another track's board; ids no
# zone of the game uses (65..76 and 130..133: their boards are empty on the servers).
STATISTICS = {
    't929_neo_bowser_city': (65, 66, 130),
}
STATISTIC_ID = 0xc


def with_statistics(zoneinfo, ids):
    z = Igz(zoneinfo)
    stats = [off for off, t in z.objects() if t == 'CStatisticData']
    if len(stats) != len(ids):
        raise ValueError(f'unexpected zone info ({len(stats)} statistics)')
    out = bytearray(zoneinfo)
    base = z.sections[1][1]
    for off, value in zip(stats, ids):
        struct.pack_into('<I', out, base + off + STATISTIC_ID, value)
    return bytes(out)


def own_files(game, m, reg, folder=None):
    """own_texts_and_files(), and the zone info pointed at the track's own preview movie when track.json
    has one ("preview") and at its own leaderboards (STATISTICS)."""
    files, texts = own_texts_and_files(game, m, reg, folder)
    zinfo = zone_info_path(m)
    if movie_name(m) and zinfo in files:
        full, logical, data = files[zinfo]
        files[zinfo] = (full, logical, with_movie(data, movie_name(m)))
    if zone(m) in STATISTICS and zinfo in files:
        full, logical, data = files[zinfo]
        files[zinfo] = (full, logical, with_statistics(data, STATISTICS[zone(m)]))
    return files, texts


def own_texts_and_files(game, m, reg, folder=None):
    """({logical (lower case): (full, logical, bytes)}, [('lang_file', path)]): the files track.json
    lists, from its registration, and its name: the source track's texts (every language of the
    player's edition: USA en fr mx pt, EU en fr de it es ar...) renamed, its zone info pointed at them."""
    files = {}
    for low in {p.lower() for p in m['files']}:
        if low not in reg:
            raise ValueError(f'{m["id"]}: {low} is not in its registration')
        files[low] = reg[low]
    # The pictures and materials those files load (TDEP), from the registration: the menus load them
    # at start, from the archives loaded then (Neo Bowser City's menu material uses Sewer Speedway's
    # race-select pictures, which the game keeps only in its menu and level archives: without them the
    # start failed, "Required file materialinstances/ui/neobowserselect_materials.igz ... failed").
    todo = list(files)
    while todo:
        for dep in visual_dependencies(files[todo.pop()][2]):
            if dep in reg and dep not in files:
                files[dep] = reg[dep]
                todo.append(dep)
    texts = []
    if m.get('icon'):
        if folder is None:
            raise ValueError(f'{m["id"]}: its icon needs its folder')
        files, texts = with_icon(files, m, folder)
    if m.get('background'):
        if folder is None:
            raise ValueError(f'{m["id"]}: its background needs its folder')
        files = with_background(files, m, folder, game)
    source = m.get('name_source', '').lower()
    if not source:
        return files, texts
    pattern = re.compile(rf'maps/octane/{re.escape(source)}/{re.escape(source)}_zoneinfo_([a-z]+)\.lng\Z')
    src_texts = newest(game, pattern)
    if not src_texts:
        raise ValueError(f'{m["id"]}: the game has no texts of {source}')
    rename = renamer(title(source), title(zone(m)))
    paths = []
    for low, (full, logical, data) in sorted(src_texts.items()):
        lang = pattern.match(low)[1]
        name = m['names'].get(lang) or m['names'].get('en')
        path = lng_path(zone(m), lang, logical)
        files[path.lower()] = (PREFIX + path, path, with_name(data, name, rename))
        texts.append(('lang_file', path.lower()))
        paths.append(path)
    zinfo = zone_info_path(m)
    if zinfo in files:
        full, logical, data = files[zinfo]
        files[zinfo] = (full, logical, with_texts(data, rename, paths))
    return files, texts


# Game-wide files a level archive brings its own copies of, hidden from the game (their entries'
# hashes moved off their paths: the game finds them by the FNV-1a hash of their path in each archive's
# sorted index, 0x38 + 4 * entry). Neo Bowser City's archive has other versions of the game's shaders and
# cutscene presets: the game loaded them over its own with each race there and freed them with the
# level, and something kept using the freed ones (crash at guest 0x1504095, an object reference into
# freed memory, after many Neo races: TachoGHC, 25/11). With them hidden the game's own stay.
HIDDEN_FILES = {
    't929_neo_bowser_city': ('shaders/standard.igz', 'loosedata/presets/cutscenepresets.igz'),
}
INDEX = 0x38


def game_wide_files(game, skip=(), cache=None):
    """(game-wide paths, every path's hash): the lower-case paths the game's own archives carry outside every
    level archive (tNNN_*.pak), and the FNV-1a hashes of all their paths. skip: archive names (the tracks' own).
    26/11: Neo Bowser City's level archive carries 231 such files (the race HUD, its scripts, the end of race
    timer, kart and crate scripts, sounds, older translations...), 177 of them other versions than the game's:
    loaded with the level over the game's own and freed with it, they left the game holding freed objects
    after each Neo race (crashes at guest 0x1574050, 0x14a94e9, 0x14d721c, 0x1504095 back in the lobby). A
    stock level carries none of them, so every track's copies are hidden and the game's own are used."""
    import archive_patch
    paks = [pak for pak in sorted((Path(game) / 'archives').glob('*.pak')) if pak.name.lower() not in skip]
    # ~6 s for every archive's index: kept in cache (out/tracks/game-wide.json) while the archives stay the same
    # (by name, size and modification time, as the game view's hard links keep them)
    stamp = []
    for pak in paks:
        try:
            st = pak.stat()
        except OSError:
            continue
        stamp.append([pak.name.lower(), st.st_size, st.st_mtime_ns])
    if cache:
        try:
            kept = json.loads(Path(cache).read_text(encoding='utf-8'))
            if kept.get('stamp') == stamp:
                return set(kept['wide']), set(kept['hashes'])
        except (OSError, ValueError, KeyError, TypeError):
            pass
    common, level, hashes = set(), set(), set()
    for pak in paks:
        name = pak.name.lower()
        try:
            archive = Archive(pak)
        except (OSError, ValueError, struct.error):
            continue
        try:
            paths = {archive.logical(i).lower() for i in range(len(archive.names))}
        finally:
            archive.f.close()
        (level if re.match(r't[0-9]{3}_', name) else common).update(paths)
    for path in common | level:
        hashes.add(archive_patch.path_hash(path))
    wide = common - level
    if cache:
        try:
            Path(cache).parent.mkdir(parents=True, exist_ok=True)
            tmp = Path(str(cache) + '.tmp')
            tmp.write_text(json.dumps({'stamp': stamp, 'wide': sorted(wide), 'hashes': sorted(hashes)}), encoding='utf-8')
            os.replace(tmp, cache)
        except OSError:
            pass
    return wide, hashes


def hidden_ranges(path, names, avoid=()):
    """[(offset, 4 bytes)]: the archive's index entries of names (lower case) with a hash no path has
    here, between their neighbours' (the index stays sorted), and none of avoid (the game's paths' hashes:
    a moved entry must not answer for another file)."""
    import archive_patch
    with open(path, 'rb') as f:
        header = f.read(INDEX)
        count, = struct.unpack_from('<I', header, 0xc)
        hashes = list(struct.unpack(f'<{count}I', f.read(4 * count)))
    if hashes != sorted(hashes):
        raise ValueError(f'{Path(path).name}: its index is not sorted')
    ranges = []
    for name in names:
        want = archive_patch.path_hash(name)
        if want not in hashes:
            continue
        i = hashes.index(want)
        low = hashes[i - 1] if i else -1
        high = hashes[i + 1] if i + 1 < count else 1 << 32
        moved = next((h for d in range(1, 256) for h in (want - d, want + d)
                      if low < h < high and h not in avoid), None)
        if moved is None:
            raise ValueError(f'{Path(path).name}: no free hash beside {name}')
        hashes[i] = moved
        ranges.append((INDEX + 4 * i, struct.pack('<I', moved)))
    return ranges


def prepare(game, out, config, env, base):
    """{guest archive: patch}: each level archive whose own copy of the zone info still names the
    track it was built from gets the renamed one (own_files), keeping the patch base has for it.
    (The level loads its archive after update3.pak: its copy would bring the old name back in races.)
    Its copies of game-wide files (HIDDEN_FILES) are hidden in the same patch."""
    import archive_patch
    patches = {}
    found = packages()
    own_archives = {Path(rel).name.lower() for _folder, m in found for rel in m['archives']}
    wide, avoid = game_wide_files(game, own_archives, Path(out) / 'tracks' / 'game-wide.json') if found else (set(), set())
    for folder, m in found:
        reg = archive_entries(folder / m['registration'])
        files, _texts = own_files(game, m, reg, folder)
        zinfo = files.get(zone_info_path(m))
        for rel in m['archives']:
            path = folder / rel
            archive = Archive(path)
            try:
                index = next((i for i in range(len(archive.names))
                              if archive.logical(i).lower() == zone_info_path(m)), None)
                # an encrypted level archive (tools/content_crypt.py): its data is not readable here; the
                # zone info is replaced by the patch anyway
                current = archive.read(index) if index is not None and not archive_patch.encrypted(path) else None
                carried = {archive.logical(i).lower() for i in range(len(archive.names))}
            finally:
                archive.f.close()
            entries = {index: zinfo[2]} if zinfo is not None and index is not None and current != zinfo[2] else {}
            own = {f.lower() for f in m.get('files', [])} | {zone_info_path(m)}
            names = sorted((carried & wide) - own | set(HIDDEN_FILES.get(zone(m), ())))
            hidden = hidden_ranges(path, names, avoid)
            if not entries and not hidden:
                continue
            guest = f'/app0/archives/{Path(rel).name}'
            target = Path(out) / 'tracks' / (Path(rel).stem + '.bbpatch')
            target.parent.mkdir(parents=True, exist_ok=True)
            previous = base.get(guest)
            if hidden:
                ranges = archive_patch.read(previous)[2] if previous is not None else []
                if {o for o, _d in ranges} & {o for o, _d in hidden}:
                    raise ValueError(f'{Path(rel).name}: its index is patched already')
                previous = target.with_name(target.stem + '.hidden.bbpatch')
                archive_patch.write(previous, path, guest, ranges + hidden)
            if entries:
                archive_patch.write_entries_patch(target, path, guest, entries, previous)
            else:
                os.replace(previous, target)
            patches[guest] = target
            if entries:
                print(f'Tracks: {m["id"]}: its name in {Path(rel).name}')
            if hidden:
                print(f'Tracks: {m["id"]}: {len(hidden)} game-wide files of {Path(rel).name} hidden')
    return patches


def entries(game, root=None):
    """[(full, logical, bytes)] for the port's update3.pak: the game's track lists with every
    package's track, and the packages' own files; [] without packages."""
    found = packages(root)
    if not found:
        return []
    base = newest(game, {GUI, SELECT, CHUNKS})
    if set(base) != {GUI, SELECT, CHUNKS}:
        raise ValueError('the game archives lack the track lists')
    lists = {CHUNKS: bytearray(base[CHUNKS][2])}
    menus = {path: File(base[path][2]) for path in LISTS}   # the track lists, as they grow
    items = {path: [] for path in LISTS}
    files = {}
    for folder, m in found:
        level = m['level'].lower()
        reg = archive_entries(folder / m['registration'])
        for path, (item, _list_kind) in LISTS.items():
            if path not in reg:
                raise ValueError(f'{m["id"]}: no {path} in its registration')
            if level_objects(menus[path], item, level):
                raise ValueError(f'{m["id"]}: the game has {level} already')
            items[path].append(copy_level(menus[path], reg[path][2], level, item))
        mine, texts = own_files(game, m, reg, folder)
        files.update(mine)
        own = set(mine)
        # The global package: the pairs of its registration for its own files, then its texts.
        _g, _c, reg_pairs = chunk_pairs(reg[CHUNKS][2])
        pictures = [pair for pair in texts if pair[0] == 'texture']   # its icon, before the material using it
        lists[CHUNKS] = bytearray(add_chunks(bytes(lists[CHUNKS]), pictures +
                                             [(k, p) for k, p in reg_pairs if p.lower() in own] +
                                             [pair for pair in texts if pair[0] != 'texture']))
    # Their own area in track select ("Power Charged", after the game's race areas), with its title in
    # every language of the player's edition.
    for path, menu in menus.items():
        add_area(menu, path, items[path])
        lists[path] = bytearray(menu.build())
    for low, (full, logical, data) in newest(game, MENU_TEXTS).items():
        files[low] = (full, logical, with_area_title(data))
    for low, (full, logical, data) in newest(game, GUI_TEXTS).items():
        files[low] = (full, logical, with_area_title(data, path=GUI))
    out = [(base[k][0], base[k][1], bytes(v)) for k, v in lists.items()]
    out += [v for k, v in sorted(files.items()) if k not in lists]
    return out


def digest(root=None):
    """A stamp of the packages (their manifests and archives' sizes and times)."""
    h = hashlib.sha256()
    for folder, m in packages(root):
        h.update(json.dumps(m, sort_keys=True).encode())
        for path in list(m['archives']) + [m['registration']] + [m[k] for k in ('icon', 'background', 'preview') if m.get(k)]:
            st = (folder / path).stat()
            h.update(f'{path}:{st.st_size}:{st.st_mtime_ns}'.encode())
    h.update(Path(__file__).read_bytes())
    return h.hexdigest()[:16]


def level_archives(root=None):
    """[(relative path in the game, source file)]: each package's level archives (scripts/mods.py)."""
    return [(Path(p), folder / p) for folder, m in packages(root)
            for p in list(m['archives']) + ([m['preview']] if m.get('preview') else [])]


# ---- the tracks from the server ----
# The tracks are not in the port's download: the RyuNET server's web folder has them (content/:
# manifest.json and <id>/<file>), and each start brings what is missing or changed, file by file,
# checked by SHA256. No server, no answer: the tracks already here stay as they are.
# manifest.json: {"format": 1, "tracks": [{"id": ..., "version": ..., "name": ...,
#                 "files": [{"path": "track.json", "size": N, "sha256": "..."}, ...]}]}
FILE_PATH = re.compile(r'[A-Za-z0-9_][A-Za-z0-9_.\-]*(/[A-Za-z0-9_][A-Za-z0-9_.\-]*)*\Z')
SYNC_STATE = '.sync.json'


def settings_values(path):
    values = {}
    try:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            k, sep, v = line.partition('=')
            if sep and not k.strip().startswith('#'):
                values[k.strip()] = v.strip()
    except OSError:
        pass
    return values


def content_url(settings):
    """Where the tracks are: content_url= (bbport.ini), else the online server's web folder: a
    name's https://<name>/content/, an address's (the test server) http://<address>:8094/content/."""
    values = settings_values(settings) if settings else {}
    url = os.environ.get('BB_CONTENT_URL') or values.get('content_url', '')
    if url:
        return url if url.endswith('/') else url + '/'
    server = (os.environ.get('BB_ONLINE_SERVER') or values.get('online_server') or 'octr.ryu7w7.xyz').strip()
    host = server.split(':')[0] if server.count(':') == 1 else server
    if not host:
        return ''
    if re.fullmatch(r'[0-9.]+', host):
        return f'http://{host}:8094/content/'
    return f'https://{host}/content/'


def file_sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


# Cloudflare (the R2 mirror's domain) refuses Python's own User-Agent (Python-urllib: 403, 25/11).
USER_AGENT = 'SacredFire-content/1'


def open_url(url, timeout):
    import urllib.request
    return urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': USER_AGENT}), timeout=timeout)


MIRROR = re.compile(r'https?://[A-Za-z0-9.-]+(:[0-9]+)?(/[A-Za-z0-9._~%/-]*)?/\Z')
MAX_MIRRORS = 4


def mirror_ok(url):
    return isinstance(url, str) and bool(MIRROR.match(url)) and '/../' not in url and '/./' not in url


def sources(manifest, base):
    """Where the files are taken from, in order: the manifest's mirrors (e.g. a Cloudflare R2 bucket, the
    same <id>/<path> layout under each), then the server that gave the list, the last resort. Each file
    is checked by its SHA256 whatever its source, so a mirror cannot change a track."""
    out = []
    for url in (manifest.get('mirrors') or []) if isinstance(manifest, dict) else []:
        if len(out) < MAX_MIRRORS and mirror_ok(url) and url not in out:
            out.append(url)
    if base and base not in out:
        out.append(base)
    return out


def manifest_of(root, mirrors=()):
    """The manifest of a content folder's track packages (for the server's web folder); mirrors: other
    places holding the same files, tried before the server (sources)."""
    tracks = []
    for folder, m in packages(root):
        files = []
        for path in sorted(p for p in folder.rglob('*') if p.is_file() and p.name != SYNC_STATE):
            relative = path.relative_to(folder).as_posix()
            files.append({'path': relative, 'size': path.stat().st_size, 'sha256': file_sha(path)})
        tracks.append({'id': m['id'], 'version': m.get('version', ''), 'name': m.get('names', {}).get('en', m['id']),
                       'files': files})
    out = {'format': 1, 'tracks': tracks}
    mirrors = [m if m.endswith('/') else m + '/' for m in mirrors]
    if mirrors:
        bad = [m for m in mirrors if not mirror_ok(m)]
        if bad:
            raise ValueError(f'not a mirror address: {", ".join(bad)}')
        out['mirrors'] = mirrors
    return out


class RequiredContent(Exception):
    """The server's tracks are not all here, whole: the game does not start without them."""


MANIFEST_COPY = 'manifest.json'     # the last list the server gave, kept in the content folder


def fetch_manifest(settings, root, log=print, timeout=4):
    """(manifest, from the server now, the server's content URL): the server's list of tracks, kept here as
    a copy; that copy when the server does not answer. RequiredContent when neither can be read."""
    base = content_url(settings)
    manifest, fetched = None, False
    if base:
        try:
            with open_url(base + 'manifest.json', timeout) as reply:
                manifest = json.loads(reply.read(1 << 20))
            fetched = True
        except (OSError, ValueError) as error:
            log(f'Tracks: the server did not answer ({base}: {getattr(error, "reason", error)}); checking the copy here')
    if manifest is None:
        try:
            manifest = json.loads((root / MANIFEST_COPY).read_text(encoding='utf-8'))
        except (OSError, ValueError):
            raise RequiredContent('the list of the tracks could not be read from the server')
    if not isinstance(manifest, dict) or manifest.get('format') != 1:
        raise RequiredContent('the server sent an unknown list of tracks')
    if fetched:
        try:
            root.mkdir(parents=True, exist_ok=True)
            (root / MANIFEST_COPY).write_text(json.dumps(manifest), encoding='utf-8')
        except OSError:
            pass
    return manifest, fetched, base


def track_entry_ok(tid, files):
    return bool(ID.match(tid)) and all(isinstance(f, dict) and FILE_PATH.match(str(f.get('path', '')))
                                       and '..' not in str(f.get('path')) for f in files)


def verify(settings=None, root=None, log=print, timeout=4):
    """The launcher's Scan (Tools > Content): every file of every track read again whole, cache or not.
    {'server': the list came from the server now, 'tracks': [{id, name, version, state, bad}], 'problems'}:
    state ok, missing (none of its files), outdated (an older version here) or damaged."""
    root = Path(root) if root else content_root()
    manifest, fetched, _base = fetch_manifest(settings, root, log, timeout)
    rows = []
    tracks = [t for t in manifest.get('tracks', []) if track_entry_ok(str(t.get('id', '')), t.get('files') or [])]
    total = sum(f['size'] for t in tracks for f in t.get('files') or []) or 1
    done = shown = 0
    for track in tracks:
        tid, files = str(track['id']), track.get('files') or []
        folder = root / 'tracks' / tid
        missing = bad = 0
        for f in files:
            path = folder / f['path']
            try:
                size = path.stat().st_size
            except OSError:
                missing += 1
                continue
            if size != f['size'] or file_sha(path) != f['sha256']:
                bad += 1
            done += f['size']
            if done * 100 // total != shown:
                shown = done * 100 // total
                log(f'@progress scan {shown}')
        try:
            here = json.loads((folder / 'track.json').read_text(encoding='utf-8')).get('version', '')
        except (OSError, ValueError, AttributeError):
            here = ''
        state = ('ok' if not missing and not bad else 'missing' if missing == len(files)
                 else 'outdated' if here and here != track.get('version', '') else 'damaged')
        rows.append({'id': tid, 'name': track.get('name') or tid, 'version': track.get('version', ''),
                     'state': state, 'bad': missing + bad})
    return {'server': fetched, 'tracks': rows, 'problems': sum(r['state'] != 'ok' for r in rows)}


def repair(settings=None, root=None, log=print):
    """The launcher's Download again: every track checked whole (the size/date cache dropped), and what is
    missing, damaged or old downloaded again (sync)."""
    root = Path(root) if root else content_root()
    for state in (root / 'tracks').glob('*/' + SYNC_STATE) if (root / 'tracks').is_dir() else ():
        try:
            state.unlink()
        except OSError:
            pass
    return sync(settings, root, log=log)


def sync(settings=None, root=None, log=print, timeout=4, attempts=2):
    """Bring the server's tracks into the content folder, then check every one of them is here, whole:
    [ids changed]. The tracks are part of the game (every player must have them): RequiredContent when
    one is missing or damaged after the downloads, or when the list was never had (the server's, or the
    copy of the last one kept here when the server does not answer)."""
    root = Path(root) if root else content_root()
    manifest, fetched, base = fetch_manifest(settings, root, log, timeout)
    changed, problems = [], []
    for track in manifest.get('tracks', []):
        tid = str(track.get('id', ''))
        files = track.get('files') or []
        name = track.get('name') or tid
        if not track_entry_ok(tid, files):
            log(f'Tracks: {tid!r} skipped (a bad entry in the list)')
            continue
        folder = root / 'tracks' / tid
        state_path = folder / SYNC_STATE
        try:
            state = json.loads(state_path.read_text(encoding='utf-8'))
        except (OSError, ValueError):
            state = {}

        def missing_files():
            out = []
            for f in files:
                path = folder / f['path']
                try:
                    st = path.stat()
                except OSError:
                    out.append(f)
                    continue
                if st.st_size != f['size']:
                    out.append(f)
                elif state.get(f['path']) != [st.st_size, st.st_mtime_ns, f['sha256']]:
                    if file_sha(path) != f['sha256']:
                        out.append(f)
                    else:
                        state[f['path']] = [st.st_size, st.st_mtime_ns, f['sha256']]
            return out

        missing = missing_files()
        # The server's own copy only when it answered; the mirrors also from the list kept here.
        urls = sources(manifest, base if fetched else '')
        for attempt in range(attempts if missing and urls else 0):
            total = sum(f['size'] for f in missing)
            log(f'Tracks: downloading {name} {track.get("version", "")} ({total / 1048576:.0f} MB)'
                + (' again' if attempt else ''))
            done, shown = 0, -1
            try:
                for f in missing:
                    path = folder / f['path']
                    path.parent.mkdir(parents=True, exist_ok=True)
                    part = path.with_name(path.name + '.part')
                    for i, source in enumerate(urls):
                        start, h = done, hashlib.sha256()
                        try:
                            with open_url(source + tid + '/' + f['path'], 30) as reply:
                                with open(part, 'wb') as out:
                                    for block in iter(lambda: reply.read(1 << 20), b''):
                                        out.write(block)
                                        h.update(block)
                                        done += len(block)
                                        percent = min(done * 100 // max(total, 1), 100)
                                        if percent != shown:    # the launcher's bar (not in its log)
                                            shown = percent
                                            log(f'@progress tracks {percent} {name}')
                            if h.hexdigest() != f['sha256']:
                                raise ValueError(f'{f["path"]} arrived damaged')
                        except (OSError, ValueError) as error:
                            part.unlink(missing_ok=True)
                            done = start
                            if i + 1 == len(urls):
                                raise
                            log(f'Tracks: {name}: {source}: {getattr(error, "reason", error)}; trying the next source')
                            continue
                        os.replace(part, path)
                        st = path.stat()
                        state[f['path']] = [st.st_size, st.st_mtime_ns, f['sha256']]
                        break
            except (OSError, ValueError) as error:
                log(f'Tracks: {name}: {getattr(error, "reason", error)}')
            finally:
                log('@progress tracks done')
            missing = missing_files()
            if not missing:
                changed.append(tid)
                log(f'Tracks: {name} ready')
                break
        try:
            if state:
                folder.mkdir(parents=True, exist_ok=True)
                state_path.write_text(json.dumps(state), encoding='utf-8')
        except OSError:
            pass
        if missing:
            problems.append(f'{name} ({len(missing)} of its files missing or damaged)')
    if problems:
        raise RequiredContent(', '.join(problems))
    return changed


REQUIRED_MESSAGE = (
    'The game needs its tracks from the server and could not get them all: {}.\n'
    'Check your internet connection (and that no antivirus or firewall blocks the launcher) and press Play again.\n'
    'El juego necesita sus pistas del servidor y no pudo tenerlas todas: {}.\n'
    'Revisa tu conexión a internet (y que ningún antivirus o firewall bloquee el launcher) y pulsa Jugar otra vez.')


NOT_ADDED_MESSAGE = (
    'The game could not add its tracks ({}). It does not start without them.\n'
    'Press Play again; if it happens again, send the log (last_run) to the server\'s staff.\n'
    'El juego no pudo añadir sus pistas ({}). No arranca sin ellas.\n'
    'Pulsa Jugar otra vez; si vuelve a pasar, envía el log (last_run) al staff del servidor.')


def main():
    p = argparse.ArgumentParser(description='New tracks as the port\'s content.')
    p.add_argument('game', type=Path, nargs='?')
    p.add_argument('--content', type=Path)
    p.add_argument('--check', action='store_true')
    p.add_argument('--sync', action='store_true', help='bring the server\'s tracks (--settings bbport.ini)')
    p.add_argument('--settings', type=Path)
    p.add_argument('--manifest', type=Path, help='write the content folder\'s manifest.json here')
    p.add_argument('--mirror', action='append', default=[],
                   help='with --manifest: another place with the same files (e.g. the R2 bucket\'s public '
                        'address), tried before the server; repeatable, in order')
    a = p.parse_args()
    if a.sync:
        try:
            sync(a.settings, a.content, log=lambda s: print(s, flush=True))
        except RequiredContent as error:
            print(REQUIRED_MESSAGE.format(error, error), file=sys.stderr, flush=True)
            sys.exit(3)
        return
    if a.manifest:
        a.manifest.write_text(json.dumps(manifest_of(a.content, a.mirror), indent=1), encoding='utf-8')
        return
    if not a.game:
        p.error('GAME is needed')
    for full, logical, data in entries(a.game, a.content):
        print(f'{logical}  {len(data)} bytes')


if __name__ == '__main__':
    main()
