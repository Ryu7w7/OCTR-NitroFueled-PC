#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A classic track mod converted to an external content package (scripts/content_package.py).

A classic track mod is a ZIP or folder with the track's own archives (archives/t922_suzuka.pak,
its flybys) and a whole update3.pak with the track's registration and its own files in it. A mod
with several new tracks (CTR Track Editor exports every track of a project in one mod) becomes one
package per track: each takes the archives named after it and the new files naming it. The
conversion keeps:

  - the track's own archives, with the reference counts of their map files fixed (tracks saved
    by CTR Track Editor before v0.5 have them wrong, and the game crashes loading the next level:
    scripts/track_editor.py);
  - the files of update3.pak the game does not have (zone info, picture texture, materials);
  - from the registration files: the track's map, and its picture material and texture when it
    put one in LevelSelect_materials.

It leaves out every other change the mod made to the game's own files ("global" changes, such as
scripts every track uses): those would change the game for all tracks, which external content
cannot. They are named in the result, and the classic mod stays as it was.
The package keeps the mod's name (with several tracks, each track's own, from its zone info),
version and author, and says what it was converted from.

    python scripts/content_convert.py GAME_FOLDER MOD.zip OUTPUT_FOLDER
"""
import argparse
import hashlib
import json
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

import content_compat
import content_delta as cd
import content_package as cp
import track_editor as te
import track_registry as tr
from graphics_tab import Archive
from igz_merge import File


def source_folder(source, work):
    source = Path(source)
    if source.is_dir():
        return source
    with zipfile.ZipFile(source) as z:
        for info in z.infolist():
            cp.safe_path(info.filename.rstrip('/'))
        z.extractall(work)
    root = Path(work)
    while not (root / 'archives').is_dir():
        dirs = [p for p in root.iterdir() if p.is_dir()]
        if len(dirs) != 1:
            raise ValueError('no archives/ folder in the mod')
        root = dirs[0]
    return root


def squeezed(name):
    """A name to compare names by: LevelImage_T904_Custom_Map, t904custommap."""
    return name.casefold().replace('_', '')


def entry(entries, path):
    key = next((k for k in entries if k.casefold() == path.casefold()), None)
    return entries[key][1] if key else None


def find_tracks(game, entries):
    """[(map, image, texture)] of the tracks the mod's registration adds: each new
    CGuiOctaneLevelData, and the new LevelImage_* of LevelSelect_materials with its texture (the
    one named after the track; with one track, or one picture left, the one there is)."""
    gui = entry(entries, tr.GUI)
    if gui is None:
        raise ValueError('the mod registers no track (no guisystemdata.igz in its update archive)')
    have = tr.registered(game.newest(tr.GUI))
    new = sorted(tr.registered(gui) - have)
    if not new:
        raise ValueError('the mod registers no new track')
    g = File(gui)
    spelled = {g.string_at(o + 0x10).casefold(): g.string_at(o + 0x10) for o, t in g.typed.items()
               if t == tr.LEVEL_DATA}
    tracks = [spelled[n] for n in new]
    pictures = {}
    materials = entry(entries, tr.MATERIALS)
    if materials is not None:
        old = set(tr.named(File(game.newest(tr.MATERIALS))))
        z = File(materials)
        for image, o in tr.named(z).items():
            if not image.startswith(tr.IMAGE) or image in old:
                continue
            textures = [z.handle(z.u32(site))[1] for x in tr.owned(z, o) if z.typed[x] == 'igGraphicsTexture'
                        for tag, site in z.sites_in(x, z.size(x)) if tag == 'RHND']
            if len(textures) != 1:
                raise ValueError(f'{image}: expected one texture')
            pictures[image] = textures[0]
    found = {}
    for track in tracks:
        image = next((n for n in pictures if squeezed(n) == squeezed(tr.IMAGE + track.rsplit('/', 1)[-1])), None)
        if image:
            found[track] = image
    left = [n for n in pictures if n not in found.values()]
    without = [t for t in tracks if t not in found]
    if len(left) == 1 and len(without) == 1:
        found[without[0]] = left[0]
    elif len(tracks) == 1 and left:
        raise ValueError('the mod adds several track pictures: ' + ', '.join(sorted(pictures)))
    return [(t, found.get(t), pictures.get(found.get(t))) for t in tracks]


def owner(name, idents):
    """The track id a file or archive name is named after (the longest one it holds), or None."""
    name = squeezed(name)
    named = [i for i in idents if squeezed(i) in name]
    return max(named, key=len) if named else None


def zone_name(entries, track):
    """The English name of a track, from its zone info, or None."""
    base = track.rsplit('/', 1)[-1]
    data = entry(entries, f'maps/{track.rsplit("/", 1)[0]}/{base}_zoneInfo_en.lng')
    if not data:
        return None
    try:
        z = File(data)
        first = min(o for o, t in z.typed.items() if t == 'igLocalizedStringData')
        name = z.strings[z.u64(first + 0x10)].strip()
    except (ValueError, IndexError, KeyError):
        return None
    return name[:80] or None


def convert(game_folder, source, output, log=print):
    """The package folders written in `output` (a new folder named after each track id). Returns
    ([package folders], [global changes left out])."""
    installed = te.notice(game_folder)
    if installed:
        raise ValueError(installed)
    game = cd.Game(game_folder)
    try:
        with tempfile.TemporaryDirectory() as work:
            folder = source_folder(source, work)
            meta = json.loads((folder / 'mod.json').read_text(encoding='utf-8')) if (folder / 'mod.json').is_file() else {}
            paks = sorted(p for p in (folder / 'archives').iterdir() if p.suffix.casefold() == '.pak')
            updates = [p for p in paks if p.name.casefold().startswith('update')]
            if len(updates) != 1:
                raise ValueError('expected one update archive (update3.pak) in the mod')
            entries = {logical: (kind, data) for kind, logical, data in cd.delta(game, updates[0])}
            tracks = find_tracks(game, entries)
            idents = [track.rsplit('/', 1)[-1].casefold() for track, _, _ in tracks]
            several = len(tracks) > 1
            own = {i: [] for i in idents}
            bad = []
            for pak in paks:
                if pak in updates:
                    continue
                stem = pak.name.casefold()
                mine = max((i for i in idents if stem.startswith(i)), key=len, default=None)
                (own[mine] if mine else bad).append(pak)
            if bad:
                raise ValueError(f'archives not named after the track ({", ".join(idents)}): '
                                 + ', '.join(p.name for p in bad))
            empty = [i for i in idents if not own[i]]
            if empty:
                raise ValueError('no archives of its own for the track ' + ', '.join(empty))
            for ident in idents:
                if (Path(output) / ident).exists():
                    raise ValueError(f'{Path(output) / ident} already exists')
            dropped = sorted(logical for logical, (kind, _) in entries.items() if kind == cd.GLOBAL)
            # Files the game does not have go to the track they name; with several tracks, one
            # naming none goes to the first.
            new_files = {i: [] for i in idents}
            for logical, (kind, _) in entries.items():
                if kind == cd.NEW:
                    new_files[owner(logical, idents) or idents[0]].append(logical)
            # Unchanged game files the mod carries that the game keeps only in archives it loads
            # for one track or menu (Sewer Speedway's scripts and menu textures, for Neo Bowser
            # City) stay, in shared/: the track needs them loaded with it. Every track gets them.
            kept = [logical for logical, (kind, _) in entries.items()
                    if kind == cd.FILLER and cd.streamed_only(game, logical)]
            mine = entries.get(next((p for p in entries if p.casefold() == tr.CHUNKS.casefold()), ''), (None, b''))[1]
            listed = set()
            if mine:
                theirs = game.newest(tr.CHUNKS)
                listed = ({p.casefold() for _, p in tr.chunk_entries(File(mine))[1]}
                          - ({p.casefold() for _, p in tr.chunk_entries(File(theirs))[1]} if theirs else set()))
            made = []
            for (track, image, texture), ident in zip(tracks, idents):
                dest = Path(output) / ident
                (dest / 'archives').mkdir(parents=True)
                fixed = 0
                rules = []
                for pak in own[ident]:
                    shutil.copy2(pak, dest / 'archives' / pak.name)
                    with_maps = Archive(pak)
                    try:
                        maps = te.map_files(with_maps)
                    finally:
                        with_maps.f.close()
                    if maps:
                        rules.append(te.rule_of(maps.values()))
                    fixed += sum(te.fix_archive(dest / 'archives' / pak.name).values())
                compat = content_compat.assess(game, own[ident], rules[0] if rules else te.ALL,
                                               meta.get('id'), meta.get('version'))
                for logical, folder_name in [(p, 'files') for p in new_files[ident]] + [(p, 'shared') for p in kept]:
                    target = cp.under(dest / folder_name, logical)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(entries[logical][1])
                for extra in folder.iterdir():
                    if extra.is_file() and extra.name.casefold().endswith(cp.EXTRAS):
                        shutil.copy2(extra, dest / extra.name)
                (dest / cp.MARKER).write_text(cp.MARKER_TEXT + '\n', encoding='utf-8')
                name = (zone_name(entries, track) or f'{meta.get("name") or ident} ({ident})') if several \
                    else meta.get('name') or ident
                m = {'format': cp.FORMAT, 'type': 'track', 'id': ident,
                     'name': {'en': name}, 'version': meta.get('version') or '1.0.0',
                     'author': meta.get('author') or 'unknown', 'map': track}
                if image:
                    m.update(image=image, texture=texture)
                online = (meta.get('compatibility') or {}).get('online')
                m['online'] = 'private' if online == 'allowed' else 'offline-only'
                # The streaming list gets what the mod's own list added for its files, and nothing
                # more (content_package.track).
                if mine:
                    m['stream'] = [p for p in kept + new_files[ident] if p.casefold() in listed]
                m['credits'] = (f'{name} by {m["author"]}. Converted by the OCTR launcher from the mod '
                                f'{meta.get("id", Path(source).stem)} {m["version"]}'
                                + (f', without {len(dropped)} changes to game files.' if dropped else '.'))
                m['convertedFrom'] = {'id': meta.get('id'), 'version': meta.get('version'), 'author': meta.get('author'),
                                      'source': Path(source).name, 'left_out': dropped}
                if fixed:
                    m['convertedFrom']['fixed_reference_counts'] = fixed
                m['convertedFrom']['compatibility'] = compat
                m['files'] = []
                for path in sorted(p for p in dest.rglob('*') if p.is_file() and p.name not in (cp.MARKER, cp.MANIFEST)):
                    with path.open('rb') as f:
                        digest = hashlib.file_digest(f, 'sha256').hexdigest()
                    m['files'].append({'file': path.relative_to(dest).as_posix(), 'sha256': digest,
                                       'bytes': path.stat().st_size})
                cp.validate_manifest(m)
                (dest / cp.MANIFEST).write_text(json.dumps(m, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
                cp.read_folder(dest)
                log(f'{ident}: {name} {m["version"]} by {m["author"]}'
                    + (f'; left out {len(dropped)} changes to game files' if dropped else '')
                    + (f'; fixed the reference counts of {fixed} map objects' if fixed else '')
                    + f'; compatibility {compat["level"]}' + (' (tried in game)' if compat['tested'] else '')
                    + (': ' + ', '.join(f'{k} {v}' for k, v in compat['issues'].items()) if compat['issues'] else ''))
                made.append(dest)
            return made, dropped
    finally:
        game.close()


def pack(folder, zip_path):
    """A package folder as a ZIP (stored: the archives are compressed already)."""
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_STORED) as z:
        for path in sorted(p for p in Path(folder).rglob('*') if p.is_file()):
            z.write(path, path.relative_to(folder).as_posix())
    return zip_path


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('game', type=Path)
    parser.add_argument('mod', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--zip', action='store_true', help='also write <id>.zip and remove the folder')
    args = parser.parse_args(argv)
    try:
        dest, dropped = convert(args.game, args.mod, args.output)
    except (ValueError, OSError, zipfile.BadZipFile) as error:
        print(f'Conversion failed: {error}')
        return 1
    for path in dropped:
        print(f'  left out (changes a game file): {path}')
    if args.zip:
        pack(dest, dest.with_suffix('.zip'))
        shutil.rmtree(dest)
        print(dest.with_suffix('.zip'))
    else:
        print(dest)
    return 0


if __name__ == '__main__':
    sys.exit(main())
