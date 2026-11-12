"""Native NORMAL / ITEMLESS / BATTLE options for the shared online mode selector.

The runtime keeps Speed disabled in public matchmaking until server queues support it.
"""
import argparse
import hashlib
import json
import os
import re
import struct
from pathlib import Path

import archive_patch
import hall_of_fame
from ranked_results import score_card
from build_ui_asset import build_pak as build_ui_asset_pak, lobby_background_texture
import gcn_shaders as iga
from igz_file import Igz, unpack_offsets, pack_offsets
from graphics_tab import fnv1a
from mod_file_paths import resolve, records
from mod_overlays import file_map
from mod_components import long_path
from patches import load_segments
from ranked_ui_assets import (HOF_BACKGROUND_IMAGE, HOF_BACKGROUND_MATERIAL,
                              HOF_BACKGROUND_TEXTURE, HOF_ICON_MATERIAL,
                              QUICK_ICON_TEXTURE, RANKED_BACKGROUND_MATERIAL,
                              RANKED_BACKGROUND_NAMESPACE, RANKED_BACKGROUND_PATH,
                              RANKED_BACKGROUND_TEXTURE,
                              SINGLE_RACE_TEXTURE, TIERS, asset_replacements,
                              clone_lobby_materials, clone_lobby_rank_materials, tier_icon,
                              menu_asset_replacements,
                              menu_icon, quick_icon_texture)

ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_modeselect.igz'
LABEL_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_modeselect_en.lng'
RACE_MUSIC_ENTRY = 'temporary/octane/data/ps4/output/vsc/common/race/common_race_music_manager.igz'
TIMER_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_returntolobby_timer.igz'
LOBBY_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_privategame.igz'
SETTINGS_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_privategamesettings.igz'
PUBLIC_LOBBY_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_matchmaking.igz'
MODE_SELECT_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_modeselect.igz'
MAIN_LOBBY_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_main.igz'
LOBBY_MATERIALS_ENTRY = ('temporary/octane/data/ps4/output/ui/materialInstances/'
                         'juice_Octane_Lobby_Main_materials.igz')
LOBBY_MEMBERS_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobbymembers.igz'
LOBBY_MEMBERS_MATERIALS_ENTRY = ('temporary/octane/data/ps4/output/ui/materialinstances/'
                                 'juice_octane_lobbymembers_materials.igz')
GRAPH_SHA256 = {
    '56d613eaa1234151a3d720af694b460480c4cbc766508cb60c324c91ca8eae47',  # USA 1.21
    'cdb4b366079458a0f1ed32fb8a8ac69d2d841f18bacb9a72430a02e93fcf52a2',  # EU 1.21
    'a23cd1ef8903e50161c2d667e59f85a51fdccecdd913a21d54461f5e693966e3',  # USA widescreen
    'e488136a8c56e367e49e07bdfa42bf7d4c88360893ef135a7ee83eb1e0afa5fc',  # EU widescreen
}
# Native selector's selected-index event adapter, including its RIP-relative LEA.
HOOK_PATTERN = re.compile(bytes.fromhex(
    '55 48 89 e5 41 57 41 56 53 50 48 8d 05') + b'[\x97\xa7]' +
    bytes.fromhex('a1 87 01'), re.DOTALL)

CUP_GRAPHS = {
    'octane_lobby_privategamesettings/octane_lobby_privategamesettings_levelselectbutton':
        ('84e8f56a17637b3fedb4fa7efed6349b6b22c12265ad929c94221e1d6a65f9d2',
         0x378, 'Octane_CupSelect', True),
    'octane_cupselect/octane_cupselect_listbutton':
        ('321eab9262d2d34bed163a88b0d64b209126c7222525ad554986b7ba090e2994',
         0x818, 'Octane_Lobby_PrivateGameSettings', False),
    'octane_cupselect/octane_cupselect_root':
        ('3fae87784d9e0fe697a2d36c4f6c2be7066f73155bf8830bb6bac558421b44b2',
         0x2280, 'Octane_Lobby_PrivateGameSettings', False),
}


TEAM_RANKING_ROOT = 'temporary/octane/data/ps4/output/vsc/ui/octane_battleteam_ranking/octane_battleteam_ranking_root.igz'


TEAM_RANKING_UI = 'temporary/octane/data/ps4/output/ui/octane_battleteam_ranking.igz'

def driving_style_visible(blob):
    """Clone the driving-style slot left of nickname; keep original untouched."""
    g = Igz(blob)
    objects = dict(g.objects())
    names = [off for off, kind in objects.items()
             if kind == 'igGuiPlaceable' and g.string_at(off + 0x28) == '[MemberName]']
    if len(names) != 1:
        return blob
    name = names[0]
    tables = [off for off, kind in objects.items() if kind == 'igGuiPlaceableTable']
    table = next((off for off in tables
                  if any(g.u64(g.u64(off + 0x20) + 8 * i) == name
                         for i in range(g.u32(off + 0x0c)))), None)
    if table is None:
        return blob
    siblings = [g.u64(g.u64(table + 0x20) + 8 * i) for i in range(g.u32(table + 0x0c))]
    handles = g.fixup('EXNM')
    handle_entries = list(struct.iter_unpack('<II', handles[3][:handles[1] * 8]))
    external_handles = [(g.strings[a], g.strings[b & 0x7fffffff])
                        for a, b in handle_entries if b & 0x80000000]
    slots = [off for off in siblings
             if off != name and objects.get(off) == 'igGuiPlaceable' and
             (g.u64(off + 0x38) & 0x80000000) and
             external_handles[g.u64(off + 0x38) & 0x7fffffff][0] == 'octane_ui_ico_singlerace_tga']
    if not slots:
        return blob
    icon = slots[0]
    for fixup_tag in ('RHND', 'RNEX'):
        if fixup_tag not in g.sites:
            g.sites[fixup_tag] = g.read_list(fixup_tag, unpack_offsets)
    size = g.sizes[g.u64(icon)]
    clone = g.append(g.data[icon:icon + size])
    g.add_site('RVTB', clone)
    for tag, sites in g.sites.items():
        for site in list(sites):
            if icon < site < icon + size:
                g.add_site(tag, clone + site - icon)
    g.set_u64(clone + 0x28, g.string_index('HideLobbyMembers'))
    g.add_site('RSTT', clone + 0x28)
    g.set_u32(clone + 0x0c, g.u32(clone + 0x0c) & ~0x400)
    name_x = struct.unpack_from('<f', g.data, name + 0x60)[0]
    icon_x, icon_y = struct.unpack_from('<2f', g.data, icon + 0x60)
    icon_w = struct.unpack_from('<f', g.data, icon + 0x90)[0]
    # left of nickname, name untouched — adjust X here (bigger = more right)
    struct.pack_into('<2f', g.data, clone + 0x60, 18.0, icon_y)
    struct.pack_into('<2f', g.data, clone + 0x90, icon_w * 1.5, icon_w * 1.5)
    def pointer(at, value):
        g.set_u64(at, value)
        if value:
            g.add_site('ROFS', at)
        elif at in g.sites['ROFS']:
            g.sites['ROFS'].remove(at)
    name_list = next(off for off, kind in objects.items() if kind == 'igNameList')
    named_count = g.u32(name_list + 0x0c)
    if named_count != g.u32(0x0c):
        return g.build()
    named_objects = [g.u64(g.u64(0x20) + i * 8) for i in range(named_count)]
    named_entries = bytearray(g.data[g.u64(name_list + 0x20):g.u64(name_list + 0x20) + named_count * 16])
    export = 'OCTR_DRIVESTYLE_CLONE'
    named_entries += struct.pack('<QII', g.string_index(export), fnv1a(export), 0)
    named_objects.append(clone)
    def replace_list(off, values, stride=8):
        data = g.append(struct.pack('<' + 'Q' * len(values), *values))
        for i in range(len(values)):
            g.add_site('ROFS', data + i * stride)
        g.set_u32(off + 0x0c, len(values))
        g.set_u32(off + 0x10, len(values))
        g.set_u64(off + 0x18, (g.u64(off + 0x18) & ~0xffffffff) | len(values) * stride)
        pointer(off + 0x20, data)
    replace_list(0, named_objects)
    name_data = g.append(named_entries)
    for i in range(len(named_objects)):
        g.add_site('RSTT', name_data + i * 16)
    g.set_u32(name_list + 0x0c, len(named_objects))
    g.set_u32(name_list + 0x10, len(named_objects))
    g.set_u64(name_list + 0x18, (g.u64(name_list + 0x18) & ~0xffffffff) | len(named_objects) * 16)
    pointer(name_list + 0x20, name_data)
    ni = siblings.index(name)
    prev = g.u64(name + 0x58)
    all_siblings = siblings[:ni] + [clone] + siblings[ni:]
    row_data = g.append(struct.pack('<' + 'Q' * len(all_siblings), *all_siblings))
    for i in range(len(all_siblings)):
        g.add_site('ROFS', row_data + 8 * i)
    g.set_u32(table + 0x0c, len(all_siblings))
    g.set_u32(table + 0x10, len(all_siblings))
    g.set_u64(table + 0x18, (g.u64(table + 0x18) & ~0xffffffff) | (8 * len(all_siblings)))
    pointer(table + 0x20, row_data)
    pointer(clone + 0x40, g.u64(name + 0x40))
    pointer(clone + 0x58, prev)
    pointer(clone + 0x50, name)
    if prev:
        pointer(prev + 0x50, clone)
    pointer(name + 0x58, clone)
    for tag in ('RHND', 'RNEX'):
        fixup = g.fixup(tag)
        sites = sorted(set(g.sites[tag]))
        fixup[1], fixup[3] = len(sites), pack_offsets(sites)
        fixup[4] = (fixup[2] + len(fixup[3]) + 3) & ~3
    return g.build()


def ready_icon_clone(blob):
    return driving_style_visible(blob)


