"""Native NORMAL / ITEMLESS / BATTLE options for the shared online mode selector.

The runtime keeps Speed disabled in public matchmaking until server queues support it.
"""
import argparse
import hashlib
import re
import struct
from pathlib import Path

import archive_patch
import gcn_shaders as iga
from igz_file import Igz, unpack_offsets, pack_offsets
from mod_file_paths import resolve, records
from mod_overlays import file_map
from mod_components import long_path
from patches import load_segments

ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_modeselect.igz'
LABEL_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_modeselect_en.lng'
RACE_MUSIC_ENTRY = 'temporary/octane/data/ps4/output/vsc/common/race/common_race_music_manager.igz'
TIMER_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_returntolobby_timer.igz'
LOBBY_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_privategame.igz'
SETTINGS_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_privategamesettings.igz'
PUBLIC_LOBBY_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_matchmaking.igz'
MAIN_LOBBY_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_main.igz'
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


def private_team_settings(blob):
    if hashlib.sha256(blob).hexdigest() not in {
        'a2259e5840343814fa5901ce16de69a3c7295760a5ceb8ed51baf2176608fbc4',  # USA 1.21
        'e8667f50355f3b0cfa35df6b25f159715c068e53bba86cc1ebecb948d4a97cb4',  # EU 1.21
        '759c89e2d1d9f19e312cc25b009df3a0375497672c40507434c475c886892df9',  # USA widescreen
        '841c6f4ee3727b95ad6c3c9438020dda3e0fb401d4ca3e143549e4e7290db161',  # EU widescreen
    }:
        raise ValueError('unsupported native private team settings')
    g = Igz(blob)
    # Single/CUP use the SingleRace settings animation. Give the existing Teams
    # row its visible battle state and row spacing; keep native selector/sync logic.
    for target, source, size in ((0x51e0, 0x4e80, 4), (0xe058, 0xdcd8, 12)):
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
    if race_blob is not None:
        # These projects must be resident before openByName: a missing project
        # falls back to a dedicated StreamingUI archive that does not exist.
        pending += ['ui/octane_race_ranking.igz', 'ui/octane_race_leaders.igz',
                    'ui/octane_battleteam_ranking.igz']
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
    rows = original + [(kind, path) for kind, path in source_rows if path in needed]
    array = g.append(b''.join(struct.pack('<QQ', g.string_index(kind), g.string_index(path))
                              for kind, path in rows))
    for offset in range(array, array + len(rows) * 16, 8):
        g.add_site('RSTT', offset)
    g.set_u64(chunk + 0x28, len(rows))
    g.set_u64(chunk + 0x30, (g.u64(chunk + 0x30) & ~0xffffffff) | len(rows) * 16)
    g.set_u64(chunk + 0x38, array)
    return g.build()


