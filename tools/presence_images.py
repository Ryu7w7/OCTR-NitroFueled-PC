#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Discord presence's track pictures: each track's picture of the online lobby's track vote (the game's
textures@!ui!Octane!RaceSelect!octane_ui_raceselect_bg_<name>, 1288x1080 BC3), cut to the middle square and
made 512x512 JPEG, named after the track's zone (t111_crash_cove.jpg), and index.json listing them. The
port's new tracks (content/tracks/<id>/) give their own background.png. Uploaded to the content bucket's
presence/ folder; scripts/track_names.py tells the launchers which tracks have one.

A tool for the port's maintainers (Pillow needed), not part of a launch.

usage: presence_images.py GAME --out DIR [--content DIR] [--size 512]
"""
import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'scripts'))
from archive_reader import Archive  # noqa: E402
import season_art  # noqa: E402

PICTURE = re.compile(r'raceselect!octane_ui_raceselect_bg_(?!pattern)([a-z0-9_]+)`', re.I)
ZONE_INFO = re.compile(r'maps/octane/([a-z]\d{3}_[a-z0-9_]+)/\1_zoneinfo', re.I)
# The pictures whose name is not the zone's without its number and underscores.
NAMES = {'t112_roos_tubes': 'roostube', 't151_ngin_labs': 'nginlab', 't154_oxide_station': 'oxydestation',
         't501_july_world_tour': 'twilighttour', 't502_back_in_time': 'prehistoricplayground',
         't503_september_spyro': 'spyrocircuit', 't504_october_spooky': 'ninasnigthmare',
         't505_nitro_festival': 'kongcircus', 't506_luminous': 'winter', 't507_dry_land': 'dryland',
         't508_toxic': 'toxic'}


def alpha_values(block):
    a0, a1 = block[0], block[1]
    bits = int.from_bytes(block[2:8], 'little')
    if a0 > a1:
        table = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else:
        table = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
    return [table[(bits >> (3 * i)) & 7] for i in range(16)]


def decode_bc3(blob):
    """(width, height, RGBA bytes, top row first) of a BC3 texture in the PS4's tiled layout."""
    width, height, off, size, _levels = season_art.image(blob)
    data = blob[off:off + size]
    across, _down = season_art.block_grid(width, height)
    pixels = bytearray(width * height * 4)
    for by in range((height + 3) // 4):
        for bx in range((width + 3) // 4):
            at = season_art.tiled_index(bx, by, across) * 16
            block = data[at:at + 16]
            alphas = alpha_values(block[:8])
            colours = season_art.decode_colours(block[8:16])
            for i in range(16):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                if x < width and y < height:
                    p = ((height - 1 - y) * width + x) * 4   # stored bottom row first
                    pixels[p:p + 4] = bytes(colours[i]) + bytes([alphas[i]])
    return width, height, bytes(pixels)


def square(image, size):
    from PIL import Image
    w, h = image.size
    side = min(w, h)
    left, top = (w - side) // 2, (h - side) // 2
    return image.crop((left, top, left + side, top + side)).resize((size, size), Image.LANCZOS)


def main():
    from PIL import Image
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--content', type=Path)
    p.add_argument('--size', type=int, default=512)
    a = p.parse_args()
    pictures, zones = {}, set()
    for pak in sorted((a.game / 'archives').glob('*.pak')):
        try:
            archive = Archive(pak)
        except (OSError, ValueError):
            continue
        for f in archive.files:
            m = PICTURE.search(f['path'])
            if m and m[1].lower() not in pictures:
                pictures[m[1].lower()] = (archive, f)
            m = ZONE_INFO.search(f['path'].replace('\\', '/'))
            if m:
                zones.add(m[1].lower())
    a.out.mkdir(parents=True, exist_ok=True)
    made = {}
    for zone in sorted(zones):
        name = NAMES.get(zone) or re.sub(r'^[a-z]\d{3}_', '', zone).replace('_', '')
        if name not in pictures:
            continue
        archive, f = pictures[name]
        w, h, rgba = decode_bc3(bytes(archive.extract(f)))
        square(Image.frombytes('RGBA', (w, h), rgba).convert('RGB'), a.size).save(
            a.out / f'{zone}.jpg', quality=88, optimize=True)
        made[zone] = name
    if a.content:
        for track in sorted((a.content / 'tracks').glob('*/track.json')):
            m = json.loads(track.read_text(encoding='utf-8'))
            picture = track.parent / m.get('background', '')
            zone = m['level'].rsplit('/', 1)[-1].lower()
            if m.get('background') and picture.is_file():
                square(Image.open(picture).convert('RGB'), a.size).save(a.out / f'{zone}.jpg', quality=88, optimize=True)
                made[zone] = str(picture.name)
    (a.out / 'index.json').write_text(json.dumps({'format': 1, 'zones': sorted(made)}, indent=1) + '\n',
                                      encoding='utf-8')
    for zone, name in sorted(made.items()):
        print(zone, '<-', name)
    print(len(made), 'pictures;', 'zones without one:', sorted(zones - set(made)))


if __name__ == '__main__':
    main()
