# SPDX-License-Identifier: GPL-2.0-or-later
"""IGZ objects copied between files: what the content registries (racers, tracks) are built with.

File reads an IGZ with every fixup a copy needs (handles, external names, dependencies) and writes
it back; Copier copies an object of one file into another with everything it references: other
objects, data blocks, strings, handles and their dependencies, renumbered for the target. Objects
the target already has under the same name are referenced, not copied.

Moved here from racer_registry.py (racer merge by tcosi and the OCTR team) so tracks can use it too.
"""
import bisect
import struct

from graphics_tab import fnv1a
from igz_file import Igz, unpack_offsets

MASK = 0x7FFFFFF
NAMED = 0x80000000
HANDLES = ('RHND', 'RNEX', 'REXT')


class File(Igz):
    """An IGZ with its handle fixups, EXNM and TDEP editable too."""

    def __init__(self, blob):
        super().__init__(blob)
        present = {f[0] for f in self.fixups}
        for tag in HANDLES:
            if tag in present:
                self.sites[tag] = self.read_list(tag, unpack_offsets)
        self.named = []
        if 'EXNM' in present:
            f = self.fixup('EXNM')
            self.named = [list(struct.unpack_from('<II', f[3], 8 * i)) for i in range(f[1])]
        self.depends = []
        if 'TDEP' in present:
            f = self.fixup('TDEP')
            raw, at = f[3], 0
            for _ in range(2 * f[1]):
                end = raw.index(b'\0', at)
                self.depends.append(raw[at:end].decode('utf-8'))
                at = end + 1
        self.starts = sorted(self.sites['RVTB'])
        self.typed = dict(self.objects())

    # --- reading ---------------------------------------------------------------------------
    def ref(self, off):
        return self.u64(off) & MASK

    def size(self, off):
        return self.sizes[self.u64(off)]

    def items(self, lst):
        """The references a list object holds."""
        data = self.ref(lst + 0x20)
        return [self.ref(data + 8 * i) for i in range(self.u32(lst + 0xC))]

    def drivers(self):
        """The game's racer list (CSystemData's available drivers; other driver lists, such as a
        cup's opponents, have other names)."""
        found = [o for n, o in self.roots()[2] if n == 'SystemData_availableDrivers']
        if len(found) != 1 or self.typed.get(found[0]) != 'CRacerDriverDataList':
            raise ValueError('no SystemData_availableDrivers racer list')
        return found[0]

    def find(self, kind):
        found = [o for o, t in self.typed.items() if t == kind]
        if len(found) != 1:
            raise ValueError(f'expected one {kind}, found {len(found)}')
        return found[0]

    def roots(self):
        """(object list, name list, [(name, object)]) of the file's ROOT and ONAM lists."""
        root = struct.unpack_from('<I', self.fixup('ROOT')[3])[0]
        names = struct.unpack_from('<I', self.fixup('ONAM')[3])[0]
        data = self.ref(names + 0x20)
        named = [self.strings[self.u64(data + 16 * i)] for i in range(self.u32(names + 0xC))]
        return root, names, list(zip(named, self.items(root)))

    def handle(self, value):
        """(name, namespace) a handle points to: a named handle counts EXNM entries with the
        namespace's high bit set, the others the rest."""
        kind = [e for e in self.named if (e[1] & NAMED) == (value & NAMED)]
        a, b = kind[value & 0x7FFFFFFF]
        return self.strings[a], self.strings[b & 0x7FFFFFFF]

    def sites_in(self, start, length):
        for tag in ('RSTT', 'ROFS', 'RPID') + HANDLES:
            sites = self.sites.get(tag, [])
            for i in range(bisect.bisect_left(sites, start), bisect.bisect_left(sites, start + length)):
                yield tag, sites[i]

    def block_at(self, ref, length):
        """True when [ref, ref + length) is a data block: inside the data, outside every object."""
        if not length or ref + length > len(self.data):
            return False
        i = bisect.bisect_right(self.starts, ref + length - 1) - 1
        return i < 0 or self.starts[i] + self.size(self.starts[i]) <= ref

    # --- writing ---------------------------------------------------------------------------
    def handle_of(self, name, space, named):
        """The handle of (name, namespace), added to EXNM when missing."""
        kind = [e for e in self.named if bool(e[1] & NAMED) == named]
        for i, (a, b) in enumerate(kind):
            if self.strings[a] == name and self.strings[b & 0x7FFFFFFF] == space:
                return i | (NAMED if named else 0)
        self.named.append([self.string_index(name), self.string_index(space) | (NAMED if named else 0)])
        return len(kind) | (NAMED if named else 0)

    def add_site(self, tag, off):
        sites = self.sites[tag]
        i = bisect.bisect_left(sites, off)
        if i == len(sites) or sites[i] != off:
            sites.insert(i, off)

    def grow(self, lst, values, size=8):
        """Entries added to a list object: its data moves to a new block at the end."""
        count, data = self.u32(lst + 0xC), self.ref(lst + 0x20)
        old = bytes(self.data[data:data + count * size])
        at = self.append(old)
        for tag, off in list(self.sites_in(data, count * size)):
            self.add_site(tag, at + off - data)
        for value in values:
            off = self.append(bytes(size), 1)
            if size == 8:  # a reference
                self.set_u64(off, value)
                self.add_site('ROFS', off)
            else:  # a name: string index and its hash
                self.set_u64(off, self.string_index(value))
                self.set_u32(off + 8, fnv1a(value))
                self.add_site('RSTT', off)
        n = count + len(values)
        self.set_u32(lst + 0xC, n)
        self.set_u32(lst + 0x10, n)
        self.set_u64(lst + 0x18, (self.u64(lst + 0x18) & ~0xFFFFFFFF) | n * size)
        self.set_u64(lst + 0x20, at)

    def _raw(self, tag, count, raw, trailing=False):
        f = self.fixup(tag)
        pad = -(f[2] + len(raw)) % 4
        if trailing and not pad:
            pad = 4  # a TDEP block ends with 1-4 zero bytes, as the game's files do
        f[1], f[3], f[4] = count, raw + bytes(pad), f[2] + len(raw) + pad

    def build(self):
        if self.named:
            self._raw('EXNM', len(self.named), b''.join(struct.pack('<II', *e) for e in self.named))
        if self.depends:
            self._raw('TDEP', len(self.depends) // 2,
                      b''.join(s.encode('utf-8') + b'\0' for s in self.depends), trailing=True)
        return super().build()


