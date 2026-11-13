#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""What a mod's update archive adds to the game: each entry against the game's own copies.

A track or racer mod ships an update archive (archives/update3.pak, update4.pak...) that holds the
whole of every game file it needed to change, and often many more it copied unchanged. Each entry
is one of:

    filler      the same bytes as a copy the game already has (dropped by the content converter)
    new         a path the game does not have (the mod's own files: zone info, textures, materials)
    registry    a game file that registers content (track lists, menus, racer lists): merged
    global      any other game file the mod changed: it changes the game for every track

Paths compare without case, as the PS4 file system does. A game file is the newest copy in load
order: update2.pak, update.pak, then the base archives (a path several base archives share, such
as a track's common scripts, matches any of their copies).

    python scripts/content_delta.py GAME_FOLDER MOD_ARCHIVE.pak [MOD_ARCHIVE.pak ...]

Reference mods used to work this out: Neo Bowser City (PanchoPewPew) and Suzuka (CTRNFModding).
"""
import argparse
import hashlib
import re
import sys
from pathlib import Path

from graphics_tab import Archive

FILLER, NEW, REGISTRY, GLOBAL = 'filler', 'new', 'registry', 'global'

# Game files that register content; a mod's copy of these is merged, never served whole.
REGISTRY_FILES = {
    'loosedata/guisystemdata.igz',                       # track pages (CGuiOctaneAreaLevelList)
    'ui/octane_levelselect_proto.igz',                   # track menu (CGuiOctaneLevelSelectData)
    'packages/generated/maps/chunkinfos_pkg.igz',        # streaming dependencies of the maps
    'materialinstances/ui/levelselect_materials.igz',    # track menu pictures (LevelImage_*)
    'packages/generated/ui/domains/juicedomain_onlinemenus_pkg.igz',
    'packages/generated/ui/domains/juicedomain_startscreen_pkg.igz',
    'loosedata/systemdata.igz',                          # racers, karts, cups
    'characterlisting.igz',                              # racer names
}
# The newest archives first: their copy is the one the game loads.
UPDATES = ('update2.pak', 'update.pak')


def is_registry(path):
    p = path.casefold()
    return p in REGISTRY_FILES or (p.startswith('loosedata/systemdata_') and p.endswith('.lng'))


class Game:
    """The game's archives, indexed by logical path (no case); entries hashed when first asked."""

    def __init__(self, folder):
        self.folder = Path(folder) / 'archives'
        if not self.folder.is_dir():
            raise ValueError(f'{folder}: no archives folder')
        order = [self.folder / n for n in UPDATES if (self.folder / n).is_file()]
        order += sorted(p for p in self.folder.glob('*.pak')
                        if p.name.casefold() not in UPDATES and not p.name.casefold().startswith('update'))
        self.copies = {}  # path -> [(archive path, index)], newest first
        for path in order:
            try:
                archive = Archive(path)
            except ValueError:
                continue
            try:
                for i in range(len(archive.names)):
                    self.copies.setdefault(archive.logical(i).casefold(), []).append((path, i))
            finally:
                archive.f.close()
        self._hashes = {}
        self._open = {}

    def read(self, path, i):
        archive = self._open.get(path)
        if archive is None:
            archive = self._open[path] = Archive(path)
        return archive.read(i)

    def hashes(self, logical):
        """sha256 of each game copy of a path, newest first."""
        key = logical.casefold()
        if key not in self._hashes:
            self._hashes[key] = [hashlib.sha256(self.read(p, i)).hexdigest()
                                 for p, i in self.copies.get(key, [])]
        return self._hashes[key]

    def newest(self, logical):
        """The bytes the game loads for a path, or None."""
        copies = self.copies.get(logical.casefold())
        return self.read(*copies[0]) if copies else None

    def close(self):
        for archive in self._open.values():
            archive.f.close()
        self._open.clear()


# Archives the game always has open: a file in one of them can be found at any time.
ALWAYS_LOADED = re.compile(r'(update\d*|permanent.*|juicedomain_permanent)\.pak\Z', re.I)


def streamed_only(game, logical):
    """True when every game copy of a path is in an archive loaded only for a track or a menu."""
    copies = game.copies.get(logical.casefold(), [])
    return bool(copies) and not any(ALWAYS_LOADED.match(Path(p).name) for p, _ in copies)


def classify(game, logical, data):
    if logical.casefold() not in game.copies:
        return NEW
    if hashlib.sha256(data).hexdigest() in game.hashes(logical):
        return FILLER
    return REGISTRY if is_registry(logical) else GLOBAL


def delta(game, archive_path):
    """[(kind, logical path, bytes)] of a mod archive, in its order."""
    archive = Archive(Path(archive_path))
    try:
        out = []
        for i in range(len(archive.names)):
            logical, data = archive.logical(i), archive.read(i)
            out.append((classify(game, logical, data), logical, data))
        return out
    finally:
        archive.f.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('game', type=Path)
    parser.add_argument('archives', type=Path, nargs='+')
    parser.add_argument('--all', action='store_true', help='list filler entries too')
    args = parser.parse_args(argv)
    game = Game(args.game)
    try:
        for path in args.archives:
            entries = delta(game, path)
            counts = {k: sum(1 for e in entries if e[0] == k) for k in (FILLER, NEW, REGISTRY, GLOBAL)}
            print(f'{path}: {len(entries)} entries, ' + ', '.join(f'{v} {k}' for k, v in counts.items()))
            for kind, logical, data in entries:
                if kind != FILLER or args.all:
                    print(f'  {kind:8} {len(data):>10}  {logical}')
    finally:
        game.close()


if __name__ == '__main__':
    sys.exit(main())
