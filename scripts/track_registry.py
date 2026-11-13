# SPDX-License-Identifier: GPL-2.0-or-later
"""Tracks added to the game's track menus: the registration the launcher builds for them.

A track is a map (octane/t922_suzuka/t922_suzuka) the game finds in its own archive
(archives/t922_suzuka.pak) and in its zone info (maps/octane/<id>/<id>_zoneinfo.igz, the track's
name, times and menu pictures). What makes it selectable is in game files every track shares:

    loosedata/guisystemdata.igz                      a CGuiOctaneLevelData (the map) in one of the
                                                     menu's pages, GuiSystemData_raceAreaLevelDataList_N
    ui/Octane_LevelSelect_Proto.igz                  a CGuiOctaneLevelSelectData (the map, the
                                                     button's animations) in that page's list,
                                                     LevelList_listItemPopulator_levelData_<page>
    packages/generated/maps/chunkInfos_pkg.igz       the track's own files, so they stream in
    materialinstances/UI/LevelSelect_materials.igz   its picture, LevelImage_<name>, when its zone
                                                     info names one there
    packages/generated/UI/domains/JuiceDomain_*_pkg  the picture's texture, for the menus that show it

Track mods used to ship whole copies of these files, so only one could be on at a time and they
hid every other change to them (Custom Cup's thumbnails). Here each one is built from the game's
copy (or another mod's already changed copy, such as Custom Cup's menu) by cloning a record the
game has and naming the new track in it: the game's records, the order of its tracks and the
other changes in the files stay as they are.

The records and the files they go in were worked out from Neo Bowser City (PanchoPewPew) and
Suzuka (CTRNFModding), and the Nitro Creator Handbook (venvs and collaborators, chapter 16).
"""
from dataclasses import dataclass, field

from igz_merge import MASK, Copier, File

GUI = 'loosedata/guisystemdata.igz'
SELECT = 'ui/Octane_LevelSelect_Proto.igz'
CHUNKS = 'packages/generated/maps/chunkInfos_pkg.igz'
MATERIALS = 'materialinstances/UI/LevelSelect_materials.igz'
DOMAINS = ('packages/generated/UI/domains/JuiceDomain_OnlineMenus_pkg.igz',
           'packages/generated/UI/domains/JuiceDomain_StartScreen_pkg.igz')
FILES = (GUI, SELECT, CHUNKS, MATERIALS) + DOMAINS

AREA = 'GuiSystemData_raceAreaLevelDataList_'
POPULATOR = 'LevelList_listItemPopulator_levelData_'
LEVEL_DATA, SELECT_DATA = 'CGuiOctaneLevelData', 'CGuiOctaneLevelSelectData'
IMAGE = 'LevelImage_'
DONOR_IMAGE = 'LevelImage_PapusPyramid'  # a plain picture material: one texture, shared settings


@dataclass
class Track:
    """A track to register. map: its map path (octane/<id>/<id>). page: the menu page's list
    (GuiSystemData_raceAreaLevelDataList_N), None for the last page. image/texture: the picture
    material its zone info names in LevelSelect_materials and that material's texture, or None
    when the track brings its own material file. files: [(kind, logical path)] of its own files,
    for chunkInfos (kinds as the game's: igx_file, texture, material_instances...)."""
    map: str
    page: str = None
    image: str = None
    texture: str = None
    files: list = field(default_factory=list)


def texture_file(texture):
    """The logical path of a texture asset name."""
    return 'textures/' + texture + '.igz'


# --- reading -----------------------------------------------------------------------------------

def named(z):
    return dict((n, o) for n, o in z.roots()[2])


def maps(z, lst):
    return [z.string_at(o + 0x10) for o in z.items(lst)]