def ranked_lobby_icons(blob):
    """PNG rank sprites (no decals). Tags OCTR_RANK_ICON_{mode}_{tier} for prebuilt."""
    from ranked_ui_assets import TIERS, tier_icon, tier_material
    g = Igz(blob)
    objects = dict(g.objects())
    names = [off for off, kind in objects.items()
             if kind == 'igGuiPlaceable' and g.string_at(off + 0x28) == '[MemberName]']
    if len(names) != 1: raise ValueError('unsupported Lobby Members name placeable')
    name = names[0]
    tables = [off for off, kind in objects.items() if kind == 'igGuiPlaceableTable']
    table = next((off for off in tables
                  if any(g.u64(g.u64(off + 0x20) + 8 * i) == name
                         for i in range(g.u32(off + 0x0c)))), None)
    if table is None: raise ValueError('Lobby Members row has no placeable table')
    siblings = [g.u64(g.u64(table + 0x20) + 8 * i) for i in range(g.u32(table + 0x0c))]
    handles = g.fixup('EXNM')
    handle_entries = list(struct.iter_unpack('<II', handles[3][:handles[1] * 8]))
    external_handles = [(g.strings[a], g.strings[b & 0x7fffffff])
                        for a, b in handle_entries if b & 0x80000000]
    templates = {}
    for off in siblings:
        if off != name and objects.get(off) == 'igGuiPlaceable' and g.u64(off + 0x38) & 0x80000000:
            asset = external_handles[g.u64(off + 0x38) & 0x7fffffff][0]
            templates.setdefault(asset, []).append(off)
    if 'octane_ui_ico_singlerace_tga' not in templates:
        raise ValueError('Lobby Members row has no sprite placeable')
    external = g.read_list('RNEX', unpack_offsets)
    handle_sites = g.read_list('RHND', unpack_offsets)
    def handle(asset):
        index = sum(bool(space & 0x80000000) for _, space in handle_entries)
        handle_entries.append((g.string_index(asset),
                               g.string_index('Octane_LobbyMembers') | 0x80000000))
        return index | 0x80000000
    def pointer(at, value):
        g.set_u64(at, value)
        if value: g.add_site('ROFS', at)
        elif at in g.sites['ROFS']: g.sites['ROFS'].remove(at)
    name_list = next(off for off, kind in objects.items() if kind == 'igNameList')
    named_count = g.u32(name_list + 0x0c)
    if named_count != g.u32(0x0c): raise ValueError('Lobby Members export/object lists differ')
    named_objects = [g.u64(g.u64(0x20) + i * 8) for i in range(named_count)]
    named_entries = bytearray(g.data[g.u64(name_list + 0x20):g.u64(name_list + 0x20) + named_count * 16])
    assets = next(off for off, kind in objects.items() if kind == 'igGuiAssetList')
    asset_objects = [g.u64(g.u64(assets + 0x20) + i * 8) for i in range(g.u32(assets + 0x0c))]
    sprite_template = next(off for off in asset_objects if objects.get(off) == 'igGuiSprite')
    sprite_size = g.sizes[g.u64(sprite_template)]
    material_ids = g.fixup('EXID')
    sprite_exports = {}
    for mode, tier in TIERS:
        image_name = tier_icon(mode, tier)[1]
        material = tier_material(mode, tier)[2]
        sprite = g.append(g.data[sprite_template:sprite_template + sprite_size])
        g.add_site('RVTB', sprite)
        for tag, sites in g.sites.items():
            for site in list(sites):
                if sprite_template < site < sprite_template + sprite_size:
                    g.add_site(tag, sprite + site - sprite_template)
        for site in list(handle_sites):
            if sprite_template < site < sprite_template + sprite_size:
                handle_sites.append(sprite + site - sprite_template)
        for site in list(external):
            if sprite_template < site < sprite_template + sprite_size:
                external.append(sprite + site - sprite_template)
        g.set_u64(sprite + 0x10, g.string_index(image_name))
        g.add_site('RSTT', sprite + 0x10)
        g.set_u64(sprite + 0x20, material_ids[1])
        # EXID blocks pad after their counted entries: append after the last entry, not the padding.
        material_ids[3] = (material_ids[3][:material_ids[1] * 8] +
                           struct.pack('<II', fnv1a(material), fnv1a('juice_Octane_LobbyMembers_materials')))
        material_ids[1] += 1
        export = f'OCTR_RANK_{mode}_{tier}_ASSET'
        named_entries += struct.pack('<QII', g.string_index(export), fnv1a(export), 0)
        named_objects.append(sprite)
        asset_objects.append(sprite)
        sprite_exports[mode, tier] = export
    material_ids[4] = (material_ids[2] + len(material_ids[3]) + 3) & ~3
    template, shadow_template = templates['octane_ui_ico_singlerace_tga'][0], templates['octane_ui_ico_singlerace_tga'][-1]
    size = g.sizes[g.u64(template)]
    tw, th = struct.unpack_from('<2f', g.data, template + 0x90)
    iw, ih = abs(tw), abs(th)
    new = []
    for mode, tier in TIERS:
        for kind, source in (('SHADOW', shadow_template), ('ICON', template)):
            icon = g.append(g.data[source:source + size])
            g.add_site('RVTB', icon)
            for tag, sites in g.sites.items():
                for site in list(sites):
                    if source < site < source + size:
                        g.add_site(tag, icon + site - source)
            for site in list(handle_sites):
                if source < site < source + size:
                    handle_sites.append(icon + site - source)
            for site in list(external):
                if source < site < source + size:
                    external.append(icon + site - source)
            g.set_u64(icon + 0x28, g.string_index(f'OCTR_RANK_{kind}_{mode}_{tier}'))
            g.set_u64(icon + 0x38, handle(sprite_exports[mode, tier]))
            g.set_u32(icon + 0x0c, g.u32(icon + 0x0c) | 0x400)
            pointer(icon + 0xf0, 0)
            g.add_site('RSTT', icon + 0x28)
            struct.pack_into('<2f', g.data, icon + 0x90, iw, ih)
            g.data[icon + 0x60:icon + 0x68] = g.data[source + 0x60:source + 0x68]
            if kind == 'SHADOW':
                x, y = struct.unpack_from('<2f', g.data, template + 0x60)
                struct.pack_into('<2f', g.data, icon + 0x60, x + 2.0, y + 2.0)
                struct.pack_into('<4f', g.data, icon + 0xc0, 0.0, 0.0, 0.0, 1.0)
            new.append(icon)
    # Keep the native style slots addressable by their behavior, but never render them.
    for source, kind in objects.items():
        if kind == 'igGuiPlaceable' and g.u64(source + 0x38) & 0x80000000:
            asset = external_handles[g.u64(source + 0x38) & 0x7fffffff][0]
            if asset == 'octane_ui_ico_singlerace_tga':
                struct.pack_into('<f', g.data, source + 0xe0, 0.0)
    def replace_list(off, values, stride=8):
        data = g.append(struct.pack('<' + 'Q' * len(values), *values))
        for i in range(len(values)): g.add_site('ROFS', data + i * stride)
        g.set_u32(off + 0x0c, len(values)); g.set_u32(off + 0x10, len(values))
        g.set_u64(off + 0x18, (g.u64(off + 0x18) & ~0xffffffff) | len(values) * stride)
        pointer(off + 0x20, data)
    replace_list(0, named_objects)
    name_data = g.append(named_entries)
    for i in range(len(named_objects)): g.add_site('RSTT', name_data + i * 16)
    g.set_u32(name_list + 0x0c, len(named_objects)); g.set_u32(name_list + 0x10, len(named_objects))
    g.set_u64(name_list + 0x18, (g.u64(name_list + 0x18) & ~0xffffffff) | len(named_objects) * 16)
    pointer(name_list + 0x20, name_data)
    replace_list(assets, asset_objects)
    all_siblings = siblings + new
    row_data = g.append(struct.pack('<' + 'Q' * len(all_siblings), *all_siblings))
    for i in range(len(all_siblings)): g.add_site('ROFS', row_data + 8 * i)
    g.set_u32(table + 0x0c, len(all_siblings)); g.set_u32(table + 0x10, len(all_siblings))
    g.set_u64(table + 0x18, (g.u64(table + 0x18) & ~0xffffffff) | (8 * len(all_siblings)))
    pointer(table + 0x20, row_data)
    parent = g.u64(name + 0x40)
    previous = name
    while g.u64(previous + 0x50):
        previous = g.u64(previous + 0x50)
    for icon in new:
        pointer(icon + 0x40, parent); pointer(icon + 0x58, previous)
        pointer(previous + 0x50, icon); previous = icon
    pointer(previous + 0x50, 0)
    handles[1] = len(handle_entries)
    handles[3] = b''.join(struct.pack('<II', *entry) for entry in handle_entries)
    handles[4] = (handles[2] + len(handles[3]) + 3) & ~3
    for tag, sites in (('RHND', handle_sites), ('RNEX', external)):
        fixup = g.fixup(tag); sites = sorted(set(sites))
        fixup[1], fixup[3] = len(sites), pack_offsets(sites)
        fixup[4] = (fixup[2] + len(fixup[3]) + 3) & ~3
    return g.build()



def team_ranking_header(blob):
    g = Igz(blob)
    if g.string_at(0xaa0 + 0x28) != '[WinsText]' or g.string_at(0xbc0 + 0x28) != '[WinnerNameText]':
        raise ValueError('unsupported native team ranking header')
    for tag in ('RHND', 'RNEX'):
        try:
            g.sites[tag] = g.read_list(tag, unpack_offsets)
        except KeyError:
            pass
    source = 0xbc0
    clone = g.append(g.data[source:source + 0x120])
    for tag, sites in g.sites.items():
        for site in list(sites):
            if source <= site < source + 0x120:
                g.add_site(tag, clone + site - source)
    g.set_u64(clone + 0x28, g.string_index('OCTR_TEAM_POINTS'))
    g.set_u32(clone + 12, g.u32(clone + 12) | 0x400)  # Runtime reveals only private race teams.
    g.set_u64(clone + 0x50, source)
    g.set_u64(source + 0x58, clone)
    array = g.append(struct.pack('<3Q', 0xaa0, source, clone))
    for i in range(3):
        g.add_site('ROFS', array + i * 8)
    table = 0xa60
    g.set_u32(table + 12, 3)
    g.set_u32(table + 16, 3)
    g.set_u64(table + 24, (1 << 56) | (1 << 52) | 24)
    g.set_u64(table + 32, array)
    # The project already exports an unused Portrait001 sprite. Native 9b87e0
    # replaces its material with the racer's minimap portrait, as in Highscores.
    behavior, row, template = 0x5a38, 0x52d0, 0x5550
    # RaceRanking's entrance moves/scales the row but leaves _alpha to DNF.
    # BattleTeam also fades to alpha=1, erasing the DNF value after binding.
    for tracks, keep in ((0x6268, (0x62a8, 0x64f0)),
                         (0x6650, (0x6690, 0x67f8))):
        if g.u32(tracks + 12) != 3:
            raise ValueError('unsupported native team entrance tracks')
        array = g.u64(tracks + 32)
        for i, track in enumerate(keep):
            g.set_u64(array + i * 8, track)
        g.set_u32(tracks + 12, 2)
    names = next(o for o, t in g.objects() if t == 'igNameList')
    exports = {g.string_at(g.u64(names + 32) + i * 16): g.u64(g.u64(32) + i * 8)
               for i in range(g.u32(names + 12))}
    if (dict(g.objects()).get(exports.get('Portrait001')) != 'igGuiSprite' or
            dict(g.objects()).get(exports.get('Portrait002')) != 'igGuiSprite' or
            dict(g.objects()).get(behavior) != 'CGuiBehaviorOctaneRaceRankingSelectItem' or
            g.u64(behavior + 0x98) or g.u64(behavior + 0xa0) or g.u64(0x5b50 + 0x58) != 0x5c70 or
            g.u64(0x5c70 + 0x50) != 0x5b50):
        raise ValueError('unsupported native team portrait layout')
    exnm = g.fixup('EXNM')
    pairs = list(struct.iter_unpack('<II', exnm[3][:exnm[1] * 8]))
    handle = sum(bool(b & 0x80000000) for _, b in pairs)
    pairs.append((g.string_index('Portrait001'),
                  g.string_index('Octane_BattleTeam_Ranking') | 0x80000000))
    pairs.append((g.string_index('Portrait002'),
                  g.string_index('Octane_BattleTeam_Ranking') | 0x80000000))
    exnm[1], exnm[3] = len(pairs), b''.join(struct.pack('<II', *p) for p in pairs)
    exnm[4] = exnm[2] + len(exnm[3])
    portrait = g.append(g.data[template:template + 0x120])
    for tag, sites in g.sites.items():
        for site in list(sites):
            if template <= site < template + 0x120:
                g.add_site(tag, portrait + site - template)
    g.set_u64(portrait + 0x28, g.string_index('OCTR_TEAM_AVATAR'))
    g.add_site('RSTT', portrait + 0x28)
    g.set_u64(portrait + 0x38, handle | 0x80000000)
    g.set_u64(portrait + 0x40, row)
    g.set_u32(portrait + 12, g.u32(portrait + 12) | 0x400)
    # Same portrait geometry as the finish panel; native material stays untinted.
    struct.pack_into('<3f', g.data, portrait + 0x60, 80.0, 53.0, 0.0)
    struct.pack_into('<3f', g.data, portrait + 0x6c, 0.5, 0.5, 0.5)
    struct.pack_into('<3f', g.data, portrait + 0x84, 0.85, 0.85, 1.0)
    struct.pack_into('<2f', g.data, portrait + 0x90, 64.0, 64.0)
    # The null-placeable template uses relative alignment (2,2). Portraits in
    # RaceRanking/Highscores use absolute alignment; copying 2,2 adds half a row.
    g.set_u32(portrait + 0xa0, 0)
    g.set_u32(portrait + 0xa4, 0)
    struct.pack_into('<f', g.data, portrait + 0x80, -3.3261168)
    # +50/+58 link the flattened project order, not just this child table.
    # Insert between two neighbors, preserving the remainder of the chain.
    g.set_u64(portrait + 0x50, 0x5b50)
    g.set_u64(portrait + 0x58, 0x5c70)
    g.set_u64(0x5b50 + 0x58, portrait)
    g.set_u64(0x5c70 + 0x50, portrait)
    g.set_u64(behavior + 0x98, portrait)
    g.add_site('ROFS', behavior + 0x98)
    engine = g.append(g.data[portrait:portrait + 0x120])
    for tag, sites in g.sites.items():
        for site in list(sites):
            if portrait <= site < portrait + 0x120:
                g.add_site(tag, engine + site - portrait)
    g.set_u64(engine + 0x28, g.string_index('OCTR_TEAM_ENGINE'))
    g.set_u64(engine + 0x38, (handle + 1) | 0x80000000)
    struct.pack_into('<3f', g.data, engine + 0x60, 131.0, 50.0, 0.0)
    struct.pack_into('<3f', g.data, engine + 0x84, 0.45, 0.45, 1.0)
    struct.pack_into('<2f', g.data, engine + 0x90, 80.0, 80.0)
    g.set_u64(portrait + 0x58, engine)
    g.set_u64(engine + 0x50, portrait)
    g.set_u64(0x5c70 + 0x50, engine)
    g.set_u64(behavior + 0xa0, engine)
    g.add_site('ROFS', behavior + 0xa0)
    # Runtime adjusts only private-team lists before their graph populates rows.
    g.set_u64(0xe30 + 0x28, g.string_index('OCTR_TEAM_LIST'))
    g.add_site('RSTT', 0xe30 + 0x28)
    table = g.u64(row + 0x48)
    count, old = g.u32(table + 12), g.u64(table + 32)
    children = [g.u64(old + i * 8) for i in range(count)]
    children[children.index(0x5b50) + 1:children.index(0x5b50) + 1] = [portrait, engine]
    array = g.append(struct.pack(f'<{len(children)}Q', *children))
    for i in range(len(children)):
        g.add_site('ROFS', array + i * 8)
    g.set_u32(table + 12, len(children))
    g.set_u32(table + 16, len(children))
    g.set_u64(table + 24, (g.u64(table + 24) & ~0xffffffff) | len(children) * 8)
    g.set_u64(table + 32, array)
    return g.build()


