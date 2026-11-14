#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A classic track mod converted to an external content package (scripts/content_package.py).

A classic track mod is a ZIP or folder with the track's own archives (archives/t922_suzuka.pak,
its flybys) and a whole update3.pak with the track's registration and its own files in it. The
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
The package keeps the mod's name, version and author, and says what it was converted from.

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


def find_track(game, entries):
    """(map, image, texture) the mod's registration adds: the new CGuiOctaneLevelData, and the
    new LevelImage_* of LevelSelect_materials with its texture."""
    def entry(path):
        key = next((k for k in entries if k.casefold() == path.casefold()), None)
        return entries[key][1] if key else None
    gui = entry(tr.GUI)
    if gui is None:
        raise ValueError('the mod registers no track (no guisystemdata.igz in its update archive)')
    have = tr.registered(game.newest(tr.GUI))
    new = sorted(tr.registered(gui) - have)
    if len(new) != 1:
        raise ValueError(f'expected one new track in the mod, found {len(new)}: ' + ', '.join(new))
    g = File(gui)
    track = next(g.string_at(o + 0x10) for o, t in g.typed.items()
                 if t == tr.LEVEL_DATA and g.string_at(o + 0x10).casefold() == new[0])
    image = texture = None
    materials = entry(tr.MATERIALS)
    if materials is not None:
        old = set(tr.named(File(game.newest(tr.MATERIALS))))
        z = File(materials)
        added = [(n, o) for n, o in tr.named(z).items() if n.startswith(tr.IMAGE) and n not in old]
        if len(added) > 1:
            raise ValueError('the mod adds several track pictures: ' + ', '.join(n for n, _ in added))
        if added:
            image, o = added[0]
            textures = [z.handle(z.u32(site))[1] for x in tr.owned(z, o) if z.typed[x] == 'igGraphicsTexture'
                        for tag, site in z.sites_in(x, z.size(x)) if tag == 'RHND']
            if len(textures) != 1:
                raise ValueError(f'{image}: expected one texture')
            texture = textures[0]
    return track, image, texture


def convert(game_folder, source, output, log=print):
    """The package folder written in `output` (a new folder named after the track id). Returns
    (package folder, [global changes left out])."""
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
            track, image, texture = find_track(game, entries)
            ident = track.rsplit('/', 1)[-1].casefold()
            own = [p for p in paks if p not in updates]
            bad = [p.name for p in own if not p.name.casefold().startswith(ident)]
            if bad:
                raise ValueError(f'archives not named after the track ({ident}): ' + ', '.join(bad))
            dropped = sorted(logical for logical, (kind, _) in entries.items() if kind == cd.GLOBAL)
            dest = Path(output) / ident
            if dest.exists():
                raise ValueError(f'{dest} already exists')
            (dest / 'archives').mkdir(parents=True)
            fixed = 0
            rules = []
            for pak in own:
                shutil.copy2(pak, dest / 'archives' / pak.name)
                with_maps = Archive(pak)
                try:
                    maps = te.map_files(with_maps)
                finally:
                    with_maps.f.close()
                if maps:
                    rules.append(te.rule_of(maps.values()))
                fixed += sum(te.fix_archive(dest / 'archives' / pak.name).values())
            compat = content_compat.assess(game, own, rules[0] if rules else te.ALL,
                                           meta.get('id'), meta.get('version'))
            # Unchanged game files the mod carries that the game keeps only in archives it loads
            # for one track or menu (Sewer Speedway's scripts and menu textures, for Neo Bowser
            # City) stay, in shared/: the track needs them loaded with it.
            kept = []
            for logical, (kind, data) in entries.items():
                if kind == cd.NEW:
                    folder_name = 'files'
                elif kind == cd.FILLER and cd.streamed_only(game, logical):
                    kept.append(logical)
                    folder_name = 'shared'
                else:
                    continue
                target = cp.under(dest / folder_name, logical)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            for extra in folder.iterdir():
                if extra.is_file() and extra.name.casefold().endswith(cp.EXTRAS):
                    shutil.copy2(extra, dest / extra.name)
            (dest / cp.MARKER).write_text(cp.MARKER_TEXT + '\n', encoding='utf-8')
            m = {'format': cp.FORMAT, 'type': 'track', 'id': ident,
                 'name': {'en': meta.get('name') or ident}, 'version': meta.get('version') or '1.0.0',
                 'author': meta.get('author') or 'unknown', 'map': track}
            if image:
                m.update(image=image, texture=texture)
            online = (meta.get('compatibility') or {}).get('online')
            m['online'] = 'private' if online == 'allowed' else 'offline-only'
            # The streaming list gets what the mod's own list added for its files, and nothing
            # more (content_package.track).
            mine = entries.get(next((p for p in entries if p.casefold() == tr.CHUNKS.casefold()), ''), (None, b''))[1]
            if mine:
                from igz_merge import File
                theirs = game.newest(tr.CHUNKS)
                own = {p.casefold() for _, p in tr.chunk_entries(File(mine))[1]}
                base = {p.casefold() for _, p in tr.chunk_entries(File(theirs))[1]} if theirs else set()
                ours = kept + [p for p, (kind, _) in entries.items() if kind == cd.NEW]
                m['stream'] = [p for p in ours if p.casefold() in own - base]
            m['credits'] = (f'{m["name"]["en"]} by {m["author"]}. Converted by the OCTR launcher from the mod '
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
            log(f'{ident}: {m["name"]["en"]} {m["version"]} by {m["author"]}'
                + (f'; left out {len(dropped)} changes to game files' if dropped else '')
                + (f'; fixed the reference counts of {fixed} map objects' if fixed else '')
                + f'; compatibility {compat["level"]}' + (' (tried in game)' if compat['tested'] else '')
                + (': ' + ', '.join(f'{k} {v}' for k, v in compat['issues'].items()) if compat['issues'] else ''))
            return dest, dropped
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
