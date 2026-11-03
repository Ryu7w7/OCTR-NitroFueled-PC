"""Racers added by several mods, merged into one registration.

A mod that adds a racer ships an extra update archive (archives/update4.pak, update5.pak...) with
the game's whole racer registration: characterlisting.igz (the racer names), looseData/
systemData.igz (a CRacerDriverData per racer, with its skins and outfits) and the
looseData/systemData_<language>.lng texts. The game does not merge these: the last update archive
that loads wins, and every other mod's racer disappears (or comes back as the stale copy another
mod carried).

When two or more enabled mods ship a registration, the launcher builds one: the game's own
registration (the newest copy in archives/update.pak, update2.pak), plus every racer the mods add.
Each racer is taken from the mod that ships its archive (archives/driver<name>.pak), else from
the last registration that has it, so a copy of another mod's racer never hides that mod's own
newer one. A racer is copied object by object: its CRacerDriverData, skin list, skins, outfits
and material packages, with their strings, references and handles renumbered for the merged
file; objects with a name the merged file already has (shared game objects) are referenced, not
copied. Its names go into the root object and name lists, its texts into each .lng file, its name
into characterlisting.igz. The other entries of the registrations (HUD materials, portraits) are
kept, a racer's own from the mod that ships the racer.

The merged registration replaces the highest-numbered mod update archive; the lower ones still
load first, so their other entries stay available. Built-in edits of the game's characterlisting.igz
(the online racer list) are not in it: as with a single racer mod, the mods' registration wins.
The server's item-box rules go over it afterwards (scripts/item_rules.py: every systemData.igz
copy the game loads).
SPDX-License-Identifier: GPL-2.0-or-later
"""
import bisect
import re
import struct
from pathlib import Path

from graphics_tab import Archive, fnv1a, write_archive
from igz_file import Igz, unpack_offsets

SYSTEM = 'looseData/systemData.igz'
LISTING = 'characterlisting.igz'
LANGUAGE = re.compile(r'looseData/systemData_[a-z]+\.lng\Z')
UPDATE = re.compile(r'archives/update(\d*)\.pak\Z', re.I)
PREFIX = 'temporary/octane/data/ps4/output/'
MASK = 0x7FFFFFF
NAMED = 0x80000000
HANDLES = ('RHND', 'RNEX', 'REXT')


def registration(entries):
    """True for an archive's {logical path: bytes} that holds a racer registration."""
    return SYSTEM in entries or LISTING in entries


def update_number(target):
    """The N of archives/updateN.pak (update.pak is 1), None for any other path."""
    m = UPDATE.match(target)
    return (int(m.group(1)) if m.group(1) else 1) if m else None


def read_archive(path):
    """{logical path: (bytes, ordinal)} of an archive."""
    archive = Archive(Path(path))
    try:
        return {archive.logical(i): archive.read(i) for i in range(len(archive.names))}
    finally:
        archive.f.close()


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
    """Objects of one IGZ copied into another, with their references, strings and handles."""

    def __init__(self, source, target):
        self.source, self.target, self.copies = source, target, {}
        self.names = {}
        self.existing = {}
        if any(f[0] == 'ONAM' for f in source.fixups):
            self.names = {o: n for n, o in source.roots()[2]}
            self.existing = {n: o for n, o in target.roots()[2]}

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

    def fix(self, off, new, size, kind):
        s, t = self.source, self.target
        for tag, site in list(s.sites_in(off, size)):
            at = new + site - off
            if tag == 'RSTT':
                t.set_u64(at, t.string_index(s.strings[s.u64(site)]))
            elif tag in ('ROFS', 'RPID'):
                value = s.u64(site)
                ref = value & MASK
                if ref in s.typed:
                    ref = self.object(ref)
                elif kind.endswith('List') and site - off == 0x20:  # a list's data
                    length = s.u64(off + 0x18) & 0xFFFFFFFF
                    block = t.append(bytes(s.data[ref:ref + length]))
                    self.fix(ref, block, length, '')
                    ref = block
                else:
                    raise ValueError(f'unexpected reference at {site:#x} in a {kind or "list"}')
                t.set_u64(at, (value & ~MASK) | ref)
            else:
                value = s.u32(site)
                name, space = s.handle(value)
                t.set_u32(at, t.handle_of(name, space, bool(value & NAMED)))
                spaces = s.depends[0::2]
                if value & NAMED and space in spaces and space not in t.depends[0::2]:
                    t.depends += [space, s.depends[2 * spaces.index(space) + 1]]
            t.add_site(tag, at)


