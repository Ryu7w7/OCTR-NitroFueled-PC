#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""How well a converted track works as external content: full or partial compatibility.

Some community tracks play but crash the game, or leave memory allocated, when the next level
loads. What tells them apart, found on the tracks tried in game (2026-11-14):

  game_scripts   changed copies of the game's scripts (vsc/) in the track's own archive: the game
                 registers a script class once, by name, and a second, different copy breaks its
                 list of classes when the track unloads (Suzuka 0.7.2: 2, Papu's Pyramid's; Neo
                 Bowser City 1.2.0: 37). The tracks that work have none.
  game_menus     changed copies of the game's menus and HUD (ui/): Neo Bowser City 104.
  game_data      changed copies of other game data (loosedata/, packages/, cutscene/, actors/).
  other_tool     map files keeping reference counts of reached pointers only (scripts/track_editor.py):
                 made by another tool than CTR Track Editor; Neo Bowser City leaves memory allocated
                 after it and the next level can crash.

Changed textures, sounds, models and materials are the track's own look and are fine (Sand Race
carries 44 textures). A track with none of the above has full compatibility; tracks made with CTR
Track Editor for Nitro-Fueled 0.6 and later are made that way (tried in game by Kyouko8).

TESTED names the mods tried in game; they keep that result whatever the checks say.
"""
import hashlib
from collections import Counter
from pathlib import Path

from graphics_tab import Archive

FULL, PARTIAL = 'full', 'partial'
# Kinds of game files a track must not carry changed (module docstring), by first path part.
KINDS = {'vsc': 'game_scripts', 'ui': 'game_menus', 'loosedata': 'game_data', 'packages': 'game_data',
         'cutscene': 'game_data', 'actors': 'game_data'}
# (mod id, version) -> result in game. Tried by Kyouko8, 2026-11-14.
TESTED = {
    ('ctr-editor-backrooms', '1.5.1'): FULL,      # after the reference count fix: no leak, no crash
    ('ctr-editor-sand-race', '1.0.0'): FULL,      # CTR Track Editor 0.6
    ('suzuka', '0.7.2'): PARTIAL,                 # the next level crashes (Coco Park, Neo Bowser City)
    ('nbc', '1.2.0'): PARTIAL,                    # 154 blocks left allocated; the next level can crash
}


def changed_game_files(game, paks):
    """Counter {issue: n} of the changed copies of game files (KINDS) in a track's own archives."""
    issues = Counter()
    for pak in paks:
        archive = Archive(Path(pak))
        try:
            for i in range(len(archive.names)):
                logical = archive.logical(i).casefold()
                issue = KINDS.get(logical.split('/', 1)[0])
                if issue is None or logical not in game.copies:
                    continue
                if hashlib.sha256(archive.read(i)).hexdigest() not in game.hashes(logical):
                    issues[issue] += 1
        finally:
            archive.f.close()
    return issues


def assess(game, paks, rule, mod_id=None, version=None):
    """{'level': full|partial, 'tested': bool, 'issues': {issue: n}} of a converted track: `paks`
    its own archives, `rule` the reference count convention of its map files."""
    import track_editor
    issues = changed_game_files(game, paks)
    if rule == track_editor.REACHED:
        issues['other_tool'] = 1
    tested = TESTED.get((mod_id, version))
    level = tested or (PARTIAL if issues else FULL)
    return {'level': level, 'tested': tested is not None, 'issues': dict(sorted(issues.items()))}


def of_manifest(m):
    """The compatibility of a package's content.json: what its conversion found, or the result of
    the mod it was converted from when it was tried in game; None when not known."""
    origin = m.get('convertedFrom') or {}
    found = origin.get('compatibility')
    tested = TESTED.get((origin.get('id'), origin.get('version')))
    if tested:
        return {'level': tested, 'tested': True, 'issues': (found or {}).get('issues', {})}
    return found


def clashes(packages, cache=None):
    """{(id, id): n} of the pairs of packages whose archives hold n files of the same name with
    different contents. The game keeps one of them for both (the one it finds first): CTR Track
    Editor saves the game textures a track changes under their own names, so with several of its
    tracks on, every track can show another one's road. `packages` is [(manifest, folder)];
    `cache` a JSON file keeping the hashes, by archive (its sha256 in the manifest)."""
    import json
    import content_package as cp
    known = {}
    if cache is not None and Path(cache).is_file():
        try:
            known = json.loads(Path(cache).read_text(encoding='utf-8'))
        except ValueError:
            known = {}
    sums = {r['file']: r['sha256'] for m, _ in packages for r in m['files']}
    where = {}  # id -> {logical: (archive file, its sha256, index, size)}
    for m, folder in packages:
        mine = where.setdefault(m['id'], {})
        for target, source in cp.archives(m, folder):
            archive = Archive(Path(source))
            try:
                for i in range(len(archive.names)):
                    mine.setdefault(archive.logical(i).casefold(),
                                    (Path(source), sums.get(target), i, archive.infos[i][2]))
            finally:
                archive.f.close()
    seen = Counter(n for names in where.values() for n in names)
    shared = {n for n, k in seen.items() if k > 1}
    if not shared:
        return {}
    # Hash what has to be: files of a shared name, of one size in more than one package.
    sizes = {}
    for ident, names in where.items():
        for n in shared & names.keys():
            sizes.setdefault(n, set()).add(names[n][3])
    need = {}
    for ident, names in where.items():
        for n in shared & names.keys():
            source, digest, i, size = names[n]
            if len(sizes[n]) == 1 and (known.get(digest) or {}).get(n) is None:
                need.setdefault(source, []).append((n, i, digest))
    changed = False
    for source, wanted in need.items():
        archive = Archive(source)
        try:
            for n, i, digest in wanted:
                known.setdefault(digest, {})[n] = hashlib.sha256(archive.read(i)).hexdigest()[:16]
                changed = True
        finally:
            archive.f.close()
    if changed and cache is not None:
        Path(cache).parent.mkdir(parents=True, exist_ok=True)
        Path(cache).write_text(json.dumps(known), encoding='utf-8')
    result = Counter()
    idents = sorted(where)
    for a_i, a in enumerate(idents):
        for b in idents[a_i + 1:]:
            for n in shared & where[a].keys() & where[b].keys():
                x, y = where[a][n], where[b][n]
                if x[3] != y[3] or (known.get(x[1]) or {}).get(n) != (known.get(y[1]) or {}).get(n):
                    result[(a, b)] += 1
    return dict(result)


def field(compat):
    """One listing field: "level;tested;issue=n,..." or "" (scripts/content_cli.py)."""
    if not compat:
        return ''
    issues = ','.join(f'{k}={v}' for k, v in sorted(compat.get('issues', {}).items()))
    return f'{compat["level"]};{1 if compat.get("tested") else 0};{issues}'