def team_selector_legend(blob):
    """Mark Ready/Back so private Team Race can show the current native action."""
    g = Igz(blob)
    x = g.fixup('EXNM')
    methods = [g.strings[a] for a, b in struct.iter_unpack('<II', x[3][:x[1] * 8])
               if not b & 0x80000000]
    target = methods.index('CGuiNodes::addButtonToLegend_VSC')
    kinds = dict(g.objects())
    marked = 0
    for node, kind in list(g.objects()):
        if kind != 'igVscCppActionNode' or g.u64(node + 0x38) != target:
            continue
        pins = g.u64(node + 0x20)
        button = g.u64(pins + 8)
        if g.u32(node + 0x10) != 4 or kinds.get(button) != 'igVscConstEnumAccessor' or g.u32(button + 16) not in (1, 2):
            continue
        count = 6 if g.u32(button + 16) == 1 else 5
        moved = g.append(g.data[pins:pins + 32] + bytes((count - 4) * 8))
        for i in range(4):
            if g.u64(moved + i * 8):
                g.add_site('ROFS', moved + i * 8)
        g.set_u32(node + 0x10, count)
        g.set_u32(node + 0x14, count)
        g.set_u64(node + 0x18, (1 << 56) | (1 << 52) | (count * 8))
        g.set_u64(node + 0x20, moved)
        marked += 1
    if marked != 2:
        raise ValueError('unsupported native team selector Ready/Back legends')
    return g.build()


def team_finish_sounds(blob):
    """Mark just the individual finish stings; runtime delays them in team races."""
    g = Igz(blob)
    names = next(o for o, t in g.objects() if t == 'igNameList')
    table = g.u64(names + 32)
    objects = {g.string_at(table + i * 16): g.u64(g.u64(32) + i * 8)
               for i in range(g.u32(names + 12))}
    accessors = {objects[name] for name in (
        'Sound_id_xkmi6sp9_Accessor', 'Sound_id_vm79byis_Accessor')}
    x = g.fixup('EXNM')
    methods = [g.strings[a] for a, b in struct.iter_unpack('<II', x[3][:x[1] * 8])
               if not b & 0x80000000]
    target = methods.index('CAudioNodes::playSound_v2_VSC')
    marked = 0
    for node, kind in list(g.objects()):
        if kind != 'igVscCppActionNode' or g.u64(node + 0x38) != target:
            continue
        pins = g.u64(node + 0x20)
        if g.u32(node + 0x10) != 4 or g.u64(pins + 8) not in accessors:
            continue
        moved = g.append(g.data[pins:pins + 32] + bytes(8))
        for i in range(4):
            if g.u64(moved + i * 8):
                g.add_site('ROFS', moved + i * 8)
        g.set_u32(node + 0x10, 5)
        g.set_u32(node + 0x14, 5)
        g.set_u64(node + 0x18, (1 << 56) | (1 << 52) | 40)
        g.set_u64(node + 0x20, moved)
        marked += 1
    if marked != 2:
        raise ValueError('unsupported native race finish sound graph')
    return g.build()


def private_start_button(blob):
    """Validate the host before readyPlayerLobby can hide/lock the lobby."""
    g = Igz(blob)
    g.sites['RNEX'] = g.read_list('RNEX', unpack_offsets)
    x = g.fixup('EXNM')
    methods = [g.strings[a] for a, b in struct.iter_unpack('<II', x[3][:x[1] * 8])
               if not b & 0x80000000]
    def action(name):
        method = methods.index(name)
        nodes = [o for o, t in g.objects()
                 if t == 'igVscCppActionNode' and g.u64(o + 0x38) == method]
        if len(nodes) != 1:
            raise ValueError('unsupported native private Start button: ' + name)
        return nodes[0]
    can_start = action('COctaneLobbyNodes::canStartGame_VSC')
    ready = action('COctaneLobbyNodes::readyPlayerLobby_VSC')
    host = action('CNetworkNodes::isHost2_VSC')
    incoming = [g.u64(o + 0x20) + 8 for o, t in g.objects()
                if t == 'igVscActionNodeList' and g.u32(o + 0x10) == 2
                and g.u64(g.u64(o + 0x20) + 8) == ready]
    if len(incoming) != 1 or g.u32(can_start + 0x10) != 0:
        raise ValueError('unsupported native private ready branch')
    source_outputs = g.u64(can_start + 0x30)
    def branch(source, yes, no, marked=False):
        node = g.append(g.data[source:source + 0x40])
        outputs = g.append(g.data[source_outputs:source_outputs + 0x38])
        for tag, sites in g.sites.items():
            for site in list(sites):
                if source <= site < source + 0x40:
                    g.add_site(tag, node + site - source)
                if source_outputs <= site < source_outputs + 0x38:
                    g.add_site(tag, outputs + site - source_outputs)
        g.set_u64(node + 0x30, outputs)
        g.add_site('ROFS', node + 0x30)
        g.set_u64(outputs + 0x20, outputs + 0x28)
        g.add_site('ROFS', outputs + 0x20)
        for offset, target in ((0x28, yes), (0x30, no)):
            g.set_u64(outputs + offset, target)
            if target:
                g.add_site('ROFS', outputs + offset)
        if marked:
            g.set_u32(node + 0x10, 1)
            g.set_u32(node + 0x14, 1)
            g.set_u64(node + 0x18, (1 << 56) | (1 << 52) | 8)
            g.set_u64(node + 0x20, g.append(bytes(8)))
            g.add_site('ROFS', node + 0x20)
        return node
    guard = branch(can_start, ready, 0, marked=True)
    g.set_u64(incoming[0], branch(host, guard, ready))
    return g.build()


def team_selector_callback(blob):
    """Mark the lobby callback that disables the newly opened Team Selector."""
    g = Igz(blob)
    names = next(o for o, t in g.objects() if t == 'igNameList')
    array = g.u64(names + 32)
    index = next(i for i in range(g.u32(names + 12))
                 if g.string_at(array + i * 16) == 'method__id_qp45dard')
    method = g.u64(g.u64(32) + index * 8)
    entries = g.u64(method + 16)
    entry = g.u64(g.u64(entries + 32))
    node = g.u64(entry + 0x30)
    x = g.fixup('EXNM')
    methods = [g.strings[a] for a, b in struct.iter_unpack('<II', x[3][:x[1] * 8])
               if not b & 0x80000000]
    if (dict(g.objects()).get(node) != 'igVscCppActionNode' or g.u32(node + 16) != 2 or
            methods[g.u64(node + 0x38)] != 'CGuiNodes::disableControls_VSC'):
        raise ValueError('unsupported Team Selector opened callback')
    old = g.u64(node + 32)
    pins = g.append(g.data[old:old + 16] + bytes(8))
    for site in g.sites['ROFS'][:]:
        if old <= site < old + 16:
            g.add_site('ROFS', pins + site - old)
    # Third unused pin marks only this call for the private Single runtime gate.
    g.set_u32(node + 16, 3)
    g.set_u32(node + 20, 3)
    g.set_u64(node + 24, (1 << 56) | (1 << 52) | 24)
    g.set_u64(node + 32, pins)
    return g.build()


def team_ranking_navigation(blob):
    """Mark only this graph's network branches for private race navigation."""
    g = Igz(blob)
    names = g.fixup('EXNM')
    methods = [g.strings[a] for a, b in struct.iter_unpack('<II', names[3][:names[1] * 8])
               if not b & 0x80000000]
    method = methods.index('CNetworkNodes::isInNetworkGame2_VSC')
    nodes = [o for o, t in g.objects() if t == 'igVscCppActionNode' and g.u64(o + 0x38) == method]
    if len(nodes) != 3 or any(g.u32(o + 0x10) != 0 for o in nodes):
        raise ValueError('unsupported native team ranking network branches')
    # isInNetworkGame2 takes no pins; one unused null pin is our scoped marker.
    pin = g.append(bytes(8))
    for node in nodes:
        g.set_u32(node + 0x10, 1)
        g.set_u32(node + 0x14, 1)
        g.set_u64(node + 0x18, (1 << 56) | (1 << 52) | 8)
        g.set_u64(node + 0x20, pin)
        g.add_site('ROFS', node + 0x20)
    return g.build()


PRIVATE_TEAM_SETTINGS_SHA256 = {
    'a2259e5840343814fa5901ce16de69a3c7295760a5ceb8ed51baf2176608fbc4',  # USA 1.21
    'e8667f50355f3b0cfa35df6b25f159715c068e53bba86cc1ebecb948d4a97cb4',  # EU 1.21
    '759c89e2d1d9f19e312cc25b009df3a0375497672c40507434c475c886892df9',  # USA widescreen
    '841c6f4ee3727b95ad6c3c9438020dda3e0fb401d4ca3e143549e4e7290db161',  # EU widescreen
}
# Single/CUP use the SingleRace settings animation. Give the existing Teams
# row its visible battle state and row spacing; keep native selector/sync logic.
PRIVATE_TEAM_ROWS = ((0x51e0, 0x4e80, 4), (0xe058, 0xdcd8, 12))


def private_team_settings(blob, native=None):
    """The private lobby settings with the Teams row shown for races. blob is a supported game
    version, or a mod's version of the menu (more lap counts, say) when native, the game's own
    menu it was made from, is: then the mod must keep native's objects, fixups and strings and
    leave both Teams rows as they are there, so the same change still lands on the same fields."""
    if hashlib.sha256(blob).hexdigest() not in PRIVATE_TEAM_SETTINGS_SHA256:
        if native is None or hashlib.sha256(native).hexdigest() not in PRIVATE_TEAM_SETTINGS_SHA256:
            raise ValueError('unsupported native private team settings')
        g, n = Igz(blob), Igz(native)
        fields = [(at + 0x18, size) for target, source, size in PRIVATE_TEAM_ROWS for at in (target, source)]
        if (g.objects() != n.objects() or g.fixups != n.fixups or g.strings != n.strings
                or any(g.data[at:at + size] != n.data[at:at + size] for at, size in fields)):
            raise ValueError('unsupported native private team settings: a mod changes its Teams row')
    g = Igz(blob)
    for target, source, size in PRIVATE_TEAM_ROWS:
        g.data[target + 0x18:target + 0x18 + size] = g.data[source + 0x18:source + 0x18 + size]
    return g.build()


