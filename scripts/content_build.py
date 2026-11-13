# SPDX-License-Identifier: GPL-2.0-or-later
"""The update archive that adds external content to the game: archives/update3.pak.

The game loads archives/update.pak, update2.pak, update3.pak... in turn, each overriding the files
of the ones before. With external content on, the launcher builds update3.pak from:

  - the game's track registration files (scripts/track_registry.py FILES), the newest copy the
    game loads with the enabled mods' entry changes applied (Custom Cup's track menu, ...), with
    every enabled track registered in them;
  - the game files the packages add (zone info, picture textures, materials);
  - the Options menu's Graphics tab and the main menu's Quit Game (scripts/graphics_tab.py), which
    the runtime otherwise serves as update3.pak itself, with the mods' changes to those menus.

It is served through the mods' file redirection (scripts/mod_content.py), as a mod's own
update3.pak would be; a mod that ships a whole update3.pak cannot be on at the same time.
"""
import contextlib
import io
from pathlib import Path

import content_package as cp
import track_registry as tr
from graphics_tab import Archive, write_archive
from mod_components import payload_bytes

TARGET = 'archives/update3.pak'
PREFIX = 'temporary/octane/data/ps4/output/'
UPDATES = ('update2.pak', 'update.pak')  # newest first


def newest(game, paths, optional=()):
    """{path: (archive name, bytes)} of the newest copy the game loads of each path: update2.pak,
    update.pak, then any other game archive. Paths in `optional` may be missing."""
    paths = list(paths) + [p for p in optional if p not in paths]
    found = {}
    wanted = {p.casefold(): p for p in paths}
    folder = Path(game) / 'archives'
    order = [folder / n for n in UPDATES]
    order += sorted(p for p in folder.glob('*.pak') if p.name.casefold() not in UPDATES
                    and not p.name.casefold().startswith('update'))
    for pak in order:
        if len(found) == len(wanted):
            break
        if not pak.is_file():
            continue
        archive = Archive(pak)
        try:
            for i in range(len(archive.names)):
                key = archive.logical(i).casefold()
                if key in wanted and wanted[key] not in found:
                    found[wanted[key]] = (pak.name, archive.read(i))
        finally:
            archive.f.close()
    missing = [p for p in paths if p not in found and p not in optional]
    if missing:
        raise ValueError('the game has no ' + ', '.join(missing))
    return found


def effective(game, paths, operations, optional=()):
    """{path: bytes} the game loads for each path with the mods' archive entry changes:
    operations [(op, folder)] as scripts/mod_content.py collects them."""
    out = {}
    for path, (archive, data) in newest(game, paths, optional).items():
        for op, folder in operations:
            if op.get('kind') == 'archive_asset' and op['archive'].casefold() == archive.casefold() \
                    and op['asset'].casefold() == path.casefold():
                data = payload_bytes(op, folder, data)
        out[path] = data
    return out


def graphics_files(game, operations, staging, log):
    """[(full name, logical name, ordinal, bytes)] of the Graphics tab's menus, built as
    scripts/graphics_tab.py does, with the mods' changes to those menus (scripts/widescreen.py
    does the same for its layouts). [] when they cannot be built."""
    import graphics_tab
    changes = {(op['archive'].casefold(), op['asset'].casefold()): (op, folder) for op, folder in operations
               if op.get('kind') == 'archive_asset'}

    def transform(archive, logical, data):
        found = changes.get((archive.casefold(), logical.casefold()))
        return payload_bytes(found[0], found[1], data) if found else data
    dest = Path(staging) / 'graphics-tab.pak'
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            graphics_tab.build(Path(game), dest, Path(staging) / 'graphics-tab-dump', asset_transform=transform)
    except (OSError, ValueError, KeyError) as error:
        log(f'External content: no Graphics tab in the update archive ({error})')
        return []
    archive = Archive(dest)
    try:
        return [(*archive.names[i], archive.infos[i][1], archive.read(i)) for i in range(len(archive.names))]
    finally:
        archive.f.close()
        dest.unlink()


def build(game, store, staging, packages, operations, settings, log=print, own_page=True):
    """archives/update3.pak written under staging/files for the enabled packages
    ([(manifest, folder)], by id). Returns [(target, source)] of the packages' own archives,
    for the file redirection, or [] when there is nothing to add."""
    if not packages:
        return []
    cp.check_against_game(packages, cp.game_index(game, store))
    titles = [tr.LANGUAGE_FILE.format(lang) for lang in tr.LANGUAGES]
    base = effective(game, tr.FILES, operations, optional=titles)
    tracks = [cp.track(m) for m, _ in packages if m['type'] == 'track']
    changed = tr.register(base, tracks, log, own_page=own_page)
    entries = dict(changed)
    for m, folder in packages:
        for logical, source in cp.game_files(m, folder):
            entries[logical] = Path(source).read_bytes()
    files = []
    if settings.get('graphics_tab', '1') != '0':
        files = graphics_files(game, operations, staging, log)
    taken = {logical.casefold() for _, logical, _, _ in files}
    clash = sorted(taken & {p.casefold() for p in entries})
    if clash:
        raise ValueError('external content and the Graphics tab both change ' + ', '.join(clash))
    ordinal = max((o >> 8 for _, _, o, _ in files), default=-1) + 1
    for logical, data in sorted(entries.items(), key=lambda e: e[0].casefold()):
        files.append((PREFIX + logical, logical, ordinal << 8, data))
        ordinal += 1
    dest = Path(staging) / 'files' / 'archives' / 'update3.pak'
    dest.parent.mkdir(parents=True, exist_ok=True)
    template = Archive(Path(game) / 'archives' / 'update2.pak')
    template.f.close()
    write_archive(dest, files, template)
    log('External content: ' + ', '.join(f'{m["name"]["en"]} {m["version"]} ({m["author"]})' for m, _ in packages))
    return [(Path(target), source) for m, folder in packages for target, source in cp.archives(m, folder)]
