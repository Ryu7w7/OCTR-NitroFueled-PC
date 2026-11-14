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


def field(compat):
    """One listing field: "level;tested;issue=n,..." or "" (scripts/content_cli.py)."""
    if not compat:
        return ''
    issues = ','.join(f'{k}={v}' for k, v in sorted(compat.get('issues', {}).items()))
    return f'{compat["level"]};{1 if compat.get("tested") else 0};{issues}'