def cup_timer(blob):
    g = Igz(blob)
    if (dict(g.objects()).get(0xc0) != 'igGuiPlaceable' or
            g.u64(0xc0 + 0xe8) != 0x6d0 or g.string_at(0x490 + 0x28) != 'TimerDesc'):
        raise ValueError('unsupported native return timer')
    g.set_u64(0xc0 + 0x28, g.string_index('OCTR_PRIVATE_RETURN_TIMER'))
    g.add_site('RSTT', 0xc0 + 0x28)
    return g.build()


def cup_route(blob, specification):
    digest, node, project, entering = specification
    if hashlib.sha256(blob).hexdigest() != digest:
        raise ValueError('unsupported native CUP navigation graph')
    g = Igz(blob)
    g.sites['RHND'] = g.read_list('RHND', unpack_offsets)
    if g.u32(node + 0x10) != 8:
        raise ValueError('unexpected native menu-open pin count')
    template = next(o for o, t in g.objects() if t == 'igVscConstObjectAccessor')
    names = g.fixup('EXNM')
    pairs = list(struct.iter_unpack('<II', names[3][:names[1] * 8]))
    handle = sum(bool(b & 0x80000000) for _, b in pairs)

    def named_pin(name, namespace):
        nonlocal handle
        pin = g.append(g.data[template:template + 24])
        g.add_site('RVTB', pin)
        names[3] = names[3][:names[1] * 8] + struct.pack(
            '<II', g.string_index(name), g.string_index(namespace) | 0x80000000)
        names[1] += 1
        g.set_u64(pin + 0x10, handle | 0x80000000)
        g.add_site('RHND', pin + 0x10)
        handle += 1
        return pin

    pin = named_pin(project, project)
    names[4] = names[2] + len(names[3])
    old = g.u64(node + 0x20)
    pins = [g.u64(old + i * 8) for i in range(8)] + [pin, 0]
    # The eleventh (unused) pin marks entry, which initializes native CUP mode.
    if entering:
        pins.append(0)
    elif node == 0x818:
        pins.extend((0, 0))  # native CUP accept, distinct from Back
    array = g.append(struct.pack(f'<{len(pins)}Q', *pins))
    for i, value in enumerate(pins):
        if value:
            g.add_site('ROFS', array + i * 8)
    g.set_u32(node + 0x10, len(pins))
    g.set_u32(node + 0x14, len(pins))
    g.set_u64(node + 0x18, (1 << 56) | (1 << 52) | len(pins) * 8)
    g.set_u64(node + 0x20, array)
    return g.build()


PACKAGE_PREFIX = 'temporary/octane/data/ps4/output/'
ONLINE_PACKAGE = PACKAGE_PREFIX + 'packages/generated/ui/domains/juicedomain_onlinemenus_pkg.igz'
START_PACKAGE = PACKAGE_PREFIX + 'packages/generated/ui/domains/juicedomain_startscreen_pkg.igz'
def package_rows(g):
    chunks = [o for o, t in g.objects() if t == 'igStreamingChunkInfo']
    if len(chunks) != 1:
        raise ValueError('expected one native streaming chunk')
    chunk = chunks[0]
    count, array = g.u64(chunk + 0x28), g.u64(chunk + 0x38)
    if g.u32(chunk + 0x30) != count * 16:
        raise ValueError('unexpected native streaming file array')
    return chunk, [(g.string_at(array + i * 16), g.string_at(array + i * 16 + 8))
                   for i in range(count)]


def cup_package(game, blob, source_blob, assets=None, race_blob=None):
    g, source = Igz(blob), Igz(source_blob)
    chunk, original = package_rows(g)
    _, source_rows = package_rows(source)
    if race_blob is not None:
        _, race_rows = package_rows(Igz(race_blob))
        paths = {path for _, path in source_rows}
        source_rows += [(kind, path) for kind, path in race_rows if path not in paths]
    source_types = {path: kind for kind, path in source_rows}
    seen = {path for _, path in original}
    # Match the native arcade package's dependency closure. Shared/global assets
    # absent from that package remain owned by their existing global chunks.
    files = {}
    for name in ('juicedomain_startscreen', 'juicedomain_race', 'update', 'update2'):
        pak = resolve(Path(game) / 'archives' / (name + '.pak'))
        for entry in iga.pak_entries(pak):
            files[entry[0].lower().removeprefix(PACKAGE_PREFIX)] = (pak, entry)
    pending = ['ui/octane_cupselect.igz', 'materialinstances/ui/cupselect_materials.igz']
    # Race owns its result projects; dedicated StreamingUI archives cover fallback opens.
    needed = set()
    while pending:
        path = pending.pop()
        if path in seen or path not in source_types:
            continue
        # Package manifests can name languages absent from this regional dump.
        # Keep English and non-language dependencies mandatory.
        if (path not in files and source_types[path] == 'lang_file'
                and path.endswith('.lng') and not path.endswith('_en.lng')):
            continue
        seen.add(path)
        needed.add(path)
        pak, entry = files[path]
        # The Arcade domain archive is released when entering Online. Keep its
        # required files available through the runtime's existing loose-file map.
        if assets is not None and pak.name.lower() in ('juicedomain_startscreen.pak', 'juicedomain_race.pak'):
            target = long_path(assets / path)
            target.parent.mkdir(parents=True, exist_ok=True)
            with pak.open('rb') as f:
                target.write_bytes(iga.read_entry(f, *entry[1:]))
        if source_types[path] == 'texture':
            continue
        with pak.open('rb') as f:
            asset = Igz(iga.read_entry(f, *entry[1:]))
        try:
            dependencies = asset.fixup('TDEP')
        except KeyError:
            continue
        strings = dependencies[3].split(b'\0')[:dependencies[1] * 2]
        for dependency in strings[1::2]:
            pending.append(dependency.decode().lower().replace(
                '<build>project:/' + PACKAGE_PREFIX, '').replace(':/', '/'))
    # Keep native ownership; imported VSC types remain shared through TDEP.
    rows = original + [(kind, path) for kind, path in source_rows
                       if path in needed and kind != 'vsc']
    array = g.append(b''.join(struct.pack('<QQ', g.string_index(kind), g.string_index(path))
                              for kind, path in rows))
    for offset in range(array, array + len(rows) * 16, 8):
        g.add_site('RSTT', offset)
    g.set_u64(chunk + 0x28, len(rows))
    g.set_u64(chunk + 0x30, (g.u64(chunk + 0x30) & ~0xffffffff) | len(rows) * 16)
    g.set_u64(chunk + 0x38, array)
    return g.build()


def package_with_rows(blob, additions):
    g = Igz(blob)
    chunk, rows = package_rows(g)
    paths = {path for _, path in rows}
    rows += [row for row in additions if row[1] not in paths]
    array = g.append(b''.join(struct.pack('<QQ', g.string_index(kind), g.string_index(path))
                              for kind, path in rows))
    for offset in range(array, array + len(rows) * 16, 8):
        g.add_site('RSTT', offset)
    g.set_u64(chunk + 0x28, len(rows))
    g.set_u64(chunk + 0x30, (g.u64(chunk + 0x30) & ~0xffffffff) | len(rows) * 16)
    g.set_u64(chunk + 0x38, array)
    return g.build()


def ranking_stream_archive(game, dest, package_blob, project, replacements, resident=()):
    """Provide the native StreamingUI fallback when the online chunk was released."""
    from graphics_tab import Archive, write_archive
    from contextlib import ExitStack
    source_names = ('juicedomain_startscreen', 'juicedomain_race', 'juicedomain_onlinemenus', 'update', 'update2')
    sources = [resolve(Path(game) / 'archives' / (name + '.pak')) for name in source_names]
    fingerprint = hashlib.sha256()
    fingerprint.update(Path(__file__).read_bytes())
    fingerprint.update(package_blob)
    fingerprint.update(repr(sorted(resident)).encode())
    for path in sources:
        stat = path.stat()
        fingerprint.update(json.dumps((str(path), stat.st_size, stat.st_mtime_ns)).encode())
    for path, data in sorted(replacements.items()):
        fingerprint.update(path.encode())
        fingerprint.update(data)
    key = dest.with_suffix('.cache-key')
    digest = fingerprint.hexdigest()
    if dest.is_file() and key.is_file() and key.read_text() == digest:
        return
    package = Igz(package_blob)
    chunk, rows = package_rows(package)
    kinds = dict((path, kind) for kind, path in rows)
    with ExitStack() as stack:
        files = {}
        template = None
        for name in source_names:
            archive = Archive(resolve(Path(game) / 'archives' / (name + '.pak')))
            stack.callback(archive.f.close)
            template = archive
            for i in range(len(archive.names)):
                path = archive.logical(i).lower().removeprefix(PACKAGE_PREFIX)
                files[path] = (archive.f.name, i)
                if path.startswith('vsc/') and path.endswith('.igz'):
                    kinds.setdefault(path, 'vsc')
        pending = ['ui/' + project + '.igz']
        needed = {}
        while pending:
            path = pending.pop()
            if path in needed or path not in kinds or path in resident:
                continue  # Other dependencies belong to native permanent chunks.
            data = replacements.get(path)
            if path not in files and data is None:
                # As in the Cup assets: a manifest can name languages this regional dump lacks
                if kinds[path] == 'lang_file' and path.endswith('.lng') and not path.endswith('_en.lng'):
                    continue
                raise ValueError('missing ranking dependency: ' + path)
            if data is None:
                archive_path, index = files[path]
                archive = Archive(Path(archive_path))
                try:
                    data = archive.read(index)
                finally:
                    archive.f.close()
            needed[path] = data
            if kinds[path] in ('texture', 'lang_file'):
                continue
            asset = Igz(data)
            try:
                dependencies = asset.fixup('TDEP')
            except KeyError:
                continue
            strings = dependencies[3].split(b'\0')[:dependencies[1] * 2]
            for raw in strings[1::2]:
                dependency = raw.decode().lower().replace('<build>project:/' + PACKAGE_PREFIX, '')
                if dependency.startswith('<build>materials:/'):
                    dependency = 'materialinstances/' + dependency.removeprefix('<build>materials:/')
                else:
                    dependency = dependency.replace(':/', '/')
                pending.append(dependency)
        # Shared VSC definitions load through TDEP; a second chunk must not own their metadata.
        selected = [(kind, path) for kind, path in rows if path in needed and kind != 'vsc']
        array = package.append(b''.join(struct.pack('<QQ', package.string_index(kind),
                                                    package.string_index(path)) for kind, path in selected))
        for offset in range(array, array + len(selected) * 16, 8):
            package.add_site('RSTT', offset)
        package.set_u64(chunk + 0x28, len(selected))
        package.set_u64(chunk + 0x30, (package.u64(chunk + 0x30) & ~0xffffffff) | len(selected) * 16)
        package.set_u64(chunk + 0x38, array)
        # 985510 requests generated/UI/<project>; the native package loader
        # adds packages/ and _pkg.igz. The root export remains chunk_info.
        package_path = 'packages/generated/ui/' + project + '_pkg.igz'
        needed[package_path] = package.build()
        write_archive(dest, [(PACKAGE_PREFIX + path, path, i << 8, data)
                             for i, (path, data) in enumerate(needed.items())], template)
    key.write_text(digest)


def hook_site(elf):
    hits = [v + m.start() for v, off, size in load_segments(elf)
            for m in HOOK_PATTERN.finditer(elf[off:off + size])
            if v + m.start() + 17 + struct.unpack_from('<i', m.group(), 13)[0] == 0x2af1fa8]
    if len(hits) != 1 or hits[0] not in (0x1277e00, 0x1277df0):
        raise ValueError('native Online selector adapter is not uniquely recognized')
    return hits[0]


