"""The port's own CTR TV logo (scripts/assets/ctr_tv_logo.png) in place of the game's.

The CTR TV logo is one texture, textures/ui/Octane/GPrix/octane_grandprix_crashtv_logo.tga in
update.pak: a 512x360 BC3 image (its rows padded to 384, stored bottom row first and tiled in 8x8
groups of 4x4 blocks). The new art is encoded (scripts/bc3.py) the same way into a copy of the
game's own file, so the menus use it with no other change.

bbport.ini ctr_tv_logo = 0 (or BB_CTR_TV_LOGO=0) keeps the game's. An archive patch
(scripts/archive_patch.py) keeping the base patch of the same archive, kept between launches while
the archives, the art and that base are the same.
SPDX-License-Identifier: GPL-2.0-or-later
"""
import hashlib
import json
import struct
from pathlib import Path

import archive_patch
from archive_reader import Archive
from button_prompts import IMG_DATA, IMG_W, _morton, _objects, read_png
from bc3 import bc3_encode
from mod_file_paths import resolve

ENTRY = 'octane_grandprix_crashtv_logo`tga,101.igz'
ARCHIVES = ('update2.pak', 'update.pak')  # newest first
ART = Path(__file__).resolve().parent / 'assets' / 'ctr_tv_logo.png'
OFF = ('0', 'off', 'no', 'false', 'original')
FORMAT = 1


def say(text):
    print(f'CTR TV logo: {text}', flush=True)


def wanted(config, env):
    value = env.get('BB_CTR_TV_LOGO', '')
    if not value and config and Path(config).is_file():
        for line in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
            key, sep, setting = line.partition('=')
            if sep and key.strip() == 'ctr_tv_logo':
                value = setting.strip()
    return value.strip().lower() not in OFF


def with_art(template, png):
    """The game's logo file with png's pixels (the same size) in its BC3 image."""
    objects, _, file_offset = _objects(template)
    images = [at for at, kind in objects if kind == 'igImage2']
    if len(images) != 1:
        raise ValueError('the logo file does not hold one image')
    width, height = struct.unpack_from('<HH', template, images[0] + IMG_W)
    packed, address = struct.unpack_from('<QQ', template, images[0] + IMG_DATA)
    length = packed & 0xffffffff
    across, rows = width // 4, (height + 3) // 4
    padded = (rows + 7) // 8 * 8
    if width % 32 or height % 4 or length != across * padded * 16:
        raise ValueError(f'unexpected logo image ({width}x{height}, {length} bytes)')
    art_width, art_height, rgba = read_png(png)
    if (art_width, art_height) != (width, height):
        raise ValueError(f'{png.name} must be {width}x{height}')
    linear = bc3_encode(width, height, rgba, flip=True)
    start = file_offset(address)
    tiled = bytearray(template[start:start + length])  # the padding rows keep the game's bytes
    pitch = across // 8
    for y in range(rows):
        for x in range(across):
            source = (y * across + x) * 16
            target = (((y >> 3) * pitch + (x >> 3)) * 64 + _morton(x & 7, y & 7)) * 16
            tiled[target:target + 16] = linear[source:source + 16]
    return template[:start] + bytes(tiled) + template[start + length:]


def prepare(game, out, config, env, base):
    """{guest archive: patch file}; none when the game's logo is wanted."""
    if not wanted(config, env) or not ART.is_file():
        return {}
    folder = Path(out) / 'ctr_tv_logo'
    archives = [resolve(Path(game) / 'archives' / name) for name in ARCHIVES]
    archives = [a for a in archives if a.is_file()]
    stamp = {'format': FORMAT, 'art': hashlib.sha256(ART.read_bytes()).hexdigest(),
             'script': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
             'archives': {a.name: [a.stat().st_size, a.stat().st_mtime_ns] for a in archives},
             'base': {g: hashlib.sha256(Path(p).read_bytes()).hexdigest() for g, p in sorted(base.items())}}
    stamp_file = folder / 'stamp.json'
    try:
        made = json.loads(stamp_file.read_text(encoding='utf-8'))
        if made['stamp'] == stamp and all(Path(p).is_file() for p in made['patches'].values()):
            return {g: Path(p) for g, p in made['patches'].items()}
    except (OSError, ValueError, KeyError):
        pass
    for path in archives:  # the newest copy is the one the game reads
        archive = Archive(path)
        entry = next((f for f in archive.files
                      if f['path'].replace('\\', '/').lower().endswith(ENTRY)), None)
        if entry is None:
            continue
        new = with_art(bytes(archive.extract(entry)), ART)
        folder.mkdir(parents=True, exist_ok=True)
        guest = f'/app0/archives/{path.name}'
        target = folder / (path.stem + '.bbpatch')
        archive_patch.write_entries_patch(target, path, guest, {entry['index']: new}, base.get(guest))
        stamp_file.write_text(json.dumps({'stamp': stamp, 'patches': {guest: str(target)}}), encoding='utf-8')
        say('the port\'s own')
        return {guest: target}
    say("the logo is not in the archives; the game's own")
    return {}
