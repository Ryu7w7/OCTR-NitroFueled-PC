# SPDX-License-Identifier: GPL-2.0-or-later
"""Fixes to the game's own levels, for every player alike, whatever mods are on.

Each one is a few bytes changed in one of the game's files, checked against the file it was made for
(SHA-256 before and after). The player's own copy (the game's update archives, never a mod's) is
changed here and goes into the port's update archive (scripts/graphics_tab.py), which the game loads
after every other one (runtime_file.c picks its number): no mod can drop or override it.

  Tiny Temple respawn (Tacho, his mod tiny-temple-reaparecer 1.2.1): falling just after the last turbo
  pad puts the kart back before it, to take it again; going back from the finish line and falling puts
  it ahead. USA and EU 1.21 ship the same file (update2.pak).
  Out of Time respawn (Tacho, his mod out-of-time-reaparecer 1.1.0): the same for the final jump's turbo pad.
"""
import hashlib
from pathlib import Path

FIXES = [
    {
        'id': 'tiny_temple_respawn',
        'by': 'Tacho (tiny-temple-reaparecer 1.2.1)',
        'path': 'maps/octane/t213_tiny_temple/t213_tiny_temple_design.igz',
        'original': '3dede21ba6083ac697e0efdd0964d90d0f8fb1bcd00adb6192ef782f7d7f9d9a',
        'fixed': '2c9dde08abf13219ad4957294951028f950747e2fb5fd19587ceb9aec52e3b4e',
        # (offset, bytes): the respawn point's position, and the fall planes' heights
        'changes': [(0x34c6c, '29f3cfc6f1958945a85e13'), (0x34d9c, 'dd7d0fc0'),
                    (0x3581c, '0050c3c7'), (0x35b5c, '0050c3c7')],
    },
    {
        'id': 'out_of_time_respawn',
        'by': 'Tacho (out-of-time-reaparecer 1.1.0)',
        'path': 'maps/octane/t231_out_of_time/t231_out_of_time_race_system.igz',
        'original': '71d18ab0fb4b145b0a04ee832c20aa543757c41c399e4c1bef140f7f669b4cb4',
        'fixed': '430d32d6ed82293456aab91dc7b1bcf82008bfb1a876246a23adeccb0cafb956',
        # the respawn point before the final jump's turbo pad, and the fall zones it answers
        'changes': [(0x217bc, '79668ac66fb890c52edcf144'), (0x218ec, 'd5ae99'),
                    (0x22350, '00'), (0x22450, '00'), (0x22480, '00')],
    },
]
ARCHIVES = ('update2.pak', 'update.pak')   # where the game's own copies are, newest first


def apply(fix, data):
    """The fixed file, or None when data is not the file the fix was made for."""
    if hashlib.sha256(data).hexdigest() != fix['original']:
        return None
    out = bytearray(data)
    for offset, hex_bytes in fix['changes']:
        b = bytes.fromhex(hex_bytes)
        out[offset:offset + len(b)] = b
    out = bytes(out)
    return out if hashlib.sha256(out).hexdigest() == fix['fixed'] else None


def entries(game, log=print):
    """[(full name, logical name, bytes)] for the port's update archive: each fix applied to the game's own
    copy (the view's archive itself, not a mod's file in the mods' file map)."""
    from graphics_tab import Archive
    from mod_file_paths import resolve
    wanted = {f['path']: f for f in FIXES}
    out, done = [], set()
    for name in ARCHIVES:
        for path in dict.fromkeys((Path(game) / 'archives' / name, resolve(Path(game) / 'archives' / name))):
            if not path.is_file() or len(done) == len(wanted):
                continue
            archive = Archive(path)
            try:
                for i in range(len(archive.names)):
                    full, logical = archive.names[i]
                    fix = wanted.get(archive.logical(i).lower())
                    if not fix or fix['id'] in done:
                        continue
                    fixed = apply(fix, archive.read(i))
                    if fixed is None:
                        continue
                    out.append((full, logical, fixed))
                    done.add(fix['id'])
                    log(f"Game fixes: {fix['id']} ({fix['by']}) in {logical}")
            finally:
                archive.f.close()
    for fix in FIXES:
        if fix['id'] not in done:
            log(f"Game fixes: {fix['id']} not applied (the game's {fix['path']} is not the one it was made for)")
    return out


def digest():
    return hashlib.sha256(Path(__file__).read_bytes()).hexdigest()[:16]
