"""Build disposable normal and private Speed archive views from the player's CTR files."""
import argparse
import lzma
import re
import struct
import tempfile
import zlib
from pathlib import Path

from archive_reader import Archive
from archive_patch import read, write, write_entries_patch
from igz_file import Igz
from widescreen_startup import IGZ

POWER_UP = re.compile(r'^maps/Octane/(T\d{3}_[^/]+)/[^/]+_Power_Up\.igz$', re.I)
DESIGN = re.compile(r'^maps/Octane/(T\d{3}_[^/]+)/[^/]+_Design\.igz$', re.I)
WUMPA_HUD = 'ui/GuiSharedAssets/Octane_RacerHud_WumpaCount.igz'
ITEM_HUD = 'ui/GuiSharedAssets/Octane_PowerUp.igz'
HUD_ARCHIVES = {
    'update2.pak',
    'juicedomain_race.pak',
    'juicedomain_race_flareday.pak',
    'juicedomain_race_flarenight.pak',
}
NESTED_RACE = re.compile(r'^archives/T5\d{2}_[^/]+\.pak$', re.I)
CRATE_SPAWNER = 'maps/Octane/T111_Crash_Cove/T111_Crash_Cove_VFX.igz'
EXTRA_ITEM_ASSETS = {
    CRATE_SPAWNER,
    'maps/Octane/T241_Assembly_Lane/T241_Assembly_Lane.igz',
}
# PUBLIC_SPEED_STEP_4: the Speed view used to strip the callbacks of the two public matchmaking
# buttons (Octane_Lobby_Main_MatchmakingButton, Octane_Online_Lobby_StartMatchMakingButton). The
# online menus are read once, in the private room, so after a Speed race the host could not start
# a public search until going back to the title screen (OCTR v0.7 test). Public races stay Normal
# without them: the Game Mode selector (runtime_online_mode.c) greys Speed out outside a private
# room and turns it off on every public pick.


def move_crates(blob, kind, tiger_gate=False, spawner=False):
    try:
        igz = Igz(blob)
    except KeyError:
        return None  # Empty stage resource without crates.
    if not igz.objects() or igz.objects()[0] != (0, 'igObjectList'):
        raise ValueError('Power Up asset has an unexpected root')
    count = igz.u32(0x0c)
    if not count:
        return None
    refs = igz.u64(0x20)
    types = dict(igz.objects())
    if refs + count * 8 > len(igz.data):
        raise ValueError('Power Up asset has an invalid root list')
    if kind == 'wumpa' and not any('WumpaFruitCrate' in name for name in igz.strings):
        return None
    crates = 0
    gates = 0
    spawners = 0
    result = bytearray(blob)
    data_offset = igz.sections[1][1]
    for i in range(count):
        off = igz.u64(refs + i * 8)
        if spawner and types.get(off) == 'CEntity':
            entity_data = igz.u64(off + 0x40)
            marker = entity_data + 0xc0
            if types.get(entity_data) == 'CEntityData' and marker in igz.sites['RSTT'] and \
                    igz.string_at(marker) == 'instance_Scripts.Graph.common_Crate_SpawnerData':
                spawners += 1
                struct.pack_into('<f', result, data_offset + off + 0x20, 1_000_000.0)
        if types.get(off) == 'CGameEntity':
            entity_data = igz.u64(off + 0x40)
            if types.get(entity_data) == 'CGameEntityData':
                model = entity_data + 0x48
                name = igz.string_at(model) if model in igz.sites['RSTT'] else ''
                if name == 'LevelAssets\\Basic\\box':
                    crates += 1
                    # Keep the object graph intact: the game dereferences these
                    # entities while loading the level. Place crates outside the
                    # playable map instead of removing their root references.
                    struct.pack_into('<f', result, data_offset + off + 0x20, 1_000_000.0)
                    continue
                if tiger_gate and name.endswith('T132_Tiger_Temple_SlideWall'):
                    gates += 1
                    struct.pack_into('<f', result, data_offset + off + 0x20, 1_000_000.0)
    if not crates and kind == 'item':
        raise ValueError('Power Up asset has no recognized item crates')
    if tiger_gate and gates != 1:
        raise ValueError(f'Tiger Temple shortcut gate count changed: {gates}')
    if tiger_gate:
        triggers = [obj for obj in IGZ(blob).objects if obj.get('name') == 'ShortcutRespawn'
                    and obj['type'] == 'CGameEntity']
        if len(triggers) != 1:
            raise ValueError(f'Tiger Temple shortcut respawn count changed: {len(triggers)}')
        struct.pack_into('<f', result, triggers[0]['offset'] + 0x20, 1_000_000.0)
    if spawner and spawners != 1:
        raise ValueError(f'Crash Cove crate spawner count changed: {spawners}')
    if not crates and not gates:
        return None
    return bytes(result)


