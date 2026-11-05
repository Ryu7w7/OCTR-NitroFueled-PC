"""Build Itemless HUD views; native Time Trial rules select unmodified track assets."""
import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

from archive_reader import Archive
from archive_patch import read, write_entries_patch
from mod_file_paths import resolve
from widescreen_startup import IGZ

TRACK_LAYER = re.compile(r'^maps/Octane/(T\d{3}_[^/]+)/[^/]+_(?:Power_Up|Design)\.igz$', re.I)
WUMPA_HUD = 'ui/GuiSharedAssets/Octane_RacerHud_WumpaCount.igz'
ITEM_HUD = 'ui/GuiSharedAssets/Octane_PowerUp.igz'
HUD_ARCHIVES = {
    'update2.pak',
    'juicedomain_race.pak',
    'juicedomain_race_flareday.pak',
    'juicedomain_race_flarenight.pak',
}
NESTED_RACE = re.compile(r'^archives/T5\d{2}_[^/]+\.pak$', re.I)


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


def race_tracks(archive):
    """Count the shipped race tracks without changing or decompressing their map assets."""
    tracks = {match.group(1).lower() for item in archive.files
              if (match := TRACK_LAYER.fullmatch(item['path']))}
    for entry in archive.files:
        if NESTED_RACE.fullmatch(entry['path']) and 'flyby' not in entry['path'].lower():
            with archive.nested(entry) as nested:
                tracks.update(race_tracks(nested))
    return tracks


# The scripts whose code shapes the overlays: a change to any of them builds them again.
SOURCES = ('speed_mode.py', 'archive_patch.py', 'archive_reader.py',
           'widescreen_startup.py', 'mod_file_paths.py')


def stamp_of(game, base_patches):
    """What the overlays are made from: these scripts, the game's archives (name, size, time) and
    the overlays they build on (their bytes: the earlier steps write them again each launch)."""
    h = hashlib.sha256()
    game = Path(game).resolve()
    h.update(str(game).encode() + b'\0')
    here = Path(__file__).resolve().parent
    for name in SOURCES:
        h.update(name.encode() + b'\0' + (here / name).read_bytes())
    for pak in sorted((game / 'archives').glob('*.pak')):
        source = resolve(pak)
        st = source.stat()
        h.update(f'{pak.name.lower()}:{source}:{st.st_dev}:{st.st_ino}:{st.st_size}:{st.st_mtime_ns}:{st.st_ctime_ns}\n'.encode())
    for path in base_patches:
        h.update(str(Path(path).resolve()).encode() + b'\0' + Path(path).read_bytes())
    return h.hexdigest()


def prepare(game, output, base_patches):
    """Return matching normal/Itemless HUD views and the race track count.

    Their directories and appended asset offsets match, so the runtime can serve
    normal HUD bytes in Arcade/Adventure and Itemless HUD bytes in eligible rooms.
    The views are kept (speed-mode/cache.json) and used again
    while nothing they are made from changed.
    """
    game, output = Path(game), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    cache = output / 'cache.json'
    stamp = stamp_of(game, base_patches)
    try:
        kept = json.loads(cache.read_text(encoding='utf-8'))
        if kept.get('stamp') == stamp and all(Path(p).is_file() for p in kept['normal'] + kept['speed']):
            return [Path(p) for p in kept['normal']], [Path(p) for p in kept['speed']], kept['tracks']
    except (OSError, ValueError, KeyError, TypeError):
        pass
    cache.unlink(missing_ok=True)  # a build stopped halfway must not be trusted next time
    normal, speed, tracks = build(game, output, base_patches)
    cache.write_text(json.dumps({'stamp': stamp, 'normal': [str(p) for p in normal],
                                 'speed': [str(p) for p in speed], 'tracks': tracks}), encoding='utf-8')
    return normal, speed, tracks


def build(game, output, base_patches):
    """The views prepare() returns, made from the game's archives."""
    current = {read(path)[1]: Path(path) for path in base_patches}
    entries = {}
    tracks = set()
    for pak in sorted((game / 'archives').glob('*.pak')):
        if pak.name not in ('update.pak', *HUD_ARCHIVES) \
                and not re.match(r'^t\d{3}_.+\.pak$', pak.name, re.I):
            continue
        archive = Archive(pak)
        try:
            tracks.update(race_tracks(archive))
            selected, normal = {}, {}
            for item in archive.files:
                path = item['path']
                if path == WUMPA_HUD and pak.name in HUD_ARCHIVES:
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
    if len(tracks) != 40:
        raise ValueError(f'Speed test resources incomplete: {len(tracks)}/40 tracks, '
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
    return list(normal_view.values()), list(speed_view.values()), len(tracks)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('game', type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--base', action='append', default=[], type=Path)
    args = parser.parse_args()
    try:
        normal, speed, count = prepare(args.game, args.out, args.base)
    except (ValueError, OSError, KeyError, IndexError) as error:
        # run.sh then starts without Speed Mode (a damaged game file must not stop the game)
        print(f'ITEMLESS not available: {error}', file=sys.stderr)
        sys.exit(2)
    print('ITEMLESS: HUD overlays prepared; native Time Trial track rules cover', count, 'tracks')
    print('archive_patches=' + '|'.join(map(str, normal)))
    print('private_archive_patches=' + '|'.join(map(str, speed)))


if __name__ == '__main__':
    main()