def pages(gui, select):
    """[(area list name, populator list name, [maps])] of the race pages, in the menu's order:
    each page's list in guisystemdata.igz and the menu list holding the same tracks."""
    g, s = File(gui), File(select)
    populators = {n: maps(s, o) for n, o in named(s).items()
                  if n.startswith(POPULATOR) and s.typed[o] == 'CGuiOctaneLevelSelectDataList'}
    out = []
    for name, o in named(g).items():
        if not name.startswith(AREA) or g.typed[o] != 'CGuiOctaneAreaLevelList':
            continue
        tracks = maps(g, o)
        same = sorted(m.casefold() for m in tracks)  # the files differ in case (t504_October_Spooky)
        match = [p for p, m in populators.items() if m and sorted(x.casefold() for x in m) == same]
        out.append((name, match[0] if len(match) == 1 else None, tracks))
    return sorted(out, key=lambda p: int(p[0][len(AREA):]))


def registered(gui):
    """Every map the menu's pages name (races and battles)."""
    g = File(gui)
    return {m.casefold() for o, t in g.typed.items() if t == LEVEL_DATA for m in [g.string_at(o + 0x10)]}


# --- copying inside one file -------------------------------------------------------------------

def references(z, off, size, seen_blocks):
    """The objects an object refers to, through its data blocks too."""
    out = []
    for tag, site in z.sites_in(off, size):
        if tag not in ('ROFS', 'RPID'):
            continue
        ref = z.u64(site) & MASK
        if ref in z.typed:
            out.append(ref)
        elif site - off >= 8 and ref not in seen_blocks:
            length = z.u64(site - 8) & 0xFFFFFFFF
            if z.block_at(ref, length):
                seen_blocks.add(ref)
                out += references(z, ref, length, seen_blocks)
    return out


def owned(z, root):
    """The objects only `root` reaches: copied with it. Everything else it reaches is shared."""
    graph = {o: references(z, o, z.size(o), set()) for o in z.typed}
    referrers = {}
    for o, refs in graph.items():
        for r in refs:
            referrers.setdefault(r, set()).add(o)
    reach, todo = {root}, [root]
    while todo:
        for r in graph[todo.pop()]:
            if r not in reach:
                reach.add(r)
                todo.append(r)
    names = set(named(z).values())
    mine, changed = {root}, True
    while changed:
        changed = False
        for o in reach - mine:
            if o not in names and referrers.get(o, set()) <= mine:
                mine.add(o)
                changed = True
    return mine


def clone(z, off, rename):
    """A copy of object `off` of `z` (changed in place) and of what only it owns, with `rename`
    applied to strings and handle names; returns the copy's offset."""
    source = File(z.build())
    mine = owned(source, off)
    keep = {o: o for o in source.typed if o not in mine}
    copier = Copier(source, z, keep=keep, rename=rename)
    copier.names.pop(off, None)  # a named record is still copied, not taken for the original
    return copier.object(off)


def name_object(z, off, name):
    """`off` added to the file's root list under `name`."""
    root, names, _ = z.roots()
    z.grow(root, [off])
    z.grow(names, [name], 16)


# --- the files ---------------------------------------------------------------------------------

def add_level(gui, track, page):
    z = File(gui)
    lst = named(z)[page]
    donor = z.items(lst)[0]
    new = clone(z, donor, {z.string_at(donor + 0x10): track.map})
    z.grow(lst, [new])
    return z.build()


def add_select(select, track, populator):
    z = File(select)
    lst = named(z)[populator]
    # The first race page's first record: a plain button (NBC's and Suzuka's records are copies).
    donor = z.items(lst)[0] if z.items(lst) else None
    if donor is None:
        donor = next(o for n, l in named(z).items() if n.startswith(POPULATOR)
                     and z.typed[l] == 'CGuiOctaneLevelSelectDataList' for o in z.items(l)[:1])
    new = clone(z, donor, {z.string_at(donor + 0x10): track.map})
    z.grow(lst, [new])
    return z.build()


