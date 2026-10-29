"""The compute shaders the game keeps in eboot.bin (no .pak holds them): each program is found from
its OrbShdr footer (type 4: compute) and the first instruction that points at it
(s_mov_b32 vcc_hi, imm: the info lies (imm + 1) * 8 bytes on), and written to
<out>/<hash>_cs.bin as code + footer, the same shape as the exported vs/ps programs.

usage: python extract_compute.py <eboot.bin> <out dir>
"""
import os
import struct
import sys

TOKEN = 0xBEEB03FF


def main(eboot, out):
    data = open(eboot, "rb").read()
    os.makedirs(out, exist_ok=True)
    seen = set()
    pos = data.find(b"OrbShdr")
    while pos >= 0:
        bits, = struct.unpack_from("<I", data, pos + 8)
        kind, length = (bits >> 2) & 0xF, bits >> 8
        shader_hash, = struct.unpack_from("<Q", data, pos + 16)
        if kind == 4 and shader_hash not in seen:
            # the program's first dword is TOKEN, its second the distance to this footer
            for start in range(max(0, pos - length - 0x400), pos, 4):
                w0, w1 = struct.unpack_from("<II", data, start)
                if w0 == TOKEN and start + (w1 + 1) * 8 == pos:
                    end = pos + 28  # footer: signature..crc32
                    name = f"{shader_hash:016x}_cs"
                    open(os.path.join(out, name + ".bin"), "wb").write(data[start:end])
                    print(f"{name}: eboot.bin+{start:#x}, {length} bytes of code")
                    seen.add(shader_hash)
                    break
        pos = data.find(b"OrbShdr", pos + 1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