def ranking_stream_archive(game, dest, package_blob, project, replacements):
    """Provide the native StreamingUI fallback when the online chunk was released."""
    from graphics_tab import Archive, write_archive
    from contextlib import ExitStack
    package = Igz(package_blob)
    chunk, rows = package_rows(package)
    kinds = dict((path, kind) for kind, path in rows)
    with ExitStack() as stack:
        files = {}
        template = None
        for name in ('juicedomain_race', 'update', 'update2'):
            archive = Archive(resolve(Path(game) / 'archives' / (name + '.pak')))
            stack.callback(archive.f.close)
            template = archive
            for i in range(len(archive.names)):
                path = archive.logical(i).lower().removeprefix(PACKAGE_PREFIX)
                files[path] = (archive, i)
        pending = ['ui/' + project + '.igz']
        needed = {}
        while pending:
            path = pending.pop()
            if path in needed or path not in kinds:
                continue  # Other dependencies belong to native permanent chunks.
            if path not in files:
                # As in the Cup assets: a manifest can name languages this regional dump lacks
                if kinds[path] == 'lang_file' and path.endswith('.lng') and not path.endswith('_en.lng'):
                    continue
                raise ValueError('missing ranking dependency: ' + path)
            archive, index = files[path]
            data = replacements.get(path)
            if data is None:
                data = archive.read(index)
            needed[path] = data
            if kinds[path] in ('texture', 'lang_file'):
                continue
            asset = Igz(data)
            try:
                dependencies = asset.fixup('TDEP')
            except KeyError:
                continue
            strings = dependencies[3].split(b'\0')[:dependencies[1] * 2]
            pending.extend(raw.decode().lower().replace('<build>project:/' + PACKAGE_PREFIX, '')
                           .replace(':/', '/') for raw in strings[1::2])
        selected = [(kind, path) for kind, path in rows if path in needed]
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
    """Clone the native Matchmaking action and place Ranked directly below it."""
    g = Igz(blob)
    if (g.u64(0x970 + 0xe8) != 0x2110 or g.u64(0x970 + 0x50) != 0xa90 or
            g.u64(0xa90 + 0x58) != 0x970 or g.u64(0x7e0 + 0x48) != 0x900 or
            g.u32(0x900 + 12) != 8 or g.u64(0x2110 + 0x28) != 0x2148):
        raise ValueError('unsupported native main lobby matchmaking button')
    references = {tag: g.read_list(tag, unpack_offsets) for tag in ('RHND', 'RNEX')}
    copies = [(0x970, 0xa90, g.append(g.data[0x970:0xa90])),
              (0x2110, 0x2580, g.append(g.data[0x2110:0x2580]))]

    def moved(value):
        return next((new + value - lo for lo, hi, new in copies if lo <= value < hi), value)

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
    button = moved(0x970)
    nav = moved(0x2148)
    g.set_u64(button + 0x28, g.string_index('OCTR_RANKED_BUTTON'))
    g.add_site('RSTT', button + 0x28)
    g.set_u64(button + 0x50, 0xa90)
    g.set_u64(button + 0x58, 0x970)
    g.set_u64(0x970 + 0x50, button)
    g.set_u64(0xa90 + 0x58, button)
    struct.pack_into('<f', g.data, 0x970 + 0x64, -315.0)
    struct.pack_into('<f', g.data, button + 0x64, -210.0)
    # Native navigation fields: up, down. Keep every other button's links intact.
    g.set_u64(0x2148 + 0x118, button)
    g.set_u64(nav + 0x110, 0x970)
    g.set_u64(nav + 0x118, 0xa90)
    g.set_u64(0xd08 + 0x110, button)
    # A distinct icon, exactly as the native Graphics-tab Restart button (graphics_widgets.py):
    # the navigation's icon materials (+0x158 with its +0x178/+0x1a8 mirrors) get the grand prix
    # button's material ('octane_ui_ico_grandprix_tga__material', already in this file's EXID),
    # and the focused icon sprite gets its name reference at +0x150.
    exid = g.fixup('EXID')
    grandprix_material = next(i for i in range(exid[1]) if
                              struct.unpack_from('<I', exid[3], i * 8)[0] == 0x7c593110)
    exnm = g.fixup('EXNM')
    entries = list(struct.iter_unpack('<II', exnm[3][:exnm[1] * 8]))

    def reference(at, name, namespace):
        index = sum(not (b & 0x80000000) for _, b in entries)
        entries.append((g.string_index(name), g.string_index(namespace)))
        g.set_u64(at, index)
        references['RNEX'].append(at)

    reference(nav + 0x150, 'Sprite_octane_ui_raceselect_ico_crashcove_tga001',
              'Menu_ButtonWithIcon')
    exnm[1], exnm[3] = len(entries), b''.join(struct.pack('<II', *e) for e in entries)
    exnm[4] = (exnm[2] + len(exnm[3]) + 3) & ~3
    g.set_u64(nav + 0x158, grandprix_material)
    g.set_u64(nav + 0x178, grandprix_material)
    g.set_u64(nav + 0x1a8, grandprix_material)
    children = [g.u64(0x928 + i * 8) for i in range(8)]
    children.insert(children.index(0x970) + 1, button)
    array = g.append(struct.pack('<9Q', *children))
    for i in range(9):
        g.add_site('ROFS', array + i * 8)
    g.set_u32(0x900 + 12, 9)
    g.set_u32(0x900 + 16, 9)
    g.set_u64(0x900 + 0x18, (g.u64(0x900 + 0x18) & ~0xffffffff) | 72)
    g.set_u64(0x900 + 0x20, array)
    # The Ranked button shares the matchmaking button's VSC behavior object instead of carrying
    # a clone: the graph registers in a per-graph static instance map and a second live instance
    # corrupts the heap at scene unload (its double destruction). Behaviors are refcounted, so
    # both lists holding one instance is safe, and the graph runs with its native owner exactly
    # as when Matchmaking itself is pressed. The clone of the object stays uninstantiated.
    g.set_u64(moved(0x2140), 0x23b8)  # the cloned list's VSC slot now references the original
    g.sites['RVTB'].remove(moved(0x23b8))
    for tag, sites in references.items():
        fixup = g.fixup(tag)
        fixup[1], fixup[3] = len(sites), pack_offsets(sorted(sites))
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