def set_options(g, options, enabled, labels):
    for lst, raw, tag in (
        (options, b''.join(struct.pack('<Q', g.string_index(s)) for s in labels), 'RSTT'),
        (enabled, bytes([1] * len(labels)), None),
    ):
        array = g.append(raw)
        g.set_u32(lst + 0x0c, len(labels))
        g.set_u32(lst + 0x10, len(labels))
        g.set_u64(lst + 0x18, (g.u64(lst + 0x18) & ~0xffffffff) | (1 << 56) | len(raw))
        g.set_u64(lst + 0x20, array)
        g.add_site('ROFS', lst + 0x20)
        if tag:
            for i in range(len(labels)):
                g.add_site(tag, array + i * 8)


def add_race_type(g):
    # Copy the native selector, including its timer, animation categories and fixups.
    for tag in ('RHND', 'RNEX'):
        g.sites[tag] = g.read_list(tag, unpack_offsets)
    blocks = ((0x1000, 0x1120), (0x1648, 0x18e8))
    copies = [(lo, hi, g.append(g.data[lo:hi])) for lo, hi in blocks]

    def relocated(value):
        for lo, hi, new in copies:
            if lo <= value < hi:
                return new + value - lo
        return value

    for tag, sites in g.sites.items():
        for site in list(sites):
            target = relocated(site)
            if target != site:
                g.add_site(tag, target)
                if tag == 'ROFS':
                    g.set_u64(target, relocated(g.u64(site)))
    row, behavior = relocated(0x1000), relocated(0x1678)
    g.set_u64(behavior + 0x148, g.string_index('RACE TYPE'))
    g.add_site('RSTT', behavior + 0x148)
    set_options(g, relocated(0x1880), relocated(0x18a8), ('SINGLE', 'CUP'))
    mode_y = struct.unpack_from('<f', g.data, 0x1064)[0]
    lobby_y = struct.unpack_from('<f', g.data, 0xb74)[0]
    # Both native rows share an unshifted parent: preserve their existing pitch.
    struct.pack_into('<f', g.data, row + 0x64, mode_y + (mode_y - lobby_y))
    # Start hidden; the runtime reveals it only in private Normal/Speed matches.
    g.set_u32(row + 0x0c, g.u32(row + 0x0c) | 0x400)
    g.set_u64(row + 0x50, 0x1000)
    g.set_u64(0x1000 + 0x58, row)
    g.set_u64(0x1120 + 0x50, row)
    g.set_u64(behavior + 0x110, 0x1000)
    # Add the row to the native child table as well as its traversal chain.
    children = g.append(struct.pack('<3Q', 0x1000, row, 0x1120))
    for i in range(3):
        g.add_site('ROFS', children + 8 * i)
    g.set_u32(0xfc0 + 0x0c, 3)
    g.set_u32(0xfc0 + 0x10, 3)
    g.set_u64(0xfc0 + 0x18, (1 << 56) | (1 << 52) | 24)
    g.set_u64(0xfc0 + 0x20, children)


def rebuild(blob):
    if hashlib.sha256(blob).hexdigest() not in GRAPH_SHA256:
        raise ValueError('native Online mode selector is not the supported USA/EU 1.21 asset')
    g = Igz(blob)
    add_race_type(g)
    # The native selector consumes the same option/availability lists as other selectors.
    set_options(g, 0x1880, 0x18a8, ('NORMAL', 'ITEMLESS', 'BATTLE'))
    return g.build()


def lobby_card(blob):
    if hashlib.sha256(blob).hexdigest() not in {
        'bc50c63007445a18a1d57c9c4b1176428330e6686bb4943a86f09463d922f6ea',
        'a468bac4f2141363d934a3ea2cb6540908e0d6481214890ed8a3807caa191c37',
        'bcce57356cb91b670e8e976a3e8db45db7594cb84f108823b903a74ba6713726',  # USA widescreen
        '63c880cd5278e9e169e765ddbd05299420b9bf896c80f8a1d017bdf1df3aa82c',  # EU widescreen
    }:
        raise ValueError('unsupported native private lobby card')
    g = Igz(blob)
    # Run the existing hooked native behavior after every VSC settings binding.
    behaviors = g.u64(0x480 + 0xe8)
    slots = g.u64(behaviors + 0x20)
    count = g.u32(behaviors + 12)
    values = [g.u64(slots + i * 8) for i in range(count)]
    types = dict(g.objects())
    hooks = [v for v in values if types.get(v) == 'CGuiBehaviorReturnToMainMenuOnDisconnect']
    if len(hooks) != 1:
        raise ValueError('unsupported private lobby event order')
    values = [v for v in values if v != hooks[0]] + hooks
    for i, value in enumerate(values):
        g.set_u64(slots + i * 8, value)
    # Runtime markers identify this menu and its existing postcard.
    for placeable, name in ((0x480, 'OCTR_PRIVATE_LOBBY'),
                            (0x4180, 'OCTR_PRIVATE_PREVIEW')):
        if g.u64(placeable + 0x28):
            raise ValueError('private lobby marker would overwrite a native binding')
        g.set_u64(placeable + 0x28, g.string_index(name))
        g.add_site('RSTT', placeable + 0x28)
    return g.build()


def public_lobby_card(blob):
    if hashlib.sha256(blob).hexdigest() not in {
        'b57ea5c3a619e01b2439b08d2c4e07905b6aad4b8c1efd35f8c5aa00e015c492',
        'df61f393a9da879a82154a634d1431b2bba31e303543ad106a3b2ef5edd28bb8',
        '9e17b0b1575d35768cca8c6235642867df5ebb94a6ba22bbe5b15fad40b306cc',  # USA widescreen
        '1af38cd0fcb77576ec6276a7e78732409ac8ceded2aad2934d8ca0ee34388884',  # EU widescreen
    }:
        raise ValueError('unsupported native public lobby card')
    g = Igz(blob)
    root = 0x5f0
    behaviors = g.u64(root + 0xe8)
    slots = g.u64(behaviors + 0x20)
    values = [g.u64(slots + i * 8) for i in range(g.u32(behaviors + 12))]
    types = dict(g.objects())
    hooks = [v for v in values if types.get(v) == 'CGuiBehaviorReturnToMainMenuOnDisconnect']
    if len(hooks) != 1 or g.u64(root + 0x28):
        raise ValueError('unsupported public lobby event order or marker')
    # Decorate vote cards after the native bindings, using the existing runtime hook.
    for i, value in enumerate([v for v in values if v != hooks[0]] + hooks):
        g.set_u64(slots + i * 8, value)
    g.set_u64(root + 0x28, g.string_index('OCTR_PUBLIC_LOBBY'))
    g.add_site('RSTT', root + 0x28)
    return g.build()


def ranked_main_button(blob):
    """Place Ranked and HALL OF FAME buttons below Matchmaking."""
    g = Igz(blob)
    if (g.u64(0x970 + 0xe8) != 0x2110 or g.u64(0x970 + 0x50) != 0xa90 or
            g.u64(0xa90 + 0x58) != 0x970 or g.u64(0x7e0 + 0x48) != 0x900 or
            g.u32(0x900 + 12) != 8 or g.u64(0x2110 + 0x28) != 0x2148):
        raise ValueError('unsupported native main lobby matchmaking button')
    references = {tag: g.read_list(tag, unpack_offsets) for tag in ('RHND', 'RNEX')}
    def duplicate(with_vsc):
        # Keep each button's VSC behavior instance separate so focus returns to its owner.
        copies = [(0x970, 0xa90, g.append(g.data[0x970:0xa90])),
                  (0x2110, 0x2368, g.append(g.data[0x2110:0x2368]))]
        state_end = 0x2410 if with_vsc else 0x23b8
        copies.append((0x2368, state_end, g.append(g.data[0x2368:state_end])))

        def moved(value):
            return next((new + value - lo for lo, hi, new in copies
                         if lo <= value < hi), value)

        for tag, sites in (*g.sites.items(), *references.items()):
            for site in list(sites):
                target = moved(site)
                if target == site:
                    continue
                if tag in references:
                    references[tag].append(target)
                else:
                    g.add_site(tag, target)
                if tag == 'ROFS':
                    g.set_u64(target, moved(g.u64(site)))
        return moved(0x970), moved(0x2148), moved(0x2110)

    ranked, ranked_nav, _ = duplicate(True)
    hall, hall_nav, hall_list = duplicate(True)
    for button, name in ((ranked, 'OCTR_RANKED_BUTTON'),
                         (hall, 'OCTR_HALL_OF_FAME_BUTTON')):
        g.set_u64(button + 0x28, g.string_index(name))
        g.add_site('RSTT', button + 0x28)
    g.set_u64(0x970 + 0x50, ranked)
    g.set_u64(ranked + 0x58, 0x970)
    g.set_u64(ranked + 0x50, hall)
    g.set_u64(hall + 0x58, ranked)
    g.set_u64(hall + 0x50, 0xa90)
    g.set_u64(0xa90 + 0x58, hall)
    struct.pack_into('<f', g.data, 0x970 + 0x64, -315.0)
    struct.pack_into('<f', g.data, ranked + 0x64, -210.0)
    struct.pack_into('<f', g.data, hall + 0x64, -105.0)
    for button in (0xa90, 0xf50, 0x1190, 0x1070, 0x6c0, 0x2580, 0x26a0):
        y = struct.unpack_from('<f', g.data, button + 0x64)[0]
        struct.pack_into('<f', g.data, button + 0x64, y + 105.0)
    g.set_u64(0x2148 + 0x118, ranked)
    g.set_u64(ranked_nav + 0x110, 0x970)
    g.set_u64(ranked_nav + 0x118, hall)
    g.set_u64(hall_nav + 0x110, ranked)
    g.set_u64(hall_nav + 0x118, 0xa90)
    g.set_u64(0xd08 + 0x110, hall)
    exid = g.fixup('EXID')
    quick = next(i for i, (name, _) in enumerate(
        struct.iter_unpack('<II', exid[3][:exid[1] * 8]))
        if name == fnv1a('octane_ui_ico_quick_tga__material'))
    g.set_u64(ranked_nav + 0x158, quick)
    g.set_u64(ranked_nav + 0x1a8, quick)
    native_background = next(space for name, space in struct.iter_unpack(
        '<II', exid[3][:exid[1] * 8])
        if name == fnv1a('octane_ui_online_bg_pattern_matchmaking_tga__material'))
    if native_background != fnv1a(RANKED_BACKGROUND_NAMESPACE):
        raise ValueError('unsupported native main lobby material namespace')
    g.set_u64(ranked_nav + 0x178, exid[1])
    g.set_u64(hall_nav + 0x178, exid[1] + 1)
    g.set_u64(hall_nav + 0x158, exid[1] + 2)
    g.set_u64(hall_nav + 0x1a8, exid[1] + 2)
    exid[3] = (exid[3][:exid[1] * 8] + b''.join(
        struct.pack('<II', fnv1a(name), native_background)
        for name in (RANKED_BACKGROUND_MATERIAL, HOF_BACKGROUND_MATERIAL,
                     HOF_ICON_MATERIAL)))
    exid[1] += 3
    exid[4] = (exid[2] + len(exid[3]) + 3) & ~3

    children = [g.u64(0x928 + i * 8) for i in range(8)]
    children[children.index(0x970) + 1:children.index(0x970) + 1] = [ranked, hall]
    array = g.append(struct.pack('<10Q', *children))
    for i in range(10):
        g.add_site('ROFS', array + i * 8)
    g.set_u32(0x900 + 12, 10)
    g.set_u32(0x900 + 16, 10)
    g.set_u64(0x900 + 0x18, (g.u64(0x900 + 0x18) & ~0xffffffff) | 80)
    g.set_u64(0x900 + 0x20, array)

    for tag, sites in references.items():
        sites = sorted(set(sites))
        fixup = g.fixup(tag)
        fixup[1], fixup[3] = len(sites), pack_offsets(sites)
        fixup[4] = (fixup[2] + len(fixup[3]) + 3) & ~3
    return g.build()


