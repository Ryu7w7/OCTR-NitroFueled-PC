"""The player's nickname at the bottom left of the menus' button bar while online, as the Xbox One
game shows its gamertag there.

The menus' button legend runs vsc/common/common_ui_signedInUser.igz (juicedomain_permanent.pak):
gen8ConsoleSwitch -> isSignedIn -> getPrimaryUsername (the first local CPlayer's _gamertag, here the
RyuNET nickname) -> the placeable's text. Only the switch's output for Xbox One (output 1) leads
there; the PlayStation 4 game, which this port runs, always takes output 0 (0x10de070), which
leads nowhere: Microsoft asks for the gamertag on screen, Sony does not. This points output 0 of
that one script's switch where output 1 goes; the switch itself, which other scripts use, stays as
it is. Offline, isSignedIn is false (sceNpGetState, src/runtime_np_online.c) and the text stays
empty, as on Xbox; the patch is made for online sessions only anyway.

An archive patch (scripts/archive_patch.py) keeping the base patch of the same archive, kept
between launches while the archives and that base are the same.
SPDX-License-Identifier: GPL-2.0-or-later
"""
import hashlib
import json
from pathlib import Path

import archive_patch
from archive_reader import Archive
from igz_file import Igz
from mod_file_paths import resolve

ENTRY = 'vsc/common/common_ui_signedinuser.igz'
ARCHIVES = ('update2.pak', 'update.pak', 'juicedomain_permanent.pak')  # newest first
LIST_COUNT, LIST_ITEMS = 0x0C, 0x20                                      # igDataList
FORMAT = 1


def say(text):
    print(f'Nickname in the menus: {text}', flush=True)


def rebuild(blob):
    """The script with its console switch's output 0 going where output 1 goes."""
    g = Igz(blob)
    pointers = set(g.sites['ROFS'])
    found = []
    for off, kind in g.objects():
        if kind != 'igVscActionNodeList' or g.u32(off + LIST_COUNT) != 4 or off + LIST_ITEMS not in pointers:
            continue
        items = g.u64(off + LIST_ITEMS)
        slots = [items + 8 * i for i in range(4)]
        linked = [s in pointers for s in slots]
        if linked == [False, True, False, False] and g.u64(slots[0]) == 0:
            found.append(slots)
    if len(found) != 1:
        raise ValueError(f'{len(found)} console switches found in the script, expected one')
    slots = found[0]
    g.set_u64(slots[0], g.u64(slots[1]))
    g.add_site('ROFS', slots[0])
    return g.build()


def prepare(game, out, config, env, base):
    """{guest archive: patch file}; none offline."""
    if env.get('BB_ONLINE') != '1':
        return {}
    folder = Path(out) / 'signed_in_name'
    archives = [resolve(Path(game) / 'archives' / name) for name in ARCHIVES]
    archives = [a for a in archives if a.is_file()]
    stamp = {'format': FORMAT, 'archives': {a.name: [a.stat().st_size, a.stat().st_mtime_ns] for a in archives},
             'base': {g: hashlib.sha256(Path(p).read_bytes()).hexdigest() for g, p in sorted(base.items())}}
    stamp_file = folder / 'stamp.json'
    try:
        made = json.loads(stamp_file.read_text(encoding='utf-8'))
        if made['stamp'] == stamp and all(Path(p).is_file() for p in made['patches'].values()):
            return {g: Path(p) for g, p in made['patches'].items()}
    except (OSError, ValueError, KeyError):
        pass
    for path in archives:  # the newest copy of the script is the one the game reads
        archive = Archive(path)
        entry = next((f for f in archive.files if f['path'].replace('\\', '/').lower().endswith(ENTRY)), None)
        if entry is None:
            continue
        new = rebuild(bytes(archive.extract(entry)))
        Igz(new)  # it must read back
        folder.mkdir(parents=True, exist_ok=True)
        guest = f'/app0/archives/{path.name}'
        target = folder / (path.stem + '.bbpatch')
        archive_patch.write_entries_patch(target, path, guest, {entry['index']: new}, base.get(guest))
        stamp_file.write_text(json.dumps({'stamp': stamp, 'patches': {guest: str(target)}}), encoding='utf-8')
        say('shown while online')
        return {guest: target}
    say('the script is not in the archives; not shown')
    return {}
