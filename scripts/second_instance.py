#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A second game on this PC for a second player (issue #287: two players online in the same room
from one PC, each with a controller, a screen and a nickname of their own).

One launch per data folder (scripts/launch_lock.py: two games must not write the same saves,
caches and bbport.ini), so the second game has a folder of its own, <data>/instance2/: its own
bbport.ini (copied from the main one the first time, then its own: its controllers, its screen),
saves, online key and nickname, and another UDP port. The game, the tracks (content/) and the
mods are the main installation's. No cloud saves, no Discord presence for it.

The launcher (Tools > Second player, or beside Stop while the game runs) starts it with the
environment it prepared for the main game:
  second_instance.py --data <main data folder> --nick <nickname>
Prints one JSON object: {"ok": true, "folder": ...} or {"ok": false, "error": ...}.
"""
import argparse
import json
import os
import re
import secrets
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FOLDER = 'instance2'
PORT = '3076'                     # the main game's is 3074
NICK = re.compile(r'[A-Za-z0-9_-]{3,16}\Z')
# Keys of bbport.ini the second game sets itself; the rest is the main one's the first time.
OWN = ('online_name', 'user_name')


def ini_lines(path):
    try:
        return Path(path).read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        return []


def value(lines, key):
    for line in lines:
        k, sep, v = line.partition('=')
        if sep and k.strip() == key:
            return v.strip()
    return ''


def prepare(data, nick, main_ini):
    folder = Path(data) / FOLDER
    folder.mkdir(parents=True, exist_ok=True)
    ini = folder / 'bbport.ini'
    lines = ini_lines(ini) or ini_lines(main_ini)     # its own from the second start on
    forced = {'online_name': nick, 'user_name': nick, 'cloud_sync': '0'}
    out = [line for line in lines if line.partition('=')[0].strip() not in forced]
    out += [f'{k}={v}' for k, v in forced.items()]
    ini.write_text('\n'.join(out) + '\n', encoding='utf-8')
    key = folder / 'ryunet_online.key'
    if not key.exists():   # its own: the main game's backup (run.py key_store) stays the main one's
        key.write_text(secrets.token_hex(32) + '\n', encoding='ascii')
    return folder, ini


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data', type=Path, required=True)
    p.add_argument('--nick', required=True)
    a = p.parse_args()
    main_ini = Path(os.environ.get('BB_CONFIG') or a.data / 'bbport.ini')
    main_nick = value(ini_lines(main_ini), 'online_name') or value(ini_lines(main_ini), 'user_name')
    if not NICK.match(a.nick):
        print(json.dumps({'ok': False, 'error': 'nick'}))
        return
    if main_nick and a.nick.casefold() == main_nick.casefold():
        print(json.dumps({'ok': False, 'error': 'same_nick'}))
        return
    try:
        folder, ini = prepare(a.data, a.nick, main_ini)
        env = dict(os.environ, BB_DATA_DIR=str(folder), BB_CONFIG=str(ini), BB_USER_DIR=str(folder / 'user'),
                   BB_ONLINE_NAME=a.nick, BB_USER_NAME=a.nick, BB_ONLINE_PORT=PORT)
        env.pop('BB_PRESENCE_FILE', None)   # Discord shows the main game
        if os.name == 'nt':
            command = [sys.executable, '-X', 'utf8', str(ROOT / 'run.py')]
            flags = subprocess.CREATE_NO_WINDOW | subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            command = ['bash', str(ROOT / 'run.sh')]
            flags = 0
        with open(folder / 'last_console.txt', 'w', encoding='utf-8') as console:
            subprocess.Popen(command, cwd=str(ROOT), env=env, stdout=console, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, creationflags=flags,
                             **({} if os.name == 'nt' else {'start_new_session': True}))
    except OSError as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return
    print(json.dumps({'ok': True, 'folder': str(folder)}, separators=(',', ':')))


if __name__ == '__main__':
    main()