def ranked_mode_select(blob):
    """Add a Ranked-only header above ModeSelect's native buttons."""
    g = Igz(blob)
    objects = dict(g.objects())
    handles = g.fixup('EXNM')
    pairs = list(struct.iter_unpack('<II', handles[3][:handles[1] * 8]))
    external = [g.strings[a] for a, b in pairs if b & 0x80000000]
    def asset(off):
        value = g.u64(off + 0x38)
        return external[value & 0x7fffffff] if value & 0x80000000 else None
    placeables = [off for off, kind in objects.items() if kind == 'igGuiPlaceable']
    buttons = next(off for off in placeables if asset(off) == 'Buttons')
    root = g.u64(buttons + 0x40)
    text_source = next(off for off in placeables if asset(off) == 'Text')
    badge_source = next(off for off in placeables
                        if asset(off) == 'octane_ui_raceselect_title_separator_tga')
    root_table = g.u64(root + 0x48)
    siblings = [g.u64(g.u64(root_table + 0x20) + i * 8)
                for i in range(g.u32(root_table + 0x0c))]
    if buttons not in siblings or len(siblings) != 4:
        raise ValueError('unsupported native ModeSelect root layout')
    button_table = g.u64(buttons + 0x48)
    if g.u32(button_table + 0x0c) != 1:
        raise ValueError('unsupported native ModeSelect button layout')
    header_parent = g.u64(g.u64(button_table + 0x20))
    header_table = g.u64(header_parent + 0x48)
    button_children = [g.u64(g.u64(header_table + 0x20) + i * 8)
                       for i in range(g.u32(header_table + 0x0c))]
    if len(button_children) != 3:
        raise ValueError('unsupported native ModeSelect button children')
    references = {tag: g.read_list(tag, unpack_offsets) for tag in ('RHND', 'RNEX')}
    def pointer(at, value):
        g.set_u64(at, value)
        if value: g.add_site('ROFS', at)
        elif at in g.sites['ROFS']: g.sites['ROFS'].remove(at)
    def duplicate(source):
        size = g.sizes[g.u64(source)]
        clone = g.append(g.data[source:source + size])
        for tag, sites in (*g.sites.items(), *references.items()):
            for site in list(sites):
                if source <= site < source + size:
                    target = clone + site - source
                    if tag in references: references[tag].append(target)
                    else: g.add_site(tag, target)
        pointer(clone + 0x40, header_parent)
        pointer(clone + 0x48, 0)
        pointer(clone + 0xf0, 0)
        g.set_u32(clone + 0x0c, g.u32(clone + 0x0c) | 0x400)
        return clone
    x = -10.0
    new = []
    handle_index = sum(bool(space & 0x80000000) for _, space in pairs)
    glow = duplicate(badge_source)
    g.set_u64(glow + 0x28, g.string_index('OCTR_RANK_GLOW'))
    g.add_site('RSTT', glow + 0x28)
    g.set_u64(glow + 0x38, 0x80000000 | handle_index)
    pairs.append((g.string_index('ctr_gui_wumpa_juiced_vfx'),
                  g.string_index('GPChallengeGroup') | 0x80000000))
    handle_index += 1
    struct.pack_into('<2f', g.data, glow + 0x60, x, -360.0)
    struct.pack_into('<2f', g.data, glow + 0x90, 100.0, 100.0)
    struct.pack_into('<2f', g.data, glow + 0x84, 1.5, 1.5)
    struct.pack_into('<f', g.data, glow + 0xe0, 1.0)
    new.append(glow)
    for mode, tier in TIERS:
        badge = duplicate(badge_source)
        g.set_u64(badge + 0x28, g.string_index(f'OCTR_CARD_ICON_{mode}_{tier}'))
        g.add_site('RSTT', badge + 0x28)
        g.set_u64(badge + 0x38, 0x80000000 | handle_index)
        pairs.append((g.string_index(f'OCTR_RANK_{mode}_{tier}_ASSET'),
                      g.string_index('Octane_LobbyMembers') | 0x80000000))
        handle_index += 1
        struct.pack_into('<2f', g.data, badge + 0x60, x, -360.0)
        struct.pack_into('<2f', g.data, badge + 0x90, 256.0, 256.0)
        struct.pack_into('<2f', g.data, badge + 0x84, 3.2, 3.2)
        struct.pack_into('<f', g.data, badge + 0xe0, 1.0)
        new.append(badge)
    backdrop = duplicate(badge_source)
    g.set_u64(backdrop + 0x28, g.string_index('OCTR_RANK_PLACEMENT_BG'))
    g.add_site('RSTT', backdrop + 0x28)
    g.set_u64(backdrop + 0x38, 0x80000000 | handle_index)
    pairs.append((g.string_index('octane_pit_stop_price_tag_tga'),
                  g.string_index('WumpaCoinsCounter') | 0x80000000))
    struct.pack_into('<2f', g.data, backdrop + 0x60, x, -45.0)
    struct.pack_into('<2f', g.data, backdrop + 0x90, 205.0, 50.0)
    struct.pack_into('<f', g.data, backdrop + 0x80, -7.0)
    struct.pack_into('<f', g.data, backdrop + 0xe0, 1.0)
    new.append(backdrop)
    for tag, height, color in (('OCTR_RANK_NAME', -165.0, (1.0, 0.52, 0.0, 1.0)),
                               ('OCTR_RANK_MMR', -115.0, (1.0, 1.0, 1.0, 1.0)),
                               ('OCTR_RANK_PLACEMENT', -45.0, (1.0, 1.0, 1.0, 1.0))):
        for shadow in (True, False):
            label = duplicate(text_source)
            g.set_u64(label + 0x28, g.string_index(tag + ('_SHADOW' if shadow else '')))
            g.add_site('RSTT', label + 0x28)
            struct.pack_into('<2f', g.data, label + 0x60,
                                 x + (2.0 if shadow else 0.0), height + (2.0 if shadow else 0.0))
            struct.pack_into('<4f', g.data, label + 0xc0,
                             *(0.0, 0.0, 0.0, 1.0) if shadow else color)
            struct.pack_into('<f', g.data, label + 0xe0, 1.0)
            new.append(label)
    children = button_children + new
    data = g.append(struct.pack(f'<{len(children)}Q', *children))
    for i in range(len(children)): g.add_site('ROFS', data + i * 8)
    g.set_u32(header_table + 0x0c, len(children))
    g.set_u32(header_table + 0x10, len(children))
    g.set_u64(header_table + 0x18, (g.u64(header_table + 0x18) & ~0xffffffff) | len(children) * 8)
    pointer(header_table + 0x20, data)
    tail = g.u64(header_parent + 0x58)
    for item in new:
        pointer(tail + 0x50, item)
        pointer(item + 0x58, tail)
        tail = item
    pointer(tail + 0x50, header_parent)
    pointer(header_parent + 0x58, tail)
    handles[1] = len(pairs)
    handles[3] = b''.join(struct.pack('<II', *pair) for pair in pairs)
    handles[4] = (handles[2] + len(handles[3]) + 3) & ~3
    dependencies = g.fixup('TDEP')
    entries = dependencies[3].split(b'\0')[:dependencies[1] * 2]
    entries += [b'WumpaCoinsCounter', b'ui:/GuiSharedAssets/WumpaCoinsCounter.igz',
                b'GPChallengeGroup', b'ui:/GuiSharedAssets/GPChallengeGroup.igz']
    dependencies[1] += 2
    dependencies[3] = b'\0'.join(entries) + b'\0'
    dependencies[4] = (dependencies[2] + len(dependencies[3]) + 3) & ~3
    for tag, sites in references.items():
        fixup = g.fixup(tag)
        sites = sorted(set(sites))
        fixup[1], fixup[3] = len(sites), pack_offsets(sites)
        fixup[4] = (fixup[2] + len(fixup[3]) + 3) & ~3
    return g.build()

def current_entry(pak, entries, index, base):
    """The entry's bytes as the game reads them now: base's own replacement (a widescreen layout's
    menu/HUD fix, kept uncompressed) when it already has one for this entry, else the archive's
    own (decompressed as the game would)."""
    if base is not None:
        _size, _guest, ranges = archive_patch.read(base)
        count = len(entries)
        record_at = 0x38 + 4 * count + 16 * index
        record = next((d for o, d in ranges if o == record_at), None)
        if record is not None:
            offset_lo, hi_extra, length, _mode = struct.unpack('<4I', record)
            position = offset_lo | ((hi_extra & 0xFF) << 32)
            return archive_patch.view(pak, base, position, length)
    with pak.open('rb') as f:
        return iga.read_entry(f, *entries[index][1:])



CHARACTERS_ENTRY = 'temporary/octane/data/ps4/output/characterlisting.igz'


def known_heroes(game):
    """The game's own racers (DriverCrash, DriverSmallNorm...), from its character listing in the
    newest archive that has one. Another player's racer outside it comes from a mod this game
    lacks: src/runtime_online_mode.c shows it as Crash, or the lobby waits for it forever."""
    for name in ('update2', 'update', 'permanent'):
        try:
            pak = resolve(Path(game) / 'archives' / (name + '.pak'))
            entry = next((e for e in iga.pak_entries(pak) if e[0].lower() == CHARACTERS_ENTRY), None)
            if entry is None:
                continue
            with pak.open('rb') as f:
                strings = Igz(iga.read_entry(f, *entry[1:])).strings
        except (OSError, ValueError, KeyError, IndexError):
            continue
        heroes = sorted({s for s in strings if re.fullmatch(r'Driver[A-Z][A-Za-z0-9]{1,30}', s)})
        if 'DriverCrash' in heroes:
            return ','.join(heroes)
    return ''


def permanent_ui_package(game, out, patches, mapped):
    """Let the native permanent chunk own UI definitions and their dependencies."""
    from contextlib import ExitStack
    from graphics_tab import Archive
    names = ('permanent', 'juicedomain_permanent', 'juicedomain_startscreen',
             'juicedomain_race', 'juicedomain_onlinemenus', 'update', 'update2')
    permanent_path = 'packages/generated/ui/domains/juicedomain_permanent_pkg.igz'
    with ExitStack() as stack:
        files, overrides = {}, {}
        for name in names:
            archive = Archive(resolve(Path(game) / 'archives' / (name + '.pak')))
            stack.callback(archive.f.close)
            guest = '/app0/archives/' + name + '.pak'
            if base := patches.get(guest):
                entries = iga.pak_entries(Path(archive.f.name))
                ranges = archive_patch.read(base)[2]
                records = {offset for offset, _ in ranges}
                for index in range(len(entries)):
                    if 0x38 + 4 * len(entries) + 16 * index in records:
                        overrides[archive, index] = current_entry(Path(archive.f.name), entries, index, base)
            for index in range(len(archive.names)):
                path = archive.logical(index).lower().removeprefix(PACKAGE_PREFIX)
                files[path] = (archive, index)

        def read(path):
            archive, index = files[path]
            if (archive, index) in overrides:
                return overrides[archive, index]
            return archive.read(index)

        kinds, roots = {}, set()
        for domain in ('permanent', 'startscreen', 'race', 'onlinemenus'):
            path = f'packages/generated/ui/domains/juicedomain_{domain}_pkg.igz'
            _, rows = package_rows(Igz(read(path)))
            kinds.update((path, kind) for kind, path in rows)
            roots.update(path for kind, path in rows if kind == 'vsc' and
                         path.startswith(('vsc/ui/', 'vsc/common/ui/')))
        pending, needed = sorted(roots), {}
        while pending:
            path = pending.pop()
            if path in needed or path not in kinds:
                continue  # Dependencies outside these chunks already have native owners.
            data = read(path)
            needed[path] = data
            if kinds[path] in ('texture', 'lang_file', 'sound_stream'):
                continue
            try:
                fixup = Igz(data).fixup('TDEP')
            except KeyError:
                continue
            for raw in fixup[3].split(b'\0')[1:fixup[1] * 2:2]:
                dependency = raw.decode().lower().removeprefix('<build>project:/' + PACKAGE_PREFIX)
                dependency = dependency.replace(':/', '/')
                if dependency.startswith('<build>data/'):
                    dependency = dependency.removeprefix('<build>data/')
                if dependency.startswith('<build>materials/'):
                    dependency = 'materialinstances/' + dependency.removeprefix('<build>materials/')
                pending.append(dependency)
        permanent = package_with_rows(read(permanent_path),
                                      [(kinds[path], path) for path in sorted(needed)])
        archive, index = files[permanent_path]
        guest = '/app0/archives/' + Path(archive.f.name).name
        target = out / 'permanent-ui.bbpatch'
        archive_patch.write_entries_patch(target, Path(archive.f.name), guest,
                                          {index: permanent}, patches.get(guest), override={index})
        patches[guest] = target
        for path, data in needed.items():
            if path in mapped:
                continue
            dest = long_path(out / 'permanent-ui-assets' / path)
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
            mapped[path] = dest