def prepare(game, out, elf, bases):
    out = Path(out)
    assets = out / 'cup-assets'
    (assets / 'vsc/ui/octane_lobby_privategame/octane_lobby_privategame_teambutton.igz').unlink(missing_ok=True)
    site = hook_site(Path(elf).read_bytes())
    pak = resolve(Path(game) / 'archives/update2.pak')
    entries = iga.pak_entries(pak)
    index = next((i for i, e in enumerate(entries) if e[0].lower() == ENTRY), None)
    if index is None:
        raise ValueError('native Online menu graph is missing')
    guest = '/app0/archives/update2.pak'
    patches = {archive_patch.read(p)[1]: Path(p) for p in bases}
    base = patches.get(guest)
    payload = rebuild(current_entry(pak, entries, index, base))
    with pak.open('rb') as f:
        ranking_ui_index = next(i for i, e in enumerate(entries) if e[0].lower() == TEAM_RANKING_UI)
        race_ui_index = next(i for i, e in enumerate(entries) if e[0].lower() ==
                             PACKAGE_PREFIX + 'ui/octane_race_ranking.igz')
        ranking_ui = team_ranking_header(current_entry(pak, entries, ranking_ui_index, base))
        ranking_index = next(i for i, e in enumerate(entries) if e[0].lower() == TEAM_RANKING_ROOT)
        ranking = team_ranking_navigation(iga.read_entry(f, *entries[ranking_index][1:]))
        settings_index = next(i for i, e in enumerate(entries) if e[0].lower() == SETTINGS_ENTRY)
        settings = private_team_settings(current_entry(pak, entries, settings_index, base))
        lobby_index = next(i for i, e in enumerate(entries) if e[0].lower() == LOBBY_ENTRY)
        # Preserve the widescreen lobby layout before adding Online's tags/order.
        lobby = lobby_card(current_entry(pak, entries, lobby_index, base))
        lobby_root_index = next(i for i, e in enumerate(entries) if e[0].lower().endswith('/octane_lobby_privategame_root.igz'))
        lobby_root = team_selector_callback(iga.read_entry(f, *entries[lobby_root_index][1:]))
        public_index = next(i for i, e in enumerate(entries) if e[0].lower() == PUBLIC_LOBBY_ENTRY)
        public_lobby = public_lobby_card(current_entry(pak, entries, public_index, base))
        main_index = next(i for i, e in enumerate(entries) if e[0].lower() == MAIN_LOBBY_ENTRY)
        main_lobby = ranked_main_button(current_entry(pak, entries, main_index, base))
        package_index = next(i for i, e in enumerate(entries) if e[0].lower() == ONLINE_PACKAGE)
        source_index = next(i for i, e in enumerate(entries) if e[0].lower() == START_PACKAGE)
        race_package_index = next(i for i, e in enumerate(entries) if e[0].lower().endswith('/juicedomain_race_pkg.igz'))
        race_package = iga.read_entry(f, *entries[race_package_index][1:])
        package = cup_package(game, iga.read_entry(f, *entries[package_index][1:]),
                              iga.read_entry(f, *entries[source_index][1:]), assets,
                              race_package)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    _, mapped = records()
    mapped = dict(mapped)
    ranking_replacements = {
        TEAM_RANKING_UI.removeprefix(PACKAGE_PREFIX): ranking_ui,
        TEAM_RANKING_ROOT.removeprefix(PACKAGE_PREFIX): ranking,
        'ui/octane_race_ranking.igz': current_entry(pak, entries, race_ui_index, base),
    }
    for project in ('octane_battleteam_ranking', 'octane_race_ranking'):
        archive = out / (project + '.pak')
        ranking_stream_archive(game, archive, race_package, project, ranking_replacements)
        mapped.setdefault('archives/' + archive.name, archive)
    for asset in long_path(assets).rglob('*'):
        if asset.is_file():
            mapped.setdefault(asset.relative_to(long_path(assets)).as_posix(), asset)
    file_map(game, out / 'cup-files.map', out / 'cup-file-dirs',
             [(Path(relative), source) for relative, source in mapped.items()])
    music_index = next(i for i, e in enumerate(entries) if e[0].lower() == RACE_MUSIC_ENTRY)
    with pak.open('rb') as f:
        music = team_finish_sounds(iga.read_entry(f, *entries[music_index][1:]))
    target = out / 'online-mode.bbpatch'
    archive_patch.write_entries_patch(target, pak, guest,
                                      {index: payload, lobby_index: lobby, main_index: main_lobby, package_index: package,
                                       settings_index: settings, ranking_index: ranking, ranking_ui_index: ranking_ui,
                                       lobby_root_index: lobby_root, music_index: music, public_index: public_lobby}, base,
                                      override={index, settings_index, ranking_ui_index, lobby_index, public_index, main_index})
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
    archive_patch.write_entries_patch(target, label_pak, guest, replacements, label_base)
    patches[guest] = target
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
