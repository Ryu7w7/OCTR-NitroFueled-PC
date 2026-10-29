"""Alchemy IGA archive (.pak, version 13) lister / extractor.

usage: python iga.py list <file.pak>
       python iga.py extract <file.pak> <outdir> [substring-filter]

Layout (all little endian):
  header 0x38: magic 'IGA\\x1a', version, tocSize, numFiles, sectorSize,
               hashSearchDivider, hashSearchSlop, numLargeBlocks, numMediumBlocks,
               numSmallBlocks, u64 nameTableOffset, nameTableSize, flags
  toc:  u32 fileId[numFiles]
        FileInfo[numFiles] = {u32 offset, u32 ordinal, u32 length, u32 blockInfo}
        u32 large[numLarge], u16 medium[numMedium], u8 small[numSmall]
  blockInfo: top 4 bits = compression (0xF.. = stored), low 28 bits = index into
  the block table selected by the file's sector span. Block entries are sector
  offsets relative to the file offset; the top bit flags a compressed block.
  Blocks inflate to 0x8000 bytes. Compressed block: u32 size, then the payload.
"""
import lzma
import os
import struct
import sys
import zlib

BLOCK = 0x8000
HDR = struct.Struct("<4sIIIIIIIIIQII")

COMP_ZLIB, COMP_LZMA, COMP_LZ4 = 1, 2, 3


class Iga:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = HDR.unpack(self.f.read(HDR.size))
        (magic, self.version, self.toc_size, self.num_files, self.sector, _div, _slop,
         n_large, n_medium, n_small, nt_off, nt_size, self.flags) = h
        if magic != b"IGA\x1a" or self.version not in (11, 13):
            raise ValueError(f"unsupported archive {magic!r} v{self.version}")
        toc = self.f.read(self.toc_size)
        n = self.num_files
        self.ids = struct.unpack_from(f"<{n}I", toc, 0)
        o = 4 * n
        self.infos = [struct.unpack_from("<4I", toc, o + 16 * i) for i in range(n)]
        o += 16 * n
        self.large = struct.unpack_from(f"<{n_large}I", toc, o)
        o += 4 * n_large
        self.medium = struct.unpack_from(f"<{n_medium}H", toc, o)
        o += 2 * n_medium
        self.small = struct.unpack_from(f"<{n_small}B", toc, o)

        self.f.seek(nt_off)
        nt = self.f.read(nt_size)
        self.names = []
        for i in range(n):
            p, = struct.unpack_from("<I", nt, 4 * i)
            end = nt.index(b"\0", p)
            phys = nt[p:end].decode("utf-8", "replace")
            end2 = nt.index(b"\0", end + 1)
            logical = nt[end + 1:end2].decode("utf-8", "replace")
            self.names.append((logical or phys, phys))

    def compression(self, i):
        return self.infos[i][3] >> 28

    def read(self, i):
        offset, _ordinal, length, binfo = self.infos[i]
        if binfo == 0xFFFFFFFF or binfo >> 28 == 0xF:
            self.f.seek(offset)
            return self.f.read(length)
        comp, index = binfo >> 28, binfo & 0x0FFFFFFF
        nblocks = (length + BLOCK - 1) // BLOCK
        span = (length + self.sector - 1) // self.sector
        if span <= 0x7F:
            table, flag = self.small, 0x80
        elif span <= 0x7FFF:
            table, flag = self.medium, 0x8000
        else:
            table, flag = self.large, 0x80000000
        out = bytearray()
        for b in range(nblocks):
            entry, nxt = table[index + b], table[index + b + 1]
            start = offset + (entry & ~flag) * self.sector
            end = offset + (nxt & ~flag) * self.sector
            want = min(BLOCK, length - b * BLOCK)
            self.f.seek(start)
            if not entry & flag:
                out += self.f.read(want)
                continue
            raw = self.f.read(end - start)
            # v13: u32 compressed size; v11 (N. Sane Trilogy PC): u16
            hs = 4 if self.version >= 13 else 2
            csize, = struct.unpack_from("<I" if hs == 4 else "<H", raw, 0)
            out += decompress(comp, raw[hs:hs + csize + (5 if comp == COMP_LZMA else 0)], want)
        return bytes(out[:length])


def decompress(comp, data, want):
    if comp == COMP_LZMA:
        props = data[0]
        lc, rem = props % 9, props // 9
        lp, pb = rem % 5, rem // 5
        dict_size, = struct.unpack_from("<I", data, 1)
        filt = [{"id": lzma.FILTER_LZMA1, "lc": lc, "lp": lp, "pb": pb, "dict_size": dict_size}]
        d = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=filt)
        return d.decompress(data[5:], want)
    if comp == COMP_ZLIB:
        return zlib.decompressobj(-15).decompress(data, want)
    raise NotImplementedError(f"compression type {comp}")


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    a = Iga(path)
    if cmd == "list":
        for i, (logical, _phys) in enumerate(a.names):
            off, _o, length, binfo = a.infos[i]
            print(f"{a.ids[i]:08x} {length:10d} c{binfo >> 28:x} {logical}")
        return
    outdir = sys.argv[3]
    filt = sys.argv[4] if len(sys.argv) > 4 else None
    root = os.path.realpath(outdir)
    count = 0
    for i, (logical, _phys) in enumerate(a.names):
        if filt and filt not in logical:
            continue
        rel = logical.replace("\\", "/").lstrip("/").replace(":", "_")
        dst = os.path.realpath(os.path.join(outdir, rel))
        if not dst.startswith(root + os.sep):
            print("skip unsafe path", logical)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(a.read(i))
        count += 1
    print(f"extracted {count} files")


if __name__ == "__main__":
    main()
