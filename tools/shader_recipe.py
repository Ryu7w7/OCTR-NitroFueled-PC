"""Builds the shader recipe the port ships (shaders/recipe.zip) from a shader cache.

A cache the port wrote while the game was played (user/cache/<title>) holds, per shader permutation,
a .meta (the shader's description and the draw state it was translated for) and its SPIR-V (.spv,
made for that GPU), and a .key per pipeline. The recipe keeps the .meta and .key entries: no game
code and nothing that depends on the GPU. At a player's first start the port translates them again
for their GPU from the game's own shaders (scripts/gcn_shaders.py; PrebuildFromRecipe in
gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp).

    python tools/shader_recipe.py <cache folder> --out shaders/recipe.zip
"""
import argparse
import re
import struct
import sys
import zipfile
from pathlib import Path

META = re.compile(r'0x[0-9a-f]{16}\.meta$')
KEY = re.compile(r'[gc]_0x[0-9a-f]{16}\.key$')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('cache', help='a shader cache folder (user/cache/CUSA13795)')
    p.add_argument('--out', required=True)
    a = p.parse_args()
    cache = Path(a.cache)
    metas = sorted(f for f in cache.iterdir() if META.match(f.name))
    keys = sorted(f for f in cache.iterdir() if KEY.match(f.name))
    if not metas:
        sys.exit(f'{cache}: no shader entries')
    versions = {}
    for f in metas:
        head = f.read_bytes()[:8]
        if len(head) == 8:
            v = struct.unpack('<II', head)
            versions[v] = versions.get(v, 0) + 1
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    temp = out.with_suffix('.tmp')
    with zipfile.ZipFile(temp, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in metas + keys:
            info = zipfile.ZipInfo(f.name, date_time=(2026, 1, 1, 0, 0, 0))  # same bytes for the same cache
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, f.read_bytes())
    temp.replace(out)
    print(f'{out}: {len(metas)} shaders, {len(keys)} pipelines, {out.stat().st_size // 1024} KB '
          f'(meta/binary versions: {versions})')


if __name__ == '__main__':
    main()