def prepare(game, out, elf, bases):
    """Reuse complete Online UI output while its sources, overlays and files match."""
    import os
    from speed_mode import stamp_of
    from cache_cleanup import discard_files
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    cache = out / 'cache.json'
    digest = hashlib.sha256(stamp_of(game, bases).encode())
    digest.update(str(out.resolve()).encode())
    digest.update(Path(elf).read_bytes())
    here = Path(__file__).resolve().parent
    for source in sorted([*here.glob('*.py'), *(here / 'assets').rglob('*')]):
        if source.is_file():
            digest.update(str(source.relative_to(here)).encode())
            digest.update(source.read_bytes())
    _, mapped = records()
    inputs = sorted((str(name), str(Path(path).resolve())) for name, path in mapped.items())
    graphics = os.environ.get('BB_GRAPHICS_TAB_ARCHIVE')
    if graphics:
        inputs.append(('graphics', str(Path(graphics).resolve())))
    def identity(path):
        stat = Path(path).stat()
        return [str(Path(path).resolve()), stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns]
    digest.update(json.dumps([(name, identity(path)) for name, path in inputs]).encode())
    stamp = digest.hexdigest()
    kept = {}
    try:
        kept = json.loads(cache.read_text())
        if (kept['stamp'] == stamp and kept['files'] and
                all(identity(row[0]) == row for row in kept['files'])):
            print('Online UI: cache hit')
            return [Path(path) for path in kept['patches']], kept['hook']
    except (OSError, ValueError, KeyError, TypeError, IndexError):
        pass
    if isinstance(kept, dict):
        protected = {Path(path).resolve() for path in [elf, *bases]}
        discard_files(out, [row[0] for row in kept.get('files', [])
                            if isinstance(row, list) and row and Path(row[0]).resolve() not in protected])
    cache.unlink(missing_ok=True)
    patches, hook = _prepare(game, out, elf, bases)
    files = [identity(path) for path in sorted(out.rglob('*'))
             if path.is_file() and path not in (cache, cache.with_suffix('.tmp'))]
    temporary = cache.with_suffix('.tmp')
    temporary.write_text(json.dumps({'stamp': stamp, 'files': files,
                                    'patches': [str(path) for path in patches], 'hook': hook}))
    temporary.replace(cache)
    print('Online UI: cache rebuilt')
    return patches, hook