def add_image(materials, track):
    z = File(materials)
    names = named(z)
    if track.image in names:
        raise ValueError(f'{track.image} is already in LevelSelect_materials')
    donor = names.get(DONOR_IMAGE) or next(o for n, o in names.items() if n.startswith(IMAGE))
    source = File(z.build())
    textures = [source.handle(source.u32(site))[1] for o in owned(source, donor)
                if source.typed[o] == 'igGraphicsTexture'
                for tag, site in source.sites_in(o, source.size(o)) if tag == 'RHND']
    if len(textures) != 1:
        raise ValueError('the picture material to copy has no single texture')
    new = clone(z, donor, {textures[0]: track.texture})
    name_object(z, new, track.image)
    return z.build()


def chunk_entries(z):
    o = next(o for o, t in z.typed.items() if t == 'igStreamingChunkInfo')
    count, data = z.u32(o + 0x28), z.ref(o + 0x38)
    return o, [(z.string_at(data + 16 * i), z.string_at(data + 16 * i + 8)) for i in range(count)]


def add_chunks(blob, files, before=None):
    """Streaming entries (kind, logical path) added to a package's igStreamingChunkInfo: at the
    end, or before the first entry for the path `before` (a texture goes before the material
    that uses it, as the game's own packages have them). Paths it already has are skipped."""
    z = File(blob)
    o, entries = chunk_entries(z)
    have = {p.casefold() for _, p in entries}
    files = [f for f in files if f[1].casefold() not in have]
    if not files:
        return blob
    at = len(entries)
    if before:
        at = next((i for i, (_, p) in enumerate(entries) if p.casefold() == before.casefold()), at)
    entries[at:at] = files
    block = z.append(bytes(16 * len(entries)))
    for i, (kind, path) in enumerate(entries):
        z.set_u64(block + 16 * i, z.string_index(kind))
        z.set_u64(block + 16 * i + 8, z.string_index(path))
        z.add_site('RSTT', block + 16 * i)
        z.add_site('RSTT', block + 16 * i + 8)
    z.set_u32(o + 0x28, len(entries))
    z.set_u64(o + 0x30, (z.u64(o + 0x30) & ~0xFFFFFFFF) | 16 * len(entries))
    z.set_u64(o + 0x38, (z.u64(o + 0x38) & ~MASK) | block)
    return z.build()


def lookup(entries, path):
    """The key of `path` in {logical path: bytes}, matched without case."""
    found = [k for k in entries if k.casefold() == path.casefold()]
    return found[0] if found else None


def register(entries, tracks, log=print):
    """The tracks registered in the registration files of `entries` ({logical path: bytes}, the
    newest copy of each file in FILES the game loads, other mods' changes included). Returns the
    changed files {logical path: bytes}, under the keys `entries` has them."""
    files = {}
    for path in FILES:
        key = lookup(entries, path)
        if key is None:
            raise ValueError(f'the game has no {path}')
        files[path] = (key, entries[key])
    have = registered(files[GUI][1])
    seen = set()
    for track in tracks:
        if track.map.casefold() in have or track.map.casefold() in seen:
            raise ValueError(f'{track.map} is already a track of the game or of another package')
        seen.add(track.map.casefold())
    if not tracks:
        return {}
    layout = pages(files[GUI][1], files[SELECT][1])
    races = [p for p in layout if p[1]]
    data = {path: blob for path, (_, blob) in files.items()}
    for track in tracks:
        page = next((p for p in races if p[0] == track.page), None) if track.page else races[-1]
        if page is None:
            raise ValueError(f'{track.map}: no menu page {track.page}')
        data[GUI] = add_level(data[GUI], track, page[0])
        data[SELECT] = add_select(data[SELECT], track, page[1])
        if track.image:
            if not track.texture:
                raise ValueError(f'{track.map}: picture {track.image} without its texture')
            data[MATERIALS] = add_image(data[MATERIALS], track)
            for domain in DOMAINS:
                data[domain] = add_chunks(data[domain], [('texture', texture_file(track.texture).casefold())],
                                          before=MATERIALS)
        data[CHUNKS] = add_chunks(data[CHUNKS], [(k, p.casefold()) for k, p in track.files])
    log('Track registration: ' + ', '.join(t.map.rsplit('/', 1)[-1] for t in tracks))
    return {files[path][0]: blob for path, blob in data.items() if blob != files[path][1]}
