"""Export every shader from the extracted shaders_ps4.pak IGZ files:
<out>/<effect>_<hash>_<type>.bin (GCN code + OrbShdr footer) and index.json with the
Gnm shader struct decoded (stage registers, usage slots, input/export semantics).

usage: python export_shaders.py <dir with *.igz> <out dir>
"""
import glob
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from igz import Igz  # noqa: E402

# Alchemy shader stage ids; 3 is the export (ES) half of a VS->GS pipeline
TYPES = {0: "ps", 1: "vs", 2: "gs", 3: "es"}


def slots_at(b, o, n):
    out = []
    for k in range(n):
        u, api, reg, fl = b[o + 4 * k:o + 4 * k + 4]
        out.append({"usage": u, "api": api, "reg": reg, "flags": fl})
    return out, o + 4 * n


def parse_gnm(kind, b):
    common, = struct.unpack_from("<I", b, 0)
    nslots = common >> 24
    meta = {"shader_size": common & 0x7FFFFF, "uses_srt": (common >> 23) & 1}
    if kind == "vs":
        regs = struct.unpack_from("<7I", b, 8)
        meta["regs"] = dict(zip(["pgm_lo", "pgm_hi", "rsrc1", "rsrc2", "vs_out_config",
                                 "pos_format", "cl_vs_out_cntl"], regs))
        n_in, n_exp = b[36], b[37]
        meta["slots"], o = slots_at(b, 40, nslots)
        meta["inputs"] = [{"sem": b[o + 4 * k], "vgpr": b[o + 4 * k + 1], "size": b[o + 4 * k + 2]}
                          for k in range(n_in)]
        o += 4 * n_in
        meta["exports"] = [{"sem": b[o + 2 * k], "out": b[o + 2 * k + 1] & 0x1F}
                           for k in range(n_exp)]
    elif kind == "ps":
        regs = struct.unpack_from("<12I", b, 8)
        meta["regs"] = dict(zip(["pgm_lo", "pgm_hi", "rsrc1", "rsrc2", "z_format", "col_format",
                                 "input_ena", "input_addr", "in_control", "baryc_cntl",
                                 "db_shader_control", "cb_shader_mask"], regs))
        n_in = b[56]
        meta["slots"], o = slots_at(b, 60, nslots)
        ins = []
        for k in range(n_in):
            v, = struct.unpack_from("<H", b, o + 2 * k)
            ins.append({"sem": v & 0xFF, "default": (v >> 8) & 3, "flat": (v >> 10) & 1,
                        "linear": (v >> 11) & 1, "custom": (v >> 12) & 1})
        meta["inputs"] = ins
    elif kind == "es":
        # Gnm EsShader: stage regs, input count, ES->GS ring vertex size, slots, input semantics
        meta["regs"] = dict(zip(["pgm_lo", "pgm_hi", "rsrc1", "rsrc2"], struct.unpack_from("<4I", b, 8)))
        n_in = b[0x18]
        meta["esgs_dwords"], = struct.unpack_from("<I", b, 0x1c)
        meta["slots"], o = slots_at(b, 0x20, nslots)
        meta["inputs"] = [{"sem": b[o + 4 * k], "vgpr": b[o + 4 * k + 1], "size": b[o + 4 * k + 2]}
                          for k in range(n_in)]
    elif kind == "gs":
        # Gnm GsShader: stage regs, VGT state, slots, then the embedded copy VsShader
        meta["regs"] = dict(zip(["pgm_lo", "pgm_hi", "rsrc1", "rsrc2"], struct.unpack_from("<4I", b, 8)))
        meta["vgt"] = list(struct.unpack_from("<8I", b, 0x18))
        meta["slots"], o = slots_at(b, 0x38, nslots)
        meta["copy"] = parse_gnm("vs", b[o:])
        meta["raw"] = b.hex()
    else:
        meta["raw"] = b.hex()
    return meta


def main(src_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    index, seen = [], set()
    for p in sorted(glob.glob(os.path.join(src_dir, "**", "*.igz"), recursive=True)):
        z = Igz(open(p, "rb").read())
        effect = os.path.basename(p)[:-4]
        for off, t in z.objects():
            if t != "igShaderBuffer":
                continue
            stype, h = struct.unpack_from("<iI", z.b, z.file_offset(off + 0x10))
            hdr, = struct.unpack_from("<Q", z.b, z.file_offset(off + 0x18))
            szf, ptr = struct.unpack_from("<QQ", z.b, z.file_offset(off + 0x20))
            size = szf & 0xFFFFFFF
            code = z.b[z.file_offset(ptr):z.file_offset(ptr) + size]
            kind = TYPES.get(stype, f"t{stype}")
            shp, rop = struct.unpack_from("<QQ", z.b, z.file_offset(hdr + 0x20))
            gnm = z.b[z.file_offset(shp):z.file_offset(rop)]
            if kind == "gs":  # the embedded copy shader struct runs past the next pointer
                gnm = z.b[z.file_offset(shp):z.file_offset(shp) + 0x100]
            name = f"{effect}_{h:08x}_{kind}"
            entry = {"name": name, "effect": effect, "hash": h, "kind": kind, "size": size,
                     "gnm": parse_gnm(kind, gnm)}
            index.append(entry)
            if name not in seen:
                seen.add(name)
                open(os.path.join(out_dir, name + ".bin"), "wb").write(code)
    json.dump(index, open(os.path.join(out_dir, "index.json"), "w"), indent=1)
    print(f"{len(index)} shaders ({len({e['hash'] for e in index})} unique) -> {out_dir}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
