"""Packs the translated shaders (out/hlsl: shaders.idx + *.dxbc) into one blob that is linked
into CrashTeamRacingNitroFueled.exe as an RCDATA resource (no shaders folder needed).

Layout: "CTRS" u32 version, u32 index_len, index text, u32 count,
        count x {u16 name_len, name, u32 size, data}

usage: python pack_shaders.py <out/hlsl> <output .bin>
"""
import glob
import os
import struct
import sys


def main(src, dst):
    index = open(os.path.join(src, "shaders.idx"), "rb").read()
    names = set()
    for line in index.decode().splitlines():
        if line.startswith("S "):
            names.add(line.split()[2])
    out = [b"CTRS", struct.pack("<II", 1, len(index)), index]
    blobs = []
    for n in sorted(names):
        p = os.path.join(src, n + ".dxbc")
        if os.path.exists(p):
            blobs.append((n.encode(), open(p, "rb").read()))
    out.append(struct.pack("<I", len(blobs)))
    for n, data in blobs:
        out += [struct.pack("<H", len(n)), n, struct.pack("<I", len(data)), data]
    data = b"".join(out)
    if not os.path.exists(dst) or open(dst, "rb").read() != data:
        open(dst, "wb").write(data)
    print(f"{len(blobs)} shaders, {len(data) >> 20} MiB -> {dst}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