class Copier:
    """Objects of one IGZ copied into another, with their references, strings and handles.

    keep: {source offset: target offset} of objects referenced as they are instead of copied (a
    copy inside one file shares what it does not own). rename: {old: new} for the strings and
    handle names of the copy (a handle's dependency path follows its renamed namespace)."""

    def __init__(self, source, target, keep=None, rename=None):
        self.source, self.target, self.copies = source, target, dict(keep or {})
        self.rename = dict(rename or {})
        self.blocks = {}
        self.names = {}
        self.existing = {}
        if any(f[0] == 'ONAM' for f in source.fixups):
            self.names = {o: n for n, o in source.roots()[2]}
            self.existing = {n: o for n, o in target.roots()[2]}

    def text(self, s):
        return self.rename.get(s, s)

    def object(self, off):
        if off in self.copies:
            return self.copies[off]
        name = self.names.get(off)
        if name in self.existing:  # a shared object the target already has
            self.copies[off] = self.existing[name]
            return self.existing[name]
        s, t = self.source, self.target
        kind = s.types[s.u64(off)]
        if kind not in t.types:
            raise ValueError(f'type {kind} is not in the merged file')
        size = s.size(off)
        new = t.append(bytes(s.data[off:off + size]))
        t.set_u64(new, (s.u64(off) & ~0xFFFFFFFF) | t.types.index(kind))
        t.add_site('RVTB', new)
        self.copies[off] = new
        self.fix(off, new, size, kind)
        return new

    def block(self, ref, length):
        """A data block (a list's items, a command stream's bytes) copied once, fixed up."""
        if ref not in self.blocks:
            s, t = self.source, self.target
            self.blocks[ref] = t.append(bytes(s.data[ref:ref + length]))
            self.fix(ref, self.blocks[ref], length, '')
        return self.blocks[ref]

    def fix(self, off, new, size, kind):
        s, t = self.source, self.target
        for tag, site in list(s.sites_in(off, size)):
            at = new + site - off
            if tag == 'RSTT':
                t.set_u64(at, t.string_index(self.text(s.strings[s.u64(site)])))
            elif tag in ('ROFS', 'RPID'):
                value = s.u64(site)
                ref = value & MASK
                if ref == 0:
                    pass  # a null field (an empty list's data): offset 0 is the ROOT list, not copied
                elif ref in s.typed:
                    ref = self.object(ref)
                elif kind and site - off >= 8 and s.block_at(ref, s.u64(site - 8) & 0xFFFFFFFF):
                    # An object's memory field: {u64 size and flags, u64 data}; lists keep theirs
                    # at +0x18/+0x20, command streams and materials elsewhere.
                    ref = self.block(ref, s.u64(site - 8) & 0xFFFFFFFF)
                else:
                    raise ValueError(f'unexpected reference at {site:#x} in a {kind or "data block"}')
                t.set_u64(at, (value & ~MASK) | ref)
            else:
                value = s.u32(site)
                name, space = s.handle(value)
                name, new_space = self.text(name), self.text(space)
                t.set_u32(at, t.handle_of(name, new_space, bool(value & NAMED)))
                spaces = s.depends[0::2]
                if value & NAMED and space in spaces and new_space not in t.depends[0::2]:
                    path = s.depends[2 * spaces.index(space) + 1]
                    t.depends += [new_space, path.replace(space, new_space)]
            t.add_site(tag, at)