def driver_names(system):
    """The racers (Driver<Name>) of a systemData.igz, in its order."""
    z = File(system) if isinstance(system, (bytes, bytearray)) else system
    return [z.string_at(o + 0x58) for o in z.items(z.drivers())]


def add_racer(merged, source, racer):
    """Racer `racer` (Driver<Name>) of the registration `source` added to `merged` (both
    {logical path: bytes}; merged is changed)."""
    s, t = File(source[SYSTEM]), File(merged[SYSTEM])
    driver = next((o for o in s.items(s.drivers()) if s.string_at(o + 0x58) == racer), None)
    if driver is None:
        raise ValueError(f'{racer} is not in this registration')
    if racer in driver_names(t):
        raise ValueError(f'{racer} is already registered')
    copier = Copier(s, t)
    new = copier.object(driver)
    t.grow(t.drivers(), [new])
    # Its named objects (the driver, skins, outfits) join the root lists, under the same names.
    root, names, _ = t.roots()
    added = [(n, copier.copies[o]) for n, o in s.roots()[2]
             if o in copier.copies and copier.copies[o] != copier.existing.get(n)]
    t.grow(root, [o for _, o in added])
    t.grow(names, [n for n, _ in added], 16)
    merged[SYSTEM] = t.build()
    named = {n for n, _ in added}
    # Its texts: every string of a named object of the racer.
    for path in sorted(p for p in source if LANGUAGE.match(p) and p in merged):
        ls, lt = File(source[path]), File(merged[path])
        have = {(lt.handle(lt.u32(o + 0x18)), lt.handle(lt.u32(o + 0x20)))
                for o in lt.items(lt.find('igLocalizedStringDataList'))}
        copier = Copier(ls, lt)
        texts = []
        for o in ls.items(ls.find('igLocalizedStringDataList')):
            key = (ls.handle(ls.u32(o + 0x18)), ls.handle(ls.u32(o + 0x20)))
            if key[0][0] in named and key not in have:
                texts.append(copier.object(o))
        if texts:
            lt.grow(lt.find('igLocalizedStringDataList'), texts)
            merged[path] = lt.build()
    if LISTING in merged:
        z = File(merged[LISTING])
        lst = z.find('igStringRefList')
        if racer not in [z.string_at(o) for o in
                         (z.ref(lst + 0x20) + 8 * i for i in range(z.u32(lst + 0xC)))]:
            count, data = z.u32(lst + 0xC), z.ref(lst + 0x20)
            at = z.append(bytes(z.data[data:data + 8 * count]) + struct.pack('<Q', z.string_index(racer)))
            for i in range(count + 1):
                z.add_site('RSTT', at + 8 * i)
            z.set_u32(lst + 0xC, count + 1)
            z.set_u32(lst + 0x10, count + 1)
            z.set_u64(lst + 0x18, (z.u64(lst + 0x18) & ~0xFFFFFFFF) | 8 * (count + 1))
            z.set_u64(lst + 0x20, at)
            merged[LISTING] = z.build()


def owned(racer, archives):
    """True when `archives` (the archive paths a mod adds) hold the racer's own archive."""
    return 'archives/' + racer.casefold() + '.pak' in {a.casefold() for a in archives}


