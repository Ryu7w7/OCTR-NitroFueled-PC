"""The game's GPU shaders (GCN code) from the player's own dump, for the shader prebuild.

The port ships a shader recipe (the shaders and pipelines of long play sessions, without game code)
and translates it for the player's GPU at the first start (gpu/shadps4/video_core/renderer_vulkan/
vk_pipeline_serialization.cpp, PrebuildFromRecipe). The code itself comes from here: the shaders/
entries of the game's .pak archives, where every PS4 shader carries its binary info ("OrbShdr",
with the hash the port knows it by, and its type: a geometry shader and its copy shader, type 6,
share a hash). Written to <out>/gcn_shaders.bin, again only when the archives change:

    "BBGCN002", u32 count, u32 0, count x {u64 hash, u32 offset, u32 bytes, u32 type, u32 0},
    then the code.

Archives: IGA v13 (entry: u32 offset, u32 high offset byte, u32 length, u32 mode; mode 0xFFFFFFFF
stored, otherwise LZMA in 0x8000-byte chunks, each [u32 size][5-byte props][data], 0x800 aligned).
"""
import argparse
import json
import lzma
import os
import struct
import sys
from pathlib import Path

FORMAT = 2
SIG = b'OrbShdr'
TOKEN = 0xBEEB03FF  # s_mov_b32 vcc_hi, imm: the first instruction, the info (imm + 1) * 8 bytes on
ENDPGM = struct.pack('<I', 0xBF810000)


def pak_entries(path):
    """[(name, offset, length, mode)] of an IGA v13 archive."""
    from mod_file_paths import resolve
    with open(resolve(path), 'rb') as f:
        h = f.read(0x38)
        if len(h) < 0x38:
            return []
        n, = struct.unpack_from('<I', h, 0xc)
        nameoff, namesz = struct.unpack_from('<QI', h, 0x28)
        f.seek(0x38 + 4 * n)
        raw = f.read(16 * n)
        f.seek(nameoff)
        blob = f.read(namesz)
    entries = []
    offs = struct.unpack_from(f'<{n}I', blob, 0)
    for i, o in enumerate(offs):
        off, extra, length, mode = struct.unpack_from('<IIII', raw, 16 * i)
        name = blob[o:blob.index(b'\0', o)].decode('latin1')
        entries.append((name, off | (extra & 0xff) << 32, length, mode))
    return entries


def lzma_chunk(props, data, out_len):
    d = props[1] | props[2] << 8 | props[3] << 16 | props[4] << 24
    lc, rem = props[0] % 9, props[0] // 9
    lp, pb = rem % 5, rem // 5
    dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': max(d, 4096),
                                                            'lc': lc, 'lp': lp, 'pb': pb}])
    return dec.decompress(data, max_length=out_len)


def read_entry(f, off, length, mode):
    f.seek(off)
    if mode == 0xFFFFFFFF:
        return f.read(length)
    out = bytearray()
    pos = off
    while len(out) < length:
        f.seek(pos)
        csize, = struct.unpack('<I', f.read(4))
        want = min(0x8000, length - len(out))
        if csize == 0 or csize > 0x8000 + 0x100:  # a stored chunk
            f.seek(pos)
            out += f.read(want)
            pos += want
        else:
            props = f.read(5)
            out += lzma_chunk(props, f.read(csize), want)
            pos += 4 + 5 + csize
        pos = (pos + 0x7ff) & ~0x7ff
    return bytes(out[:length])


def shaders_in(data):
    """{(hash, type): code} of the shaders in an archive entry. The code is the binary info's length in
    bytes up to its last s_endpgm; when the code starts with the vcc_hi token, that start must
    agree (it does for all of CTR's)."""
    found = {}
    at = data.find(SIG)
    while at >= 0:
        bits, = struct.unpack_from('<I', data, at + 8)
        length = bits >> 8
        shader_hash, = struct.unpack_from('<Q', data, at + 16)
        shader_type = (bits >> 2) & 0xf
        end = data.rfind(ENDPGM, max(0, at - length - 0x100), at)
        start = end + 4 - length if end >= 0 else -1
        token_start = -1
        for back in range(8, min(at, 0x400) + 1, 4):
            s = at - back
            w0, w1 = struct.unpack_from('<II', data, s)
            if w0 == TOKEN and s + (w1 + 1) * 8 == at:
                token_start = s
                break
        if token_start >= 0 and start != token_start:
            start = -1
        if start >= 0 and length % 4 == 0 and length:
            found.setdefault((shader_hash, shader_type), data[start:start + length])
        at = data.find(SIG, at + 1)
    return found


def archives(game):
    folder = Path(game) / 'archives'
    from mod_file_paths import archives as redirected_archives
    return redirected_archives(folder)


def stamp(paths):
    return {'format': FORMAT, 'archives': [[p.name, p.stat().st_size, p.stat().st_mtime_ns] for p in paths]}


def build(game, out):
    out = Path(out)
    target = out / 'gcn_shaders.bin'
    stamp_file = out / 'gcn_shaders.json'
    paks = archives(game)
    if not paks:
        print('Game shaders: no archives folder in the game; the shader recipe is skipped')
        return 0
    current = stamp(paks)
    try:
        if target.is_file() and json.loads(stamp_file.read_text()) == current:
            return 0
    except (OSError, ValueError):
        pass
    shaders = {}
    files = 0
    for pak in paks:
        try:
            entries = [e for e in pak_entries(pak) if '/shaders/' in e[0]]
        except (OSError, struct.error, ValueError, UnicodeDecodeError) as error:
            print(f'Game shaders: {pak.name} skipped ({error})')
            continue
        if not entries:
            continue
        with open(pak, 'rb') as f:
            for _name, off, length, mode in entries:
                try:
                    data = read_entry(f, off, length, mode)
                except (OSError, struct.error, lzma.LZMAError) as error:
                    print(f'Game shaders: an entry of {pak.name} skipped ({error})')
                    continue
                files += 1
                for key, code in shaders_in(data).items():
                    shaders.setdefault(key, code)
    out.mkdir(parents=True, exist_ok=True)
    table = bytearray()
    body = bytearray()
    base = 16 + 24 * len(shaders)
    for key in sorted(shaders):
        code = shaders[key]
        table += struct.pack('<QIIII', key[0], base + len(body), len(code), key[1], 0)
        body += code
    temp = target.with_suffix('.tmp')
    temp.write_bytes(b'BBGCN002' + struct.pack('<II', len(shaders), 0) + table + body)
    os.replace(temp, target)
    stamp_file.write_text(json.dumps(current))
    print(f'Game shaders: {len(shaders)} from {files} shader files of the game, for the shader recipe')
    return 0


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game', help='the game folder (with archives/)')
    p.add_argument('--out', required=True, help='the folder for gcn_shaders.bin')
    a = p.parse_args()
    try:
        sys.exit(build(a.game, a.out))
    except Exception as error:  # the game runs without the recipe rather than not at all
        print(f'Game shaders: not read ({error!r}); the shader recipe is skipped this time')
        sys.exit(0)
