# SPDX-License-Identifier: GPL-2.0-or-later
"""One mod's change of an IGZ file applied over another mod's version of it (a three-way merge).

The built-in mods (scripts/builtin_mods) change a few named objects of a game file: a menu's lap
list, the battle limits in systemData, 25 materials. When a community mod ships its own version of
that file (a racer mod's systemData.igz, a menu), the built-in's change is applied over it here,
object by object, instead of one of the two versions hiding the other:

- stock: the game's file; ours: stock with the built-in's change; theirs: the other mod's version.
- A named object (one in the file's ROOT/ONAM lists) ours changed is copied over theirs, in place
  (same offset: whatever refers to it still does); a named object ours added is added.
- It cannot be merged (Unmergeable) when both changed the same object differently, when theirs
  removed an object ours changes, when ours changes something no named object reaches, or when
  the result does not check out: every object ours touched as ours has it, every other named
  object as theirs has it.

Objects are compared by content, not offset: an object's fingerprint is its type and bytes with
every fixed-up field replaced by what it means (a string's text, a handle's name, a named object's
name, an unnamed object's or data block's own fingerprint).
"""
import hashlib
import struct
import sys
from collections import Counter

from igz_merge import HANDLES, MASK, Copier, File


class Unmergeable(ValueError):
    pass


POINTERS = ('ROFS', 'RPID')


class Prints:
    """Fingerprints of the objects of one file."""

    def __init__(self, f):
        self.f = f
        self.names = {o: n for n, o in f.roots()[2]}
        self.offsets = {n: o for o, n in self.names.items()}
        self.memo = {}  # object -> fingerprint, for objects outside every cycle

    def object(self, off):
        return self.walk(off, {}, ())[0]

    def walk(self, off, local, seen):
        """(fingerprint, in a cycle) of an object. An object of a cycle (a child pointing back at
        its parent) has a fingerprint that depends on where the walk entered the cycle, so it is
        kept only for this walk (local): the same file walked from the same object gives the same
        fingerprints, whatever was walked before."""
        if off in self.memo:
            return self.memo[off], False
        if off in seen:
            return 'cycle:%d' % (len(seen) - seen.index(off)), True
        if off in local:
            return local[off], True
        value, cyclic = self.part(off, self.f.size(off), self.f.typed[off], local, seen + (off,))
        (local if cyclic else self.memo)[off] = value
        return value, cyclic

    def part(self, off, length, kind, local, seen):
        f = self.f
        cyclic = False
        raw = bytearray(f.data[off:off + length])
        if kind:
            raw[0:4] = bytes(4)  # the type index (the type's name is hashed instead)
        out = [kind.encode()]
        for tag, site in f.sites_in(off, length):
            at = site - off
            if tag == 'RSTT':
                raw[at:at + 8] = bytes(8)
                out.append(b'S%d:' % at + f.strings[f.u64(site)].encode())
            elif tag in POINTERS:
                value = f.u64(site)
                raw[at:at + 8] = struct.pack('<Q', value & ~MASK)
                ref = value & MASK
                if ref in f.typed:
                    if ref in self.names:
                        out.append(b'N%d:' % at + self.names[ref].encode())
                    else:
                        h, c = self.walk(ref, local, seen)
                        cyclic |= c
                        out.append(b'O%d:' % at + h.encode())
                elif at >= 8 and f.block_at(ref, f.u64(site - 8) & 0xFFFFFFFF):
                    h, c = self.part(ref, f.u64(site - 8) & 0xFFFFFFFF, '', local, seen)
                    cyclic |= c
                    out.append(b'B%d:' % at + h.encode())
                else:
                    out.append(b'R%d:%d' % (at, ref))  # unknown: compared as is
            else:  # a handle
                raw[at:at + 4] = bytes(4)
                try:
                    name, space = f.handle(f.u32(site))
                except IndexError:  # not an EXNM handle (compared as is)
                    out.append(b'h%d:%d' % (at, f.u32(site)))
                    continue
                out.append(b'H%d:%d:' % (at, f.u32(site) >> 31) + (space + '.' + name).encode())
        out.append(bytes(raw))
        return hashlib.sha256(b'\0'.join(out)).hexdigest(), cyclic

    def named(self, name):
        return self.object(self.offsets[name])

    def reach(self, off, found):
        """The unnamed objects `off` refers to, directly, through their data blocks (a list's
        items) or through other unnamed objects, and itself."""
        f = self.f
        found.add(off)
        stack = [(off, f.size(off))]
        blocks = set()
        while stack:
            at, length = stack.pop()
            for tag, site in f.sites_in(at, length):
                if tag not in POINTERS:
                    continue
                ref = f.u64(site) & MASK
                if ref in f.typed:
                    if ref not in self.names and ref not in found:
                        found.add(ref)
                        stack.append((ref, f.size(ref)))
                elif site - at >= 8 and ref not in blocks:
                    size = f.u64(site - 8) & 0xFFFFFFFF
                    if f.block_at(ref, size):
                        blocks.add(ref)
                        stack.append((ref, size))
        return found


