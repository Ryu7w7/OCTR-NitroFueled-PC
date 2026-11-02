#!/usr/bin/env python3
"""Creator pinning and explicit player import for the optional GameBanana bridge.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import json
from pathlib import Path
import sys
import threading

from mod_sync.cache import Cache
from mod_sync.gamebanana import GameBanana
from mod_sync.models import Offer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    pin = sub.add_parser('pin', help='match a local ZIP to its exact GameBanana upload')
    pin.add_argument('zip', type=Path)
    pin.add_argument('--out', type=Path, required=True)
    stage = sub.add_parser('download', help='download and validate into staging only')
    stage.add_argument('offer', type=Path)
    stage.add_argument('--data', type=Path, required=True)
    install = sub.add_parser('import', help='import one staged ZIP, disabled, while the game is closed')
    install.add_argument('offer', type=Path)
    install.add_argument('--data', type=Path, required=True)
    install.add_argument('--patches', type=Path, help='defaults to DATA/patches')
    stage.add_argument('--game', type=Path, help='game folder: read edition/version from param.sfo')
    install.add_argument('--game', type=Path, help='required for executable patch packages')
    args = parser.parse_args()
    if args.command == 'pin':
        offer = GameBanana().pin(args.zip)
        args.out.write_text(json.dumps(offer.to_dict(), indent=2) + '\n', encoding='utf-8')
        print('Pinned exact upload:', args.out)
        return
    if args.offer.stat().st_size > 4096:
        raise ValueError('Offer document exceeds limit')
    offer = Offer.from_dict(json.loads(args.offer.read_text(encoding='utf-8')))
    from game_check import sfo_info
    info = sfo_info(args.game) if args.game else {}
    cache = Cache(args.data / 'mod-state/sync/downloads',
                  title_id=info.get('TITLE_ID', ''), app_version=info.get('APP_VER', ''))
    if args.command == 'download':
        print('Downloaded for review:', cache.stage(offer, threading.Event()))
    else:
        print('Imported, disabled:', cache.import_for_review(offer, args.data / 'mods', args.patches or args.data / 'patches'))
        print('Review and enable it in OCTR Mods, then relaunch the game.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        print('Mod sync:', error, file=sys.stderr)
        sys.exit(1)
