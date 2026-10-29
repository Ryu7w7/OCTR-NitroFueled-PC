"""Alchemy IGZ (version 10, PS4) reader: sections, fixups, objects decoded with
the reflection data from alchemy_classes.json.

usage: python igz.py <file.igz> [alchemy_classes.json]   -> JSON dump on stdout

Layout:
  header: u32 magic 0x49475A01, u32 version (10), u32 typeHash, u32 platform,
          u32 numFixups, then section descriptors {u32 unk, u32 offset, u32 length,
          u32 alignment} (section 0 = fixups, 1.. = object data)
  fixup:  {char[4] magic, u32 count, u32 length, u32 dataStart} + data
  offsets and pointers inside the data are encoded as (data section << 27) | offset.
  packed offset lists (RVTB, ROFS, RSTT, RPID, ...): nibbles, low nibble first;
  3 data bits + continuation bit, little-endian groups; each value v gives
  offset = previous + v*4.
"""
import json
import os
import struct
import sys

MAGIC = 0x49475A01
STRING_LISTS = ("TSTR", "TMET", "EXNM", "TDEP")
PACKED = ("RVTB", "ROFS", "RSTT", "RPID", "RHND", "RNEX", "REXT", "ROOT", "ONAM", "RMHN")


def unpack_offsets(data, count):
    out, prev, i = [], 0, 0
    nibbles = []
    for b in data:
        nibbles.append(b & 0xF)
        nibbles.append(b >> 4)
    for _ in range(count):
        v, shift = 0, 0
        while True:
            n = nibbles[i]
            i += 1
            v |= (n & 7) << shift
            shift += 3
            if not n & 8:
                break
        prev = prev + v * 4 if out or prev else v * 4
        out.append(prev)
    return out


def read_cstrings(data, count):
    out, p = [], 0
    for _ in range(count):
        e = data.index(b"\0", p)
        out.append(data[p:e].decode("utf-8", "replace"))
        p = e + 1
        if p & 1:  # strings are 2-byte aligned
            p += 1
    return out


class Igz:
    def __init__(self, blob):
        self.b = blob
        magic, self.version, self.type_hash, self.platform, nfix = struct.unpack_from("<5I", blob, 0)
        if magic != MAGIC:
            raise ValueError("not an IGZ file")
        self.sections = []
        o = 0x14
        while o + 16 <= 0x800:
            _u, off, length, align = struct.unpack_from("<4I", blob, o)
            if off == 0 and length == 0:
                break
            self.sections.append((off, length, align))
            o += 16
        self.fixups = {}
        fo, fend = self.sections[0][0], self.sections[0][0] + self.sections[0][1]
        p = fo
        for _ in range(nfix):
            if p >= fend:
                break
            tag, count, length, start = struct.unpack_from("<4sIII", blob, p)
            tag = tag.decode("ascii", "replace")
            data = blob[p + start:p + length]
            if tag in STRING_LISTS:
                val = read_cstrings(data, count)
            elif tag in PACKED:
                val = unpack_offsets(data, count)
            elif tag == "MTSZ":
                val = list(struct.unpack_from(f"<{count}I", data))
            else:
                val = data
            self.fixups[tag] = val
            p += length
        self.types = self.fixups.get("TMET", [])
        self.strings = self.fixups.get("TSTR", [])
        self.ptr_sites = set(self.fixups.get("ROFS", []))
        self.str_sites = set(self.fixups.get("RSTT", []))

    def file_offset(self, enc):
        """Encoded offset (data-section index << 27 | offset) -> file offset."""
        return self.sections[1 + (enc >> 27)][0] + (enc & 0x7FFFFFF)

    def u64(self, enc):
        return struct.unpack_from("<Q", self.b, self.file_offset(enc))[0]

    def objects(self):
        for off in self.fixups.get("RVTB", []):
            idx = self.u64(off)
            yield off, self.types[idx] if idx < len(self.types) else f"#{idx}"


FMT = {"igBoolMetaField": "<?", "igCharMetaField": "<b", "igUnsignedCharMetaField": "<B",
       "igShortMetaField": "<h", "igUnsignedShortMetaField": "<H", "igIntMetaField": "<i",
       "igUnsignedIntMetaField": "<I", "igLongMetaField": "<q", "igUnsignedLongMetaField": "<Q",
       "igSizeTypeMetaField": "<Q", "igFloatMetaField": "<f", "igDoubleMetaField": "<d",
       "igTimeMetaField": "<f", "igEnumMetaField": "<i", "igBitFieldMetaField": "<I",
       "igVec2fMetaField": "<2f", "igVec3fMetaField": "<3f", "igVec4fMetaField": "<4f",
       "igQuaternionfMetaField": "<4f", "igMatrix44fMetaField": "<16f"}


def decode(igz, classes):
    by = {c["name"]: c for c in classes}

    def all_fields(name):
        out = []
        while name in by:
            c = by[name]
            out = [f for f in c["fields"] if not (f["type"] or "").startswith("igStatic")] + out
            name = c["parent"]
        return out

    objs = []
    starts = {off: t for off, t in igz.objects()}
    for off, tname in igz.objects():
        rec = {"offset": off, "type": tname, "fields": {}}
        for f in all_fields(tname):
            at = off + f["offset"]
            if at in igz.str_sites:
                i = igz.u64(at)
                rec["fields"][f["name"]] = igz.strings[i] if i < len(igz.strings) else i
            elif at in igz.ptr_sites:
                p = igz.u64(at)
                rec["fields"][f["name"]] = {"ref": p, "type": starts.get(p)}
            elif f["type"] in FMT:
                v = struct.unpack_from(FMT[f["type"]], igz.b, igz.file_offset(at))
                rec["fields"][f["name"]] = v[0] if len(v) == 1 else list(v)
        objs.append(rec)
    return objs


def main():
    blob = open(sys.argv[1], "rb").read()
    igz = Igz(blob)
    res = {"version": igz.version, "platform": igz.platform, "sections": igz.sections,
           "fixups": {k: (v if not isinstance(v, bytes) else v.hex()) for k, v in igz.fixups.items()}}
    cpath = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "out", "alchemy_classes.json")
    if os.path.exists(cpath):
        res["objects"] = decode(igz, json.load(open(cpath)))
    json.dump(res, sys.stdout, indent=1)


if __name__ == "__main__":
    main()
