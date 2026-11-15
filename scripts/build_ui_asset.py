#!/usr/bin/env python3
"""Compile creator-built CTR UI texture and material assets."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

import gcn_shaders as iga
from button_prompts import IMG_DATA, IMG_W, _morton, _objects, read_png
from graphics_tab import Archive, write_archive
from ranked_ui_assets import (HOF_BACKGROUND_TEXTURE, RANKED_BACKGROUND_TEXTURE, asset_replacements,
                              compiled_asset, menu_asset_replacements)
from ui_texture import ASSETS, _replace_texture_name


def lobby_background_texture(domain_pak, kind):
    """Compile a button pattern using Matchmaking's native BC3 texture layout."""
    if kind not in ('ranked', 'hof'):
        raise ValueError('unsupported lobby background')
    entry = next(e for e in iga.pak_entries(domain_pak)
                 if 'octane_ui_online_bg_pattern_matchmaking`tga,101.igz' in e[0].lower())
    with Path(domain_pak).open('rb') as f:
        template = iga.read_entry(f, *entry[1:])
    objects, _, file_offset = _objects(template)
    image_at = next(at for at, kind in objects if kind == 'igImage2')
    if struct.unpack_from('<HH', template, image_at + IMG_W) != (256, 256):
        raise ValueError('unsupported native Matchmaking texture size')
    packed, address = struct.unpack_from('<QQ', template, image_at + IMG_DATA)
    if packed & 0xffffffff != 65536:
        raise ValueError('unsupported native Matchmaking texture format')
    png = ASSETS / f'{kind}_background.png'
    if read_png(png)[:2] != (256, 256):
        raise ValueError(f'{kind} background must be 256x256')
    encoded = subprocess.run(
        ['magick', str(png), '-flip', '-define', 'dds:compression=dxt5', 'DDS:-'],
        capture_output=True, check=True).stdout
    if encoded[84:88] != b'DXT5' or len(encoded) < 128 + 65536:
        raise ValueError(f'{kind} background BC3 encoding failed')
    linear = encoded[128:128 + 65536]
    tiled = bytearray(65536)
    for y in range(64):
        for x in range(64):
            source = (y * 64 + x) * 16
            target = (((y >> 3) * 8 + (x >> 3)) * 64 + _morton(x & 7, y & 7)) * 16
            tiled[target:target + 16] = linear[source:source + 16]
    start = file_offset(address)
    return _replace_texture_name(
        template[:start] + tiled + template[start + 65536:],
        f'data/Octane/textures/ui/Octane/Background/octane_ui_online_bg_pattern_{kind}.tga')


def build_pak(dest, template_path):
    assets = {**asset_replacements(), **menu_asset_replacements()}
    domain_pak = Path(template_path).parent / 'juicedomain_onlinemenus.pak'
    assets[RANKED_BACKGROUND_TEXTURE] = lobby_background_texture(domain_pak, 'ranked')
    assets[HOF_BACKGROUND_TEXTURE] = lobby_background_texture(domain_pak, 'hof')
    template = Archive(template_path)
    try:
        write_archive(dest, [(path, path, 0, data) for path, data in assets.items()], template)
    finally:
        template.f.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('png', type=Path)
    parser.add_argument('--name', required=True)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    texture_path, texture, namespace, material_path, export, material = compiled_asset(
        args.png, args.name)
    args.out.mkdir(parents=True, exist_ok=True)
    texture_file = args.out / (args.name + '_texture.igz')
    material_file = args.out / (args.name + '_material.igz')
    texture_file.write_bytes(texture)
    material_file.write_bytes(material)
    print(json.dumps({'texture_file': str(texture_file), 'texture_path': texture_path,
                      'material_file': str(material_file), 'material_path': material_path,
                      'namespace': namespace, 'export': export}, indent=2))


if __name__ == '__main__':
    main()
