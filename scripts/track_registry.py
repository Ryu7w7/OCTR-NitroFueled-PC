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
# The menu's pages, in order: the area lists in guisystemdata.igz and, in the same order, the menu
# lists in Octane_LevelSelect_Proto.igz (each page's title is that list's _categoryName text, in
# ui/Octane_LevelSelect_Proto_<language>.lng). The external tracks' page goes after the last.
AREAS = 'GuiSystemData_raceAreaLevelDataList'
PAGE_LISTS = 'LevelList_listItemPopulator_levelData'
OWN_AREA = AREA + 'External'
OWN_LIST = POPULATOR + 'External'
TITLE_FIELD = 'CGuiOctaneLevelSelectDataList::_categoryName'
TITLE_DONOR = POPULATOR + '6'  # the last page, Bonus Tracks
TITLES = {'en': 'External Tracks', 'es': 'Pistas externas', 'mx': 'Pistas externas', 'pt': 'Pistas externas',
          'fr': 'Circuits externes', 'de': 'Externe Strecken', 'it': 'Piste esterne'}
LANGUAGE_FILE = 'ui/Octane_LevelSelect_Proto_{}.lng'
LANGUAGES = ('en', 'fr', 'mx', 'pt', 'es', 'de', 'it', 'ar')
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
    """[(area list name, menu list name, [maps])] of the race pages in the menu's order: the area
    lists of guisystemdata.igz's GuiSystemData_raceAreaLevelDataList and, at the same position,
    the menu lists of Octane_LevelSelect_Proto.igz's LevelList_listItemPopulator_levelData (the
    game pairs them so: the Bonus Tracks area, _7, shows in the menu list _6)."""
    g, s = File(gui), File(select)
    areas, lists = named(g), named(s)
    ginv = {o: n for n, o in areas.items()}
    sinv = {o: n for n, o in lists.items()}
    if AREAS not in areas or PAGE_LISTS not in lists:
        raise ValueError('the track menu has no page lists')
    pairs = list(zip(g.items(areas[AREAS]), s.items(lists[PAGE_LISTS])))
    if len(g.items(areas[AREAS])) != len(s.items(lists[PAGE_LISTS])):
        raise ValueError('the track menu and guisystemdata.igz have different pages')
    return [(ginv.get(a), sinv.get(m), maps(g, a)) for a, m in pairs]


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
    donor = (z.items(lst) or [next(o for n, l in named(z).items() if n.startswith(AREA)
                                   and z.typed[l] == 'CGuiOctaneAreaLevelList' for o in z.items(l)[:1])])[0]
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


def new_list(z, donor, name, container):
    """An empty list like `donor`, named `name`, added at the end of the list `container`."""
    size = z.size(donor)
    new = z.append(bytes(z.data[donor:donor + size]))
    z.add_site('RVTB', new)
    z.set_u32(new + 0xC, 0)
    z.set_u32(new + 0x10, 0)
    z.set_u64(new + 0x18, z.u64(new + 0x18) & ~0xFFFFFFFF)
    z.set_u64(new + 0x20, 0)
    z.add_site('ROFS', new + 0x20)  # the list's data, once it has some
    name_object(z, new, name)
    z.grow(container, [new])
    return new


def add_page(gui, select):
    """The external tracks' page: an empty area list and menu list after the last page."""
    g = File(gui)
    areas = named(g)[AREAS]
    new_list(g, g.items(areas)[-1], OWN_AREA, areas)
    s = File(select)
    lists = named(s)[PAGE_LISTS]
    new_list(s, s.items(lists)[-1], OWN_LIST, lists)
    return g.build(), s.build()


def add_title(lng, text):
    """The page's title in a menu language file: a copy of the last page's."""
    z = File(lng)
    lst = next(o for o, t in z.typed.items() if t == 'igLocalizedStringDataList')
    donor = next((o for o in z.items(lst) if z.handle(z.u32(o + 0x18))[0] == TITLE_DONOR
                  and z.handle(z.u32(o + 0x20))[0] == TITLE_FIELD), None)
    if donor is None:
        raise ValueError('the menu language file has no title for the last page')
    new = clone(z, donor, {TITLE_DONOR: OWN_LIST, z.string_at(donor + 0x10): text})
    z.grow(lst, [new])
    return z.build()


def lookup(entries, path):
    """The key of `path` in {logical path: bytes}, matched without case."""
    found = [k for k in entries if k.casefold() == path.casefold()]
    return found[0] if found else None


def register(entries, tracks, log=print, own_page=False):
    """The tracks registered in the registration files of `entries` ({logical path: bytes}, the
    newest copy of each file in FILES the game loads, other mods' changes included). A track
    without a page goes to the last page, or with own_page to a page of its own after it, titled
    in each menu language file of `entries` (LANGUAGE_FILE). Returns the changed files
    {logical path: bytes}, under the keys `entries` has them."""
    files = {}
    if own_page and any(not t.page for t in tracks):
        for lang in LANGUAGES:
            key = lookup(entries, LANGUAGE_FILE.format(lang))
            if key is not None:
                files[LANGUAGE_FILE.format(lang)] = (key, entries[key])
        if LANGUAGE_FILE.format('en') not in files:
            raise ValueError('the game has no ' + LANGUAGE_FILE.format('en'))
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
    data = {path: blob for path, (_, blob) in files.items()}
    last = None
    if own_page and any(not t.page for t in tracks):
        data[GUI], data[SELECT] = add_page(data[GUI], data[SELECT])
        for lang in LANGUAGES:
            path = LANGUAGE_FILE.format(lang)
            if path in data:
                data[path] = add_title(data[path], TITLES.get(lang, TITLES['en']))
        last = (OWN_AREA, OWN_LIST)
    races = [p for p in pages(data[GUI], data[SELECT]) if p[1]]
    for track in tracks:
        page = next((p for p in races if p[0] == track.page), None) if track.page else (last or races[-1])
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