def merge(game_entries, registrations, log=print):
    """One registration from the game's own entries and the mods' registrations.

    game_entries: {logical path: bytes} of the game's registration (its newest copies).
    registrations: [(mod, {logical path: bytes}, [archive paths the mod adds])] in load order.
    Returns ({logical path: bytes}, [(racer, mod)])."""
    game = set(driver_names(game_entries[SYSTEM]))
    racers = {}  # racer -> (mod, entries), in the order the registrations add them
    for mod, entries, archives in registrations:
        if SYSTEM not in entries:
            continue
        for racer in driver_names(entries[SYSTEM]):
            if racer in game:
                continue
            current = racers.get(racer)
            if current is None or not owned(racer, current[2]) or owned(racer, archives):
                racers[racer] = (mod, entries, archives)
    merged = {p: d for p, d in game_entries.items() if p in (SYSTEM, LISTING) or LANGUAGE.match(p)}
    for racer, (mod, entries, _) in racers.items():
        add_racer(merged, entries, racer)
    # Other entries (HUD materials, portraits, icon materials): the copy of a mod that ships a
    # racer named in the entry (its path or its content), else the last registration's.
    shorts = [r.casefold().removeprefix('driver') for r in racers]
    paths = dict.fromkeys(p for _, e, _ in registrations for p in e
                          if p not in (SYSTEM, LISTING) and not LANGUAGE.match(p))
    for path in paths:
        copies = [(e[path], a) for _, e, a in registrations if path in e]
        def mine(copy):
            text = (path.encode() + copy[0]).lower()
            return any(owned(r, copy[1]) and s.encode() in text for r, s in zip(racers, shorts))
        merged[path] = ([c for c in copies if mine(c)] or copies)[-1][0]
    added = [(racer, mod) for racer, (mod, _, _) in racers.items()]
    log('Racer registration: ' + ', '.join(f'{r.removeprefix("Driver")} ({m})' for r, m in added)
        + ' merged into one')
    return merged, added


def game_registration(game):
    """The game's own registration entries: the newest copy of each in update.pak, update2.pak."""
    found = {}
    for name in ('update.pak', 'update2.pak'):
        path = Path(game) / 'archives' / name
        if not path.is_file():
            continue
        archive = Archive(path)
        try:
            for i in range(len(archive.names)):
                logical = archive.logical(i)
                if logical in (SYSTEM, LISTING) or LANGUAGE.match(logical):
                    found[logical] = archive.read(i)
        finally:
            archive.f.close()
    if SYSTEM not in found:
        raise ValueError('the game archives have no looseData/systemData.igz')
    return found


def write(dest, entries, template):
    """The registration as an uncompressed update archive."""
    files = sorted(entries.items(), key=lambda e: fnv1a(e[0]))
    write_archive(Path(dest), [(PREFIX + path, path, i << 8, data) for i, (path, data) in enumerate(files)],
                  template)


def prepare(game, files, sources, log=print):
    """The merged registration written to files/<target> when two or more of the update archives
    the enabled mods add hold one. sources: [(mod, target, path, [archives the mod adds])] in mod
    order. Returns the target it replaced (the highest-numbered archive), or None."""
    found = []
    for mod, target, path, archives in sources:
        number = update_number(target)
        if number is None:
            continue
        entries = read_archive(path)
        if registration(entries):
            found.append((number, target, path, mod, entries, archives))
    if len(found) < 2:
        return None
    found.sort(key=lambda f: f[0])  # the game loads update archives by number
    try:
        merged, _ = merge(game_registration(game), [(m, e, a) for _, _, _, m, e, a in found], log)
    except ValueError as error:
        raise ValueError('Racer mods ' + ', '.join(f[3] for f in found) +
                         ' each register racers, and they could not be merged: ' + str(error)) from error
    number, target, path, *_ = found[-1]
    dest = Path(files).joinpath(*Path(target).parts)
    dest.parent.mkdir(parents=True, exist_ok=True)
    template = Archive(Path(path))
    template.f.close()
    write(dest, merged, template)
    return target