def hide_hud(blob, root_name):
    igz = IGZ(blob)
    roots = [obj for obj in igz.objects if obj['type'] == 'igGuiPlaceable' and
             obj.get('name') == root_name]
    if len(roots) != 1:
        raise ValueError(f'HUD root was not found: {root_name}')
    result = bytearray(blob)
    struct.pack_into('<f', result, roots[0]['offset'] + 0x60, -10_000.0)
    return bytes(result)


def hide_wumpa_hud(blob):
    return hide_hud(blob, 'Octane_RacerHud_WumpaCount_asset_root')


def nested_track_ranges(outer, workdir):
    """Patch raw Grand Prix PAKs inside update2 without copying whole PAKs into the overlay."""
    ranges = []
    tracks = set()
    for entry in outer.files:
        if not NESTED_RACE.fullmatch(entry['path']) or 'flyby' in entry['path'].lower():
            continue
        if entry['compression'] != 0xffffffff:
            raise ValueError(f'Nested track PAK is unexpectedly compressed: {entry["path"]}')
        with tempfile.TemporaryDirectory(prefix='speed-track-', dir=workdir) as temporary:
            path = Path(temporary) / 'track.pak'
            path.write_bytes(outer.extract(entry))
            nested = Archive(path)
            try:
                if nested.version < 13:
                    raise ValueError(f'Unsupported nested track PAK: {entry["path"]}')
                for item in nested.files:
                    power, design = POWER_UP.fullmatch(item['path']), DESIGN.fullmatch(item['path'])
                    if not (power or design):
                        continue
                    before = nested.extract(item)
                    after = move_crates(before, 'item' if power else 'wumpa')
                    if after is None:
                        continue
                    if len(after) != len(before):
                        raise ValueError(f'Nested asset changed size: {item["path"]}')
                    if power:
                        tracks.add(power.group(1).lower())
                    size, comp = item['size'], item['compression']
                    table, shift = ((nested.tables[2], 7) if size <= 127 * nested.sector else
                                    (nested.tables[1], 15) if size <= 32767 * nested.sector else
                                    (nested.tables[0], 31))
                    for block in range((size + 32767) // 32768):
                        part = after[block * 32768:(block + 1) * 32768]
                        if part == before[block * 32768:(block + 1) * 32768]:
                            continue
                        marker = table[(comp & 0x0fffffff) + block]
                        at = item['offset'] + (marker & ((1 << shift) - 1)) * nested.sector
                        if not marker >> shift:
                            ranges.append((entry['offset'] + at, part))
                            continue
                        length = struct.unpack_from('<I', nested.data, at)[0]
                        kind = comp >> 28
                        if kind == 2:
                            props = bytes(nested.data[at + 4:at + 9])
                            value = props[0]
                            lc, value = value % 9, value // 9
                            lp, pb = value % 5, value // 5
                            codec = dict(id=lzma.FILTER_LZMA1,
                                         dict_size=struct.unpack_from('<I', props, 1)[0],
                                         lc=lc, lp=lp, pb=pb)
                            encoded = lzma.compress(part, format=lzma.FORMAT_RAW, filters=[codec])
                            decoded = lzma.decompress(encoded, format=lzma.FORMAT_RAW, filters=[codec])
                        elif kind == 1:
                            props = b''
                            compressor = zlib.compressobj(level=9, wbits=-15)
                            encoded = compressor.compress(part) + compressor.flush()
                            decoded = zlib.decompress(encoded, -15)
                        else:
                            raise ValueError(f'Unsupported nested compression: {kind}')
                        capacity = ((4 + len(props) + length + nested.sector - 1) //
                                    nested.sector) * nested.sector
                        if decoded != part or 4 + len(props) + len(encoded) > capacity:
                            raise ValueError(f'Nested track block does not fit: {item["path"]}')
                        ranges.append((entry['offset'] + at,
                                       struct.pack('<I', len(encoded)) + props + encoded))
            finally:
                nested.data.close()
                nested._file.close()
    return ranges, tracks


def prepare(game, output, base_patches):
    """Return matching normal/private archive views and the race track count.

    Their directories and appended asset offsets match, so the runtime can serve
    normal bytes in Arcade/Adventure and Speed bytes in a private room.
    """
    game, output = Path(game), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    current = {read(path)[1]: Path(path) for path in base_patches}
    entries = {}
    tracks = set()
    latest = Archive(game / 'archives' / 'update2.pak')
    try:
        updated_assets = {item['path'].lower() for item in latest.files}
        nested_ranges, nested_tracks = nested_track_ranges(latest, output)
    finally:
        latest.data.close()
        latest._file.close()
    earlier = Archive(game / 'archives' / 'update.pak')
    try:
        earlier_assets = {item['path'].lower() for item in earlier.files}
    finally:
        earlier.data.close()
        earlier._file.close()
    for pak in sorted((game / 'archives').glob('*.pak')):
        if pak.name not in ('update.pak', *HUD_ARCHIVES) \
                and not re.match(r'^t\d{3}_.+\.pak$', pak.name, re.I):
            continue
        archive = Archive(pak)
        try:
            selected, normal = {}, {}
            for item in archive.files:
                path = item['path']
                power = POWER_UP.fullmatch(path)
                design = DESIGN.fullmatch(path)
                extra = path in EXTRA_ITEM_ASSETS
                if power or design or extra:
                    # The newest copy of each asset takes precedence over track PAKs.
                    if pak.name == 'update.pak' and path.lower() in updated_assets:
                        continue
                    if pak.name not in ('update.pak', 'update2.pak') and \
                            path.lower() in updated_assets | earlier_assets:
                        continue
                    original = archive.extract(item)
                    changed = move_crates(original, 'item' if power or extra else 'wumpa',
                                          tiger_gate=bool(power and power.group(1).lower() == 't132_tiger_temple'),
                                          spawner=path == CRATE_SPAWNER)
                    if changed is not None:
                        selected[item['index']] = changed
                        normal[item['index']] = original
                        if power or design:
                            tracks.add((power or design).group(1).lower())
                elif path == WUMPA_HUD and pak.name in HUD_ARCHIVES:
                    original = archive.extract(item)
                    selected[item['index']] = hide_wumpa_hud(original)
                    normal[item['index']] = original
                elif path == ITEM_HUD and pak.name in HUD_ARCHIVES:
                    original = archive.extract(item)
                    selected[item['index']] = hide_hud(original, 'Octane_PowerUp_asset_root')
                    normal[item['index']] = original
            if selected:
                if selected.keys() != normal.keys() or any(len(selected[i]) != len(normal[i]) for i in selected):
                    raise ValueError(f'Speed and normal asset layouts differ: {pak.name}')
                entries[pak.name] = (normal, selected)
        finally:
            archive.data.close()
            archive._file.close()
    # The newest update is applied after per-track PAKs; duplicate track assets in
    # the base PAKs remain harmless, but the update entry is the one the game loads.
    if len(tracks | nested_tracks) != 40:
        raise ValueError(f'Speed test resources incomplete: {len(tracks | nested_tracks)}/40 tracks, '
                         f'{sorted(entries)}')
    normal_view, speed_view = current.copy(), current.copy()
    for name, (normal_entries, speed_entries) in entries.items():
        guest = '/app0/archives/' + name
        normal_target = output / (name + '.normal.bbpatch')
        speed_target = output / (name + '.speed.bbpatch')
        write_entries_patch(normal_target, game / 'archives' / name, guest,
                            normal_entries, current.get(guest))
        write_entries_patch(speed_target, game / 'archives' / name, guest,
                            speed_entries, current.get(guest))
        normal_view[guest], speed_view[guest] = normal_target, speed_target
    if nested_ranges:
        guest = '/app0/archives/update2.pak'
        target = speed_view[guest]
        size, _, ranges = read(target)
        if any(a < b + len(data) and b < a + len(chunk)
               for a, chunk in nested_ranges for b, data in ranges if b < size):
            raise ValueError('Nested track ranges overlap another update2 overlay')
        write(target, game / 'archives' / 'update2.pak', guest, ranges + nested_ranges)
    if len(normal_view) > 32 or len(speed_view) > 32:
        raise ValueError(f'Speed test exceeds the engine limit of 32 archive overlays: '
                         f'{len(normal_view)}/{len(speed_view)}')
    for guest in normal_view:
        normal_size, _, normal_ranges = read(normal_view[guest])
        speed_size, _, speed_ranges = read(speed_view[guest])
        normal_end = max([normal_size] + [o + len(d) for o, d in normal_ranges])
        speed_end = max([speed_size] + [o + len(d) for o, d in speed_ranges])
        if normal_size != speed_size or normal_end != speed_end:
            raise ValueError(f'Speed and normal archive sizes differ: {guest}')
    tracks.update(nested_tracks)
    return list(normal_view.values()), list(speed_view.values()), len(tracks)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--base', action='append', default=[], type=Path)
    args = parser.parse_args()
    normal, speed, count = prepare(args.game, args.out, args.base)
    print('Speed Mode (Experimental): private overlays prepared for', count, 'tracks')
    print('archive_patches=' + '|'.join(map(str, normal)))
    print('private_archive_patches=' + '|'.join(map(str, speed)))


if __name__ == '__main__':
    main()