def changes(stock, ours):
    """(changed names, added names) of ours against stock; Unmergeable when ours changes anything
    else."""
    S, O = Prints(stock), Prints(ours)
    removed = [n for n in S.offsets if n not in O.offsets]
    if removed:
        raise Unmergeable('it removes ' + ', '.join(removed[:3]))
    changed = [n for n in O.offsets if n in S.offsets and O.named(n) != S.named(n)]
    added = [n for n in O.offsets if n not in S.offsets]
    # Every other object of ours must be one stock has too: a change no named object reaches
    # (a script's unnamed node) cannot be carried over.
    reached = set()
    for n in changed + added:
        O.reach(O.offsets[n], reached)
    lists = set(ours.roots()[:2]) | set(stock.roots()[:2])  # the ROOT and ONAM lists grow
    left = Counter(O.object(o) for o in ours.typed if o not in reached and o not in lists)
    left.subtract(Counter(S.object(o) for o in stock.typed if o not in lists))
    if any(v > 0 for v in left.values()):
        raise Unmergeable('it changes objects no named object reaches')
    return changed, added


def replace(t, source, src, old):
    """Named object `src` of `source` copied over object `old` of `t`, at the same offset; returns
    the file rebuilt."""
    kind = source.typed[src]
    if t.typed.get(old) != kind or t.size(old) != source.size(src):
        raise Unmergeable(f'{kind} differs in layout')
    copier = Copier(source, t)
    copier.names.pop(src, None)  # copied, not taken for the object being replaced
    new = copier.object(src)
    size = t.size(old)
    tags = ('RSTT',) + POINTERS + HANDLES
    for tag in tags:
        sites = t.sites.get(tag)
        if sites is None:
            continue
        inside = [s for s in sites if old <= s < old + size]
        moved = [s for s in sites if new <= s < new + size]
        for s in inside + moved:
            sites.remove(s)
        for s in moved:
            t.add_site(tag, old + s - new)
    t.data[old:old + size] = t.data[new:new + size]
    t.sites['RVTB'].remove(new)
    for tag in POINTERS:  # what the copy's own objects refer back to
        for site in t.sites.get(tag, []):
            value = t.u64(site)
            if value & MASK == new:
                t.set_u64(site, (value & ~MASK) | old)
    return File(t.build())


def add(t, source, src, name):
    copier = Copier(source, t)
    new = copier.object(src)
    root, names, _ = t.roots()
    t.grow(root, [new])
    t.grow(names, [name], 16)
    return File(t.build())


def rebase(stock, ours, theirs):
    """theirs (bytes) with ours' change over stock; Unmergeable when that cannot be done safely."""
    limit = sys.getrecursionlimit()
    sys.setrecursionlimit(max(limit, 20000))
    try:
        S, O, T = File(stock), File(ours), File(theirs)
        changed, added = changes(S, O)
        if not changed and not added:
            if stock != ours:
                raise Unmergeable('its change is outside every named object')
            return bytes(theirs)
        PS, PO, PT = Prints(S), Prints(O), Prints(T)
        todo_changed, todo_added = [], []
        for n in changed:
            if n not in PT.offsets:
                raise Unmergeable(n + ' is not in the other version')
            mine, base, other = PO.named(n), PS.named(n), PT.named(n)
            if other == mine:
                continue
            if other != base:
                raise Unmergeable(n + ' is changed by both')
            todo_changed.append(n)
        for n in added:
            if n in PT.offsets:
                if PT.named(n) != PO.named(n):
                    raise Unmergeable(n + ' is added by both')
                continue
            todo_added.append(n)
        if not todo_changed and not todo_added:
            return bytes(theirs)
        result = T
        for n in todo_changed:
            result = replace(result, O, PO.offsets[n], Prints(result).offsets[n])
        for n in todo_added:
            result = add(result, O, PO.offsets[n], n)
        # The check: ours' objects as ours has them, every other named object as theirs has it.
        PR = Prints(result)
        for n in changed + added:
            if PR.offsets.get(n) is None or PR.named(n) != PO.named(n):
                raise Unmergeable(n + ' did not copy over as it is')
        for n in PT.offsets:
            if n not in changed and n not in added and PR.named(n) != PT.named(n):
                raise Unmergeable(n + ' changed while merging')
        if set(PR.offsets) != set(PT.offsets) | set(added):
            raise Unmergeable('the named objects changed while merging')
        return result.build()
    finally:
        sys.setrecursionlimit(limit)