def _prepare(game, out, elf, bases):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    assets = out / 'cup-assets'
    (assets / SINGLE_RACE_TEXTURE).unlink(missing_ok=True)
    (assets / 'vsc/ui/octane_lobby_main/octr_halloffamebutton.igz').unlink(missing_ok=True)
    (assets / 'vsc/ui/octane_lobby_privategame/octane_lobby_privategame_teambutton.igz').unlink(missing_ok=True)
    site = hook_site(Path(elf).read_bytes())
    pak = resolve(Path(game) / 'archives/update2.pak')
    entries = iga.pak_entries(pak)
    index = next((i for i, e in enumerate(entries) if e[0].lower() == ENTRY), None)
    if index is None:
        raise ValueError('native Online menu graph is missing')
    guest = '/app0/archives/update2.pak'
    patches = {archive_patch.read(p)[1]: Path(p) for p in bases}
    member_material_pak = resolve(Path(game) / 'archives/update.pak')
    member_material_entries = iga.pak_entries(member_material_pak)
    member_material_index = next(i for i, e in enumerate(member_material_entries)
                                 if e[0].lower() == LOBBY_MEMBERS_MATERIALS_ENTRY)
    member_materials = clone_lobby_rank_materials(current_entry(
        member_material_pak, member_material_entries, member_material_index,
        patches.get('/app0/archives/update.pak')))
    base = patches.get(guest)
    payload = ranked_mode_select(rebuild(current_entry(pak, entries, index, base)))
    with pak.open('rb') as f:
        ranking_ui_index = next(i for i, e in enumerate(entries) if e[0].lower() == TEAM_RANKING_UI)
        race_ui_index = next(i for i, e in enumerate(entries) if e[0].lower() ==
                             PACKAGE_PREFIX + 'ui/octane_race_ranking.igz')
        ranking_ui = team_ranking_header(current_entry(pak, entries, ranking_ui_index, base))
        reward_index = next(i for i,e in enumerate(entries) if e[0].lower().endswith('/ui/octane_aar_wumpaonly.igz'))
        score_ui = score_card(current_entry(pak, entries, race_ui_index, base), iga.read_entry(f, *entries[reward_index][1:]))
        ranking_index = next(i for i, e in enumerate(entries) if e[0].lower() == TEAM_RANKING_ROOT)
        ranking = team_ranking_navigation(iga.read_entry(f, *entries[ranking_index][1:]))
        settings_index = next(i for i, e in enumerate(entries) if e[0].lower() == SETTINGS_ENTRY)
        # A mod's version of the menu is checked against the game's own (private_team_settings).
        settings = private_team_settings(current_entry(pak, entries, settings_index, base),
                                         iga.read_entry(f, *entries[settings_index][1:]))
        lobby_index = next(i for i, e in enumerate(entries) if e[0].lower() == LOBBY_ENTRY)
        # Preserve the widescreen lobby layout before adding Online's tags/order.
        lobby = lobby_card(current_entry(pak, entries, lobby_index, base))
        lobby_root_index = next(i for i, e in enumerate(entries) if e[0].lower().endswith('/octane_lobby_privategame_root.igz'))
        lobby_root = team_selector_callback(iga.read_entry(f, *entries[lobby_root_index][1:]))
        public_index = next(i for i, e in enumerate(entries) if e[0].lower() == PUBLIC_LOBBY_ENTRY)
        public_lobby = public_lobby_card(current_entry(pak, entries, public_index, base))
        mode_select = payload
        lobby_members_index = next(i for i, e in enumerate(entries) if e[0].lower() == LOBBY_MEMBERS_ENTRY)
        lobby_members_native = current_entry(pak, entries, lobby_members_index, base)
        try:
            tier_assets = {**asset_replacements(), **menu_asset_replacements()}
            lobby_members_patched = ranked_lobby_icons(lobby_members_native)
        except Exception as e:
            print(f"Lobby members: ready icon clone skipped ({e}); native panel kept")
            tier_assets = {}
            lobby_members_patched = lobby_members_native
        lobby_members = lobby_members_patched
        main_index = next(i for i, e in enumerate(entries) if e[0].lower() == MAIN_LOBBY_ENTRY)
        main_native = current_entry(pak, entries, main_index, base)
        main_lobby = ranked_main_button(main_native)
        hof_index = next(i for i, e in enumerate(entries) if e[0].lower().endswith('/ui/octane_leaderboards.igz'))
        hof_ui = hall_of_fame.leaderboard(current_entry(pak, entries, hof_index, base))
        hof_languages = {}
        for archive_name in ('juicedomain_startscreen', 'update', 'update2'):
            language_pak = resolve(Path(game) / 'archives' / (archive_name + '.pak'))
            language_entries = iga.pak_entries(language_pak)
            for language_index, entry in enumerate(language_entries):
                path = entry[0].lower().removeprefix(PACKAGE_PREFIX)
                if path.startswith('ui/octane_leaderboards_') and path.endswith('.lng'):
                    target = path.replace('octane_leaderboards_', 'octane_halloffame_', 1)
                    hof_languages[target] = current_entry(language_pak, language_entries,
                        language_index, patches.get('/app0/archives/' + language_pak.name))
        hof_project = Igz(hof_ui)
        hall_of_fame.dependencies(hof_project, {
            Path(path).stem: path.replace('ui/', 'ui:/', 1) for path in hof_languages})
        hof_ui = hof_project.build()
        main_material_index = next(i for i, e in enumerate(entries)
                                   if e[0].lower() == LOBBY_MATERIALS_ENTRY.lower())
        main_materials = clone_lobby_materials(
            current_entry(pak, entries, main_material_index, base))
        package_index = next(i for i, e in enumerate(entries) if e[0].lower() == ONLINE_PACKAGE)
        source_index = next(i for i, e in enumerate(entries) if e[0].lower() == START_PACKAGE)
        race_package_index = next(i for i, e in enumerate(entries) if e[0].lower().endswith('/juicedomain_race_pkg.igz'))
        race_package = iga.read_entry(f, *entries[race_package_index][1:])
        package = cup_package(game, iga.read_entry(f, *entries[package_index][1:]),
                              iga.read_entry(f, *entries[source_index][1:]), assets,
                              race_package)
        package = package_with_rows(package, [('effect', 'vfx/ctr/gui/ctr_gui_wumpa_juiced.igz')] + [
            ('texture', path.lower()) for path in (
                RANKED_BACKGROUND_TEXTURE, HOF_BACKGROUND_TEXTURE, menu_icon('hof')[2],
                *(tier_icon(mode, tier)[2] for mode, tier in TIERS))])
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    _, mapped = records()
    mapped = dict(mapped)
    race_package = package_with_rows(race_package, [
        ('material_instances', LOBBY_MEMBERS_MATERIALS_ENTRY.removeprefix(PACKAGE_PREFIX)),
        *[('texture', path.lower()) for path in tier_assets if path.startswith('textures/')]])
    ranking_replacements = {
        TEAM_RANKING_UI.removeprefix(PACKAGE_PREFIX): ranking_ui,
        TEAM_RANKING_ROOT.removeprefix(PACKAGE_PREFIX): ranking,
        LOBBY_MEMBERS_MATERIALS_ENTRY.removeprefix(PACKAGE_PREFIX): member_materials,
        **{path.lower(): data for path, data in tier_assets.items() if path.startswith('textures/')},
        'ui/octane_race_ranking.igz': score_ui,
    }
    for project in ('octane_battleteam_ranking', 'octane_race_ranking', 'octane_race_leaders'):
        archive = out / (project + '.pak')
        ranking_stream_archive(game, archive, race_package, project, ranking_replacements)
        mapped.setdefault('archives/' + archive.name, archive)
    # StreamingUI fallbacks: game may open these pak files directly (#0009).
    lobby_archive = out / 'octane_lobbymembers.pak'
    ranking_stream_archive(
        game, lobby_archive, package, 'octane_lobbymembers',
        {LOBBY_MEMBERS_ENTRY.removeprefix(PACKAGE_PREFIX): lobby_members,
         LOBBY_MEMBERS_MATERIALS_ENTRY.removeprefix(PACKAGE_PREFIX): member_materials,
         **{path.lower(): data for path, data in tier_assets.items()
            if path.startswith('textures/')}})
    mapped.setdefault('archives/' + lobby_archive.name, lobby_archive)
    with pak.open('rb') as f:
        _, hof_source_rows = package_rows(Igz(iga.read_entry(f, *entries[source_index][1:])))
    hof_package = package_with_rows(package, hof_source_rows + [('gui_project', hall_of_fame.PATH)] +
                                    [('lang_file', path) for path in hof_languages])
    hof_replacements = {
        hall_of_fame.PATH: hof_ui, **hof_languages,
        LOBBY_MEMBERS_ENTRY.removeprefix(PACKAGE_PREFIX): lobby_members,
        LOBBY_MEMBERS_MATERIALS_ENTRY.removeprefix(PACKAGE_PREFIX): member_materials,
        **{path.lower(): data for path, data in tier_assets.items() if path.startswith('textures/')},
    }
    hof_archive = out / 'octane_halloffame.pak'
    ranking_stream_archive(game, hof_archive, hof_package, 'octane_halloffame',
                           hof_replacements, resident={path for _, path in package_rows(Igz(package))[1]})
    from graphics_tab import Archive
    with_archive = Archive(hof_archive)
    try:
        pkg_index = next(i for i in range(len(with_archive.names)) if with_archive.logical(i).lower().endswith('/octane_halloffame_pkg.igz'))
        _, hof_rows = package_rows(Igz(with_archive.read(pkg_index)))
        resident = {path for _, path in package_rows(Igz(package))[1]}
        # Online cannot read assets from the Startscreen domain after it unloads.
        added = {path for _, path in hof_rows} - resident
        for i in range(len(with_archive.names)):
            path = with_archive.logical(i).lower().removeprefix(PACKAGE_PREFIX)
            # The package manifest must be discoverable before its archive is mounted.
            if path in added or path.startswith("vsc/") or i == pkg_index:
                tier_assets.setdefault(path, with_archive.read(i))
    finally:
        with_archive.f.close()
    mapped.setdefault('archives/' + hof_archive.name, hof_archive)
    tier_assets[hall_of_fame.PATH] = hof_ui
    mode_archive = out / 'octane_lobby_modeselect.pak'
    ranking_stream_archive(game, mode_archive, package, 'octane_lobby_modeselect',
                           {MODE_SELECT_ENTRY.removeprefix(PACKAGE_PREFIX): mode_select})
    mapped.setdefault('archives/' + mode_archive.name, mode_archive)
    menu_assets = {k.lower(): v for k, v in menu_asset_replacements().items()}
    menu_assets[QUICK_ICON_TEXTURE.lower()] = quick_icon_texture()
    domain_pak = resolve(Path(game) / 'archives/juicedomain_onlinemenus.pak')
    ranked_texture = lobby_background_texture(domain_pak, 'ranked')
    menu_assets[RANKED_BACKGROUND_TEXTURE.lower()] = ranked_texture
    tier_assets[RANKED_BACKGROUND_TEXTURE] = ranked_texture
    hof_texture = lobby_background_texture(domain_pak, 'hof')
    menu_assets[HOF_BACKGROUND_TEXTURE.lower()] = hof_texture
    tier_assets[HOF_BACKGROUND_TEXTURE] = hof_texture
    domain_entries = iga.pak_entries(domain_pak)
    domain_material_index = next(i for i, e in enumerate(domain_entries)
                                 if e[0].lower() == LOBBY_MATERIALS_ENTRY.lower())
    with domain_pak.open('rb') as f:
        domain_materials = clone_lobby_materials(
            iga.read_entry(f, *domain_entries[domain_material_index][1:]))
    menu_assets[RANKED_BACKGROUND_PATH.lower()] = main_materials
    tier_assets[RANKED_BACKGROUND_PATH] = main_materials
    main_archive = out / 'octane_lobby_main.pak'
    ranking_stream_archive(
        game, main_archive, package, 'octane_lobby_main',
        {MAIN_LOBBY_ENTRY.removeprefix(PACKAGE_PREFIX): main_lobby, **menu_assets})
    mapped.setdefault('archives/' + main_archive.name, main_archive)
    asset_archive = out / 'custom_ui_assets.pak'
    build_ui_asset_pak(asset_archive, pak)
    mapped.setdefault('archives/' + asset_archive.name, asset_archive)
    # PNG tier icon assets (textures + materials) for lobby member rank icons.
    root = long_path(assets)
    folded = {rel.casefold() for rel in tier_assets}
    for asset in root.rglob('*'):
        # Material instance namespaces are case-sensitive, so a file left under an older
        # spelling redirects the same guest path a second time.
        relative = asset.relative_to(root).as_posix()
        if asset.is_file() and relative not in tier_assets and relative.casefold() in folded:
            asset.unlink()
    for rel, data in tier_assets.items():
        dest = long_path(assets / rel)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
    for asset in root.rglob('*'):
        if asset.is_file():
            mapped.setdefault(asset.relative_to(root).as_posix(), asset)
    music_index = next(i for i, e in enumerate(entries) if e[0].lower() == RACE_MUSIC_ENTRY)
    with pak.open('rb') as f:
        music = team_finish_sounds(iga.read_entry(f, *entries[music_index][1:]))
    target = out / 'online-mode.bbpatch'
    archive_patch.write_entries_patch(target, pak, guest,
                                      {index: payload, race_ui_index: score_ui, lobby_index: lobby, main_index: main_lobby,
                                       main_material_index: main_materials, package_index: package,
                                       race_package_index: race_package,
                                       settings_index: settings, ranking_index: ranking, ranking_ui_index: ranking_ui,
                                       lobby_root_index: lobby_root, music_index: music, public_index: public_lobby,
                                       lobby_members_index: lobby_members}, base,
                                      override={index, race_ui_index, settings_index, ranking_ui_index, lobby_index, public_index, main_index,
                                                main_material_index,
                                                 lobby_members_index})
    patches[guest] = target

    # Patch the native material file before the Ranked button resolves its new export.
    domain_guest = '/app0/archives/juicedomain_onlinemenus.pak'
    patches.pop(domain_guest, None)
    stale_domain = out / 'online-lobby-members.bbpatch'
    domain_quick_index = next(i for i, e in enumerate(domain_entries)
                              if e[0].lower().removeprefix(PACKAGE_PREFIX) ==
                              QUICK_ICON_TEXTURE.lower())
    domain_replacements = {domain_quick_index: menu_assets[QUICK_ICON_TEXTURE.lower()],
                           domain_material_index: domain_materials}
    domain_member_material_index = next(i for i, e in enumerate(domain_entries)
                                        if e[0].lower() == LOBBY_MEMBERS_MATERIALS_ENTRY)
    with domain_pak.open('rb') as f:
        domain_replacements[domain_member_material_index] = clone_lobby_rank_materials(
            iga.read_entry(f, *domain_entries[domain_member_material_index][1:]))
    try:
        domain_index = next(i for i, e in enumerate(domain_entries)
                            if e[0] == 'temporary/octane/data/ps4/output/ui/Octane_LobbyMembers.igz')
        domain_texture_index = next(i for i, e in enumerate(domain_entries)
                                    if e[0].lower().removeprefix(PACKAGE_PREFIX) ==
                                    SINGLE_RACE_TEXTURE.lower())
        with domain_pak.open('rb') as f:
            domain_native = iga.read_entry(f, *domain_entries[domain_index][1:])
        domain_patched = ranked_lobby_icons(domain_native)
        domain_replacements[domain_index] = domain_patched
        domain_replacements[domain_texture_index] = tier_assets[SINGLE_RACE_TEXTURE]
        print("Online: Octane_LobbyMembers domain patch carries native Ranked materials")
    except Exception as e:
        print(f"Online: members panel left fully native ({e})")
    archive_patch.write_entries_patch(stale_domain, domain_pak, domain_guest,
                                      domain_replacements, patches.get(domain_guest))
    patches[domain_guest] = stale_domain

    # update3 loads last and carries a native Single Race copy for the Graphics menu.
    if graphics_archive := os.environ.get('BB_GRAPHICS_TAB_ARCHIVE'):
        graphics_pak = Path(graphics_archive)
        graphics_entries = iga.pak_entries(graphics_pak)
        graphics_texture_index = next(i for i, e in enumerate(graphics_entries)
                                      if e[0].lower().removeprefix(PACKAGE_PREFIX) ==
                                      SINGLE_RACE_TEXTURE.lower())
        graphics_guest = '/app0/archives/update3.pak'
        target = out / 'online-graphics-rank-icon.bbpatch'
        archive_patch.write_entries_patch(
            target, graphics_pak, graphics_guest,
            {graphics_texture_index: tier_assets[SINGLE_RACE_TEXTURE]},
            patches.get(graphics_guest))
        patches[graphics_guest] = target

    # This texture is duplicated and cached by whichever domain loads first.
    for archive_name in ('juicedomain_startscreen', 'juicedomain_race',
                         'juicedomain_race_flareday', 'juicedomain_race_flarenight'):
        source = resolve(Path(game) / 'archives' / (archive_name + '.pak'))
        source_entries = iga.pak_entries(source)
        texture_index = next(i for i, e in enumerate(source_entries)
                             if e[0].lower().removeprefix(PACKAGE_PREFIX) ==
                             SINGLE_RACE_TEXTURE.lower())
        guest = '/app0/archives/' + archive_name + '.pak'
        target = out / ('online-' + archive_name + '-rank-icon.bbpatch')
        archive_patch.write_entries_patch(
            target, source, guest,
            {texture_index: tier_assets[SINGLE_RACE_TEXTURE]}, patches.get(guest))
        patches[guest] = target

    label_pak = resolve(Path(game) / 'archives/update.pak')
    label_entries = iga.pak_entries(label_pak)
    label_base = patches.get('/app0/archives/update.pak')
    label_index = next(i for i, e in enumerate(label_entries) if e[0].lower() == LABEL_ENTRY)
    with label_pak.open('rb') as f:
        label = Igz(iga.read_entry(f, *label_entries[label_index][1:]))
    if label.strings.count('Racing Type') != 1:
        raise ValueError('native Online mode label is not recognized')
    label.strings[label.strings.index('Racing Type')] = 'GAME MODE'
    guest = '/app0/archives/update.pak'
    target = out / 'online-mode-label.bbpatch'
    replacements = {label_index: label.build()}
    update_material_index = next(i for i, e in enumerate(label_entries)
                                 if e[0].lower() == LOBBY_MATERIALS_ENTRY.lower())
    replacements[update_material_index] = clone_lobby_materials(
        current_entry(label_pak, label_entries, update_material_index, label_base))
    replacements[member_material_index] = member_materials
    with label_pak.open('rb') as f:
        music_index = next(i for i, e in enumerate(label_entries) if e[0].lower() == RACE_MUSIC_ENTRY)
        replacements[music_index] = team_finish_sounds(iga.read_entry(f, *label_entries[music_index][1:]))
        selector_index = next(i for i, e in enumerate(label_entries)
                              if e[0].lower().endswith('/octane_teamselect_root.igz'))
        replacements[selector_index] = team_selector_legend(iga.read_entry(f, *label_entries[selector_index][1:]))
        start_index = next(i for i, e in enumerate(label_entries)
                           if e[0].lower().endswith('/octane_lobby_privategame_buttonready.igz'))
        replacements[start_index] = private_start_button(iga.read_entry(f, *label_entries[start_index][1:]))
        root_index = next(i for i, e in enumerate(label_entries) if e[0].lower().endswith('/octane_lobby_privategame_root.igz'))
        replacements[root_index] = team_selector_callback(iga.read_entry(f, *label_entries[root_index][1:]))
        timer_index = next(i for i, e in enumerate(label_entries) if e[0].lower() == TIMER_ENTRY)
        replacements[timer_index] = cup_timer(iga.read_entry(f, *label_entries[timer_index][1:]))
        for name, specification in CUP_GRAPHS.items():
            path = 'temporary/octane/data/ps4/output/vsc/ui/' + name + '.igz'
            index = next(i for i, e in enumerate(label_entries) if e[0].lower() == path)
            replacements[index] = cup_route(iga.read_entry(f, *label_entries[index][1:]), specification)
    archive_patch.write_entries_patch(target, label_pak, guest, replacements, label_base,
                                      override={update_material_index})
    patches[guest] = target
    permanent_ui_package(game, out, patches, mapped)
    file_map(game, out / 'cup-files.map', out / 'cup-file-dirs',
             [(Path(relative), source) for relative, source in mapped.items()])
    return list(patches.values()), f'{site:x}'


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--elf', required=True, type=Path)
    p.add_argument('--base', action='append', default=[], type=Path)
    a = p.parse_args()
    patches, hook = prepare(a.game, a.out, a.elf, a.base)
    print('archive_patches=' + '|'.join(map(str, patches)))
    print('online_mode_hook=' + hook)
    print('cup_file_map=' + str((a.out / 'cup-files.map').resolve()))
    print('known_heroes=' + known_heroes(a.game))
