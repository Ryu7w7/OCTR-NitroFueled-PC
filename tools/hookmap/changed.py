"""Pairs the functions of the graphics device/context code (1.00 0x12f0000-0x1320000) with their
1.21 versions and lists the struct offsets that only one build uses.

usage: python changed.py <old.elf> <new.elf>
"""
import sys
from collections import Counter
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from hookmap import Image, pattern, search
from capstone import x86
old, new = Image(sys.argv[1]), Image(sys.argv[2])
lo, hi, delta = 0x12f0000, 0x1320000, 0xc32c0
def fstarts(img, a, b):
    out = []; p = img.mem.find(b"\x55\x48\x89\xe5", a, b)
    while p != -1: out.append(p); p = img.mem.find(b"\x55\x48\x89\xe5", p + 1, b)
    return out
os_ = fstarts(old, lo, hi)
match = {}
for i, f in enumerate(os_):
    end = os_[i + 1] if i + 1 < len(os_) else hi
    pat = pattern(old, f, len(list(old.md.disasm(bytes(old.mem[f:end]), f))))
    h = search(new, pat, f + delta - 0x20000, f + delta + 0x20000)
    match[f] = h[0] if len(h) == 1 else None
ns = fstarts(new, lo + delta - 0x20000, hi + delta + 0x20000)
def size(img, starts, f):
    i = starts.index(f); return starts[i + 1] - f
def disps(img, f, n):
    c = Counter()
    for ins in img.md.disasm(bytes(img.mem[f:f + n]), f):
        for op in ins.operands:
            if op.type == x86.X86_OP_MEM and op.mem.base not in (x86.X86_REG_RIP, x86.X86_REG_RSP, x86.X86_REG_RBP) and op.mem.disp >= 0:
                c[op.mem.disp] += 1
    return c
for i, f in enumerate(os_):
    if match[f] is not None: continue
    # neighbours: previous and next uniquely matched
    prev = next((os_[j] for j in range(i - 1, -1, -1) if match[os_[j]]), None)
    nxt = next((os_[j] for j in range(i + 1, len(os_)) if match[os_[j]]), None)
    if prev is None or nxt is None: continue
    # functions in new between match[prev] and match[nxt], same rank
    old_between = [g for g in os_ if prev < g < nxt]
    new_between = [g for g in ns if match[prev] < g < match[nxt]]
    if len(old_between) != len(new_between):
        print(f"{f:#x}: {len(old_between)} old vs {len(new_between)} new fns between anchors"); continue
    nf = new_between[old_between.index(f)]
    so, sn = size(old, os_, f), size(new, ns, nf)
    a, b = disps(old, f, so), disps(new, nf, sn)
    only_old = sorted(set(a) - set(b)); only_new = sorted(set(b) - set(a))
    print(f"{f:#x} ({so:#x}) -> {nf:#x} ({sn:#x}); offsets only in 1.00: {[hex(x) for x in only_old][:20]}; only in 1.21: {[hex(x) for x in only_new][:20]}")
