"""Find the d3d11 port's engine hook points (EU/UP 1.00) in another eboot build.

usage: python hookmap.py <old.elf> <new.elf>
"""
import struct
import sys

import capstone
from capstone import x86

PT_LOAD = 1


class Image:
    def __init__(self, path):
        data = open(path, "rb").read()
        phoff, = struct.unpack_from("<Q", data, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
        size = 0
        segs = []
        for i in range(phnum):
            p_type, flags, off, vaddr, _, filesz, memsz, _ = struct.unpack_from(
                "<IIQQQQQQ", data, phoff + i * phentsize)
            if p_type == PT_LOAD or p_type == 0x61000010:  # LOAD, SCE_RELRO
                segs.append((vaddr, off, filesz, memsz, flags))
                size = max(size, vaddr + memsz)
        self.mem = bytearray(size)
        self.text = None
        for vaddr, off, filesz, memsz, flags in segs:
            self.mem[vaddr:vaddr + filesz] = data[off:off + filesz]
            if flags & 1 and self.text is None:
                self.text = (vaddr, vaddr + filesz)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True

    def insns(self, addr, count):
        out = []
        for ins in self.md.disasm(bytes(self.mem[addr:addr + count * 15]), addr):
            out.append(ins)
            if len(out) >= count:
                break
        return out

    def rip_target(self, ins):
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
                return ins.address + ins.size + op.mem.disp
        if ins.group(capstone.CS_GRP_CALL) or ins.group(capstone.CS_GRP_JUMP):
            for op in ins.operands:
                if op.type == x86.X86_OP_IMM:
                    return op.imm
        return None


def pattern(img, addr, n):
    """Bytes of the first n instructions; rel32 displacements of RIP-relative operands and
    calls/jumps outside the function are wildcards (None)."""
    pat = []
    for ins in img.insns(addr, n):
        b = list(ins.bytes)
        disp_off = ins.disp_offset if ins.disp_size == 4 else 0
        imm_off = ins.imm_offset if ins.imm_size == 4 else 0
        wild = None
        if any(op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP for op in ins.operands):
            wild = (disp_off, 4)
        elif (ins.group(capstone.CS_GRP_CALL) or ins.group(capstone.CS_GRP_JUMP)) and imm_off:
            tgt = img.rip_target(ins)
            if tgt is None or not (addr <= tgt < addr + 0x2000):
                wild = (imm_off, 4)
        for i, v in enumerate(b):
            pat.append(None if wild and wild[0] <= i < wild[0] + wild[1] else v)
    return pat


def search(img, pat, lo, hi):
    # anchor on the longest run of fixed bytes, then check the rest
    best, cur, start = (0, 0), 0, 0
    for i, v in enumerate(pat + [None]):
        if v is None:
            if cur > best[1] - best[0]:
                best = (start, i)
            cur = 0
            start = i + 1
        else:
            cur += 1
    a, b = best
    anchor = bytes(pat[a:b])
    hits = []
    pos = img.mem.find(anchor, lo, hi)
    while pos != -1:
        base = pos - a
        if all(v is None or img.mem[base + i] == v for i, v in enumerate(pat)):
            hits.append(base)
        pos = img.mem.find(anchor, pos + 1, hi)
    return hits


def dis(img, addr, n):
    for ins in img.insns(addr, n):
        print(f"    {ins.address:#09x}  {ins.mnemonic} {ins.op_str}")


def refs_to(img, target, lo, hi, step_fn_starts=None):
    """Code locations referencing `target` via RIP-relative disp32 (brute byte scan)."""
    import numpy as np
    out = []
    mem = img.mem
    buf = np.frombuffer(bytes(mem[lo:hi]), dtype=np.uint8)
    n = len(buf) - 4
    disp = (buf[:n].astype(np.int64) | buf[1:n + 1].astype(np.int64) << 8 |
            buf[2:n + 2].astype(np.int64) << 16 | buf[3:n + 3].astype(np.int64) << 24)
    disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
    pos = np.arange(lo, lo + n, dtype=np.int64)
    # instruction end is p+4 (forms with a trailing immediate are missed; fine for loads)
    for p in map(int, np.nonzero(pos + 4 + disp == target)[0] + lo):
        if True:
            for back in range(2, 5):
                for ins in img.md.disasm(bytes(mem[p - back:p - back + 15]), p - back):
                    if img.rip_target(ins) == target:
                        out.append((ins.address, f"{ins.mnemonic} {ins.op_str}"))
                    break
    return sorted(set(out))


def func_start(img, addr):
    """Nearest preceding `push rbp; mov rbp, rsp` (the build keeps frame pointers)."""
    p = addr
    while p > addr - 0x4000:
        if img.mem[p:p + 4] == b"\x55\x48\x89\xe5":
            return p
        p -= 1
    return None


def main():
    old, new = Image(sys.argv[1]), Image(sys.argv[2])
    hooks = {"kClearFunction": 0x1306b90, "kCopyTextureFunction": 0x1307f10}
    for name, addr in hooks.items():
        print(f"== {name} 1.00 {addr:#x}")
        dis(old, addr, 8)
        for n in (60, 40, 25):
            pat = pattern(old, addr, n)
            hits = search(new, pat, *new.text)
            print(f"  {n} insns ({len(pat)} bytes): {[hex(h) for h in hits]}")
            if len(hits) == 1:
                dis(new, hits[0], 8)
                break

    g = 0x26e7920
    print(f"== kDeviceGlobal 1.00 {g:#x}: references")
    refs = refs_to(old, g, *old.text)
    for a, t in refs[:40]:
        print(f"  {a:#x}  {t}  (func {func_start(old, a) or 0:#x})")
    print(f"  total {len(refs)}")


if __name__ == "__main__":
    main()
