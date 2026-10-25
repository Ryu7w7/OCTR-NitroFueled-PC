"""Finds the 1.21 address of the device global (1.00 0x26e7920): functions that reference it in 1.00
are matched in 1.21 and vote for the address they reference.

usage: python global.py <old.elf> <new.elf>
"""
import sys
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from hookmap import Image, pattern, search, refs_to, func_start
old, new = Image(sys.argv[1]), Image(sys.argv[2])
g = 0x26e7920
refs = [a for a, t in refs_to(old, g, *old.text) if t.split()[1].startswith("r")]
votes = {}
tried = 0
for a in refs:
    f = func_start(old, a)
    if not f or a - f > 0x300:
        continue
    n = len(old.insns(f, 400))
    ins_list = old.insns(f, 400)
    k = next(i for i, x in enumerate(ins_list) if x.address >= a)
    pat = pattern(old, f, k + 4)
    hits = search(new, pat, *new.text)
    tried += 1
    if len(hits) == 1:
        nf = hits[0]
        ni = new.insns(nf, k + 1)[k]
        t = new.rip_target(ni)
        votes[t] = votes.get(t, 0) + 1
        print(f"1.00 {f:#x}+{a-f:#x} -> 1.21 {nf:#x}: {ni.mnemonic} {ni.op_str} => {t:#x}")
    if tried >= 25:
        break
print("votes", {hex(k): v for k, v in votes.items()})
