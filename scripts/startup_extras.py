#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""run.sh's share of run.py's archive-patch steps after the button prompts, in run.py's order and
with the same modules, so the Linux launch has what the Windows one has:
  season_art      the seasonal title screen (bbport.ini title_theme)
  signed_in_name  online, the nickname bottom left of the menus (as the Xbox game shows it)
  ctr_tv_logo     the port's CTR TV logo
  tacho_pack      Pack Tacho: its HUD, reserves bar, leaders list and main menu files. Its executable
                  patches (scripts/tacho_pack/patches) only work with these files: without them a
                  race crashed while loading (guest 0x1469081 USA, 0x1469071 EU).
  skin_portraits  use skin-specific portraits in race leaders and minimap
  track_names     the tracks' own names for the Discord presence
Each step keeps the patches made before it for the same archive, as in run.py.

Output, one item a line: patch=<archive patch> (every patch of the launch, the --base ones first,
a later one for the same archive in the earlier one's place), env.KEY=VALUE (exported by run.sh),
fail=<reason> (the launch stops, exit 3: a changed Pack Tacho), anything else a message.
"""
import argparse
import os
import sys
from pathlib import Path

import archive_patch
import ctr_tv_logo
import season_art
import signed_in_name
import skin_portraits
import tacho_pack
import track_names


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('game', type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--settings', required=True, type=Path, help='bbport.ini')
    p.add_argument('--base', action='append', default=[], help='an archive patch made before (repeatable)')
    a = p.parse_args()
    env = dict(os.environ)
    before = dict(env)
    base = {archive_patch.read(path)[1]: Path(path) for path in a.base}
    for name, module, otherwise in (('Title theme', season_art, "the game's own"),
                                    ('Nickname in the menus', signed_in_name, 'not shown'),
                                    ('CTR TV logo', ctr_tv_logo, "the game's own")):
        try:
            base.update(module.prepare(a.game, a.out, a.settings, env, base))
        except (OSError, ValueError) as error:
            print(f'{name}: {error}; {otherwise}', flush=True)
    try:
        made, extra = tacho_pack.prepare(a.game, a.out, a.settings, env, base)
        base.update(made)
        env.update(extra)
    except tacho_pack.Modified as error:
        print(f'fail=Pack Tacho: {error}', flush=True)
        sys.exit(3)
    except (OSError, ValueError, KeyError) as error:
        print(f"Pack Tacho: {error}; the game's own files", flush=True)
    try:
        base.update(skin_portraits.prepare(a.game, a.out, base))
    except (OSError, ValueError, KeyError) as error:
        print(f"Skin portraits: {error}; the game's own portraits", flush=True)
    presence = Path(env.setdefault('BB_PRESENCE_FILE', str((a.out / 'presence' / 'presence.txt').resolve())))
    try:
        track_names.prepare(a.game, presence.parent)
    except (OSError, ValueError, KeyError) as error:
        print(f'Track names for Discord: {error}', flush=True)
    for patch in base.values():
        print(f'patch={patch}', flush=True)
    for key, value in env.items():
        if before.get(key) != value:
            print(f'env.{key}={value}', flush=True)


if __name__ == '__main__':
    main()
