"""Native Normal Mode / Speed Mode / Battle Mode options for the shared online mode selector.

The runtime keeps Speed disabled in public matchmaking until server queues support it.
"""
import argparse
import hashlib
import re
import struct
from pathlib import Path

import archive_patch
import gcn_shaders as iga
from igz_file import Igz, unpack_offsets
from mod_file_paths import resolve, records
from mod_overlays import file_map
from patches import load_segments

ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_modeselect.igz'
LABEL_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_modeselect_en.lng'
TIMER_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_returntolobby_timer.igz'
LOBBY_ENTRY = 'temporary/octane/data/ps4/output/ui/octane_lobby_privategame.igz'
GRAPH_SHA256 = {
    '56d613eaa1234151a3d720af694b460480c4cbc766508cb60c324c91ca8eae47',  # USA 1.21
    'cdb4b366079458a0f1ed32fb8a8ac69d2d841f18bacb9a72430a02e93fcf52a2',  # EU 1.21
    'a23cd1ef8903e50161c2d667e59f85a51fdccecdd913a21d54461f5e693966e3',  # USA widescreen
    'e488136a8c56e367e49e07bdfa42bf7d4c88360893ef135a7ee83eb1e0afa5fc',  # EU widescreen
}
# Native selector's selected-index event adapter, including its RIP-relative LEA.
HOOK_PATTERN = re.compile(bytes.fromhex(
    '55 48 89 e5 41 57 41 56 53 50 48 8d 05') + b'[\x97\xa7]' +
    bytes.fromhex('a1 87 01'), re.DOTALL)

CUP_GRAPHS = {
    'octane_lobby_privategamesettings/octane_lobby_privategamesettings_levelselectbutton':
        ('84e8f56a17637b3fedb4fa7efed6349b6b22c12265ad929c94221e1d6a65f9d2',
         0x378, 'Octane_CupSelect', True),
    'octane_cupselect/octane_cupselect_listbutton':
        ('321eab9262d2d34bed163a88b0d64b209126c7222525ad554986b7ba090e2994',
         0x818, 'Octane_Lobby_PrivateGameSettings', False),
    'octane_cupselect/octane_cupselect_root':
        ('3fae87784d9e0fe697a2d36c4f6c2be7066f73155bf8830bb6bac558421b44b2',
         0x2280, 'Octane_Lobby_PrivateGameSettings', False),
}


def cup_timer(blob):
    g = Igz(blob)
    if (dict(g.objects()).get(0xc0) != 'igGuiPlaceable' or
            g.u64(0xc0 + 0xe8) != 0x6d0 or g.string_at(0x490 + 0x28) != 'TimerDesc'):
        raise ValueError('unsupported native return timer')
    g.set_u64(0xc0 + 0x28, g.string_index('OCTR_PRIVATE_RETURN_TIMER'))
    g.add_site('RSTT', 0xc0 + 0x28)
    return g.build()


def cup_route(blob, specification):
    digest, node, project, entering = specification
    if hashlib.sha256(blob).hexdigest() != digest:
        raise ValueError('unsupported native CUP navigation graph')
    g = Igz(blob)
    g.sites['RHND'] = g.read_list('RHND', unpack_offsets)
    if g.u32(node + 0x10) != 8:
        raise ValueError('unexpected native menu-open pin count')
    template = next(o for o, t in g.objects() if t == 'igVscConstObjectAccessor')
    names = g.fixup('EXNM')
    pairs = list(struct.iter_unpack('<II', names[3][:names[1] * 8]))
    handle = sum(bool(b & 0x80000000) for _, b in pairs)

    def named_pin(name, namespace):
        nonlocal handle
        pin = g.append(g.data[template:template + 24])
        g.add_site('RVTB', pin)
        names[3] = names[3][:names[1] * 8] + struct.pack(
            '<II', g.string_index(name), g.string_index(namespace) | 0x80000000)
        names[1] += 1
        g.set_u64(pin + 0x10, handle | 0x80000000)
        g.add_site('RHND', pin + 0x10)
        handle += 1
        return pin

    pin = named_pin(project, project)
    names[4] = names[2] + len(names[3])
    old = g.u64(node + 0x20)
    pins = [g.u64(old + i * 8) for i in range(8)] + [pin, 0]
    # The eleventh (unused) pin marks entry, which initializes native CUP mode.
    if entering:
        pins.append(0)
    elif node == 0x818:
        pins.extend((0, 0))  # native CUP accept, distinct from Back
    array = g.append(struct.pack(f'<{len(pins)}Q', *pins))
    for i, value in enumerate(pins):
        if value:
            g.add_site('ROFS', array + i * 8)
    g.set_u32(node + 0x10, len(pins))
    g.set_u32(node + 0x14, len(pins))
    g.set_u64(node + 0x18, (1 << 56) | (1 << 52) | len(pins) * 8)
    g.set_u64(node + 0x20, array)
    return g.build()


PACKAGE_PREFIX = 'temporary/octane/data/ps4/output/'
ONLINE_PACKAGE = PACKAGE_PREFIX + 'packages/generated/ui/domains/juicedomain_onlinemenus_pkg.igz'
START_PACKAGE = PACKAGE_PREFIX + 'packages/generated/ui/domains/juicedomain_startscreen_pkg.igz'


def package_rows(g):
    chunks = [o for o, t in g.objects() if t == 'igStreamingChunkInfo']
    if len(chunks) != 1:
        raise ValueError('expected one native streaming chunk')
    chunk = chunks[0]
    count, array = g.u64(chunk + 0x28), g.u64(chunk + 0x38)
    if g.u32(chunk + 0x30) != count * 16:
        raise ValueError('unexpected native streaming file array')
    return chunk, [(g.string_at(array + i * 16), g.string_at(array + i * 16 + 8))
                   for i in range(count)]


def cup_package(game, blob, source_blob, assets=None):
    g, source = Igz(blob), Igz(source_blob)
    chunk, original = package_rows(g)
    _, source_rows = package_rows(source)
    source_types = {path: kind for kind, path in source_rows}
    seen = {path for _, path in original}
    # Match the native arcade package's dependency closure. Shared/global assets
    # absent from that package remain owned by their existing global chunks.
    files = {}
    for name in ('juicedomain_startscreen', 'update', 'update2'):
        pak = resolve(Path(game) / 'archives' / (name + '.pak'))
        for entry in iga.pak_entries(pak):
            files[entry[0].lower().removeprefix(PACKAGE_PREFIX)] = (pak, entry)
    pending = ['ui/octane_cupselect.igz', 'materialinstances/ui/cupselect_materials.igz']
    needed = set()
    while pending:
        path = pending.pop()
        if path in seen or path not in source_types:
            continue
        seen.add(path)
        needed.add(path)
        pak, entry = files[path]
        # The Arcade domain archive is released when entering Online. Keep its
        # required files available through the runtime's existing loose-file map.
        if assets is not None and pak == resolve(Path(game) / 'archives/juicedomain_startscreen.pak'):
            target = assets / path
            target.parent.mkdir(parents=True, exist_ok=True)
            with pak.open('rb') as f:
                target.write_bytes(iga.read_entry(f, *entry[1:]))
        if source_types[path] == 'texture':
            continue
        with pak.open('rb') as f:
            asset = Igz(iga.read_entry(f, *entry[1:]))
        try:
            dependencies = asset.fixup('TDEP')
        except KeyError:
            continue
        strings = dependencies[3].split(b'\0')[:dependencies[1] * 2]
        for dependency in strings[1::2]:
            pending.append(dependency.decode().lower().replace(
                '<build>project:/' + PACKAGE_PREFIX, '').replace(':/', '/'))
    rows = original + [(kind, path) for kind, path in source_rows if path in needed]
    array = g.append(b''.join(struct.pack('<QQ', g.string_index(kind), g.string_index(path))
                              for kind, path in rows))
    for offset in range(array, array + len(rows) * 16, 8):
        g.add_site('RSTT', offset)
    g.set_u64(chunk + 0x28, len(rows))
    g.set_u64(chunk + 0x30, (g.u64(chunk + 0x30) & ~0xffffffff) | len(rows) * 16)
    g.set_u64(chunk + 0x38, array)
    return g.build()


def hook_site(elf):
    hits = [v + m.start() for v, off, size in load_segments(elf)
            for m in HOOK_PATTERN.finditer(elf[off:off + size])
            if v + m.start() + 17 + struct.unpack_from('<i', m.group(), 13)[0] == 0x2af1fa8]
    if len(hits) != 1 or hits[0] not in (0x1277e00, 0x1277df0):
        raise ValueError('native Online selector adapter is not uniquely recognized')
    return hits[0]


def set_options(g, options, enabled, labels):
    for lst, raw, tag in (
        (options, b''.join(struct.pack('<Q', g.string_index(s)) for s in labels), 'RSTT'),
        (enabled, bytes([1] * len(labels)), None),
    ):
        array = g.append(raw)
        g.set_u32(lst + 0x0c, len(labels))
        g.set_u32(lst + 0x10, len(labels))
        g.set_u64(lst + 0x18, (g.u64(lst + 0x18) & ~0xffffffff) | (1 << 56) | len(raw))
        g.set_u64(lst + 0x20, array)
        g.add_site('ROFS', lst + 0x20)
        if tag:
            for i in range(len(labels)):
                g.add_site(tag, array + i * 8)


def add_race_type(g):
    # Copy the native selector, including its timer, animation categories and fixups.
    for tag in ('RHND', 'RNEX'):
        g.sites[tag] = g.read_list(tag, unpack_offsets)
    blocks = ((0x1000, 0x1120), (0x1648, 0x18e8))
    copies = [(lo, hi, g.append(g.data[lo:hi])) for lo, hi in blocks]

    def relocated(value):
        for lo, hi, new in copies:
            if lo <= value < hi:
                return new + value - lo
        return value

    for tag, sites in g.sites.items():
        for site in list(sites):
            target = relocated(site)
            if target != site:
                g.add_site(tag, target)
                if tag == 'ROFS':
                    g.set_u64(target, relocated(g.u64(site)))
    row, behavior = relocated(0x1000), relocated(0x1678)
    g.set_u64(behavior + 0x148, g.string_index('RACE TYPE'))
    g.add_site('RSTT', behavior + 0x148)
    set_options(g, relocated(0x1880), relocated(0x18a8), ('SINGLE', 'CUP'))
    mode_y = struct.unpack_from('<f', g.data, 0x1064)[0]
    lobby_y = struct.unpack_from('<f', g.data, 0xb74)[0]
    # Both native rows share an unshifted parent: preserve their existing pitch.
    struct.pack_into('<f', g.data, row + 0x64, mode_y + (mode_y - lobby_y))
    # Start hidden; the runtime reveals it only in private Normal/Speed matches.
    g.set_u32(row + 0x0c, g.u32(row + 0x0c) | 0x400)
    g.set_u64(row + 0x50, 0x1000)
    g.set_u64(0x1000 + 0x58, row)
    g.set_u64(0x1120 + 0x50, row)
    g.set_u64(behavior + 0x110, 0x1000)
    # Add the row to the native child table as well as its traversal chain.
    children = g.append(struct.pack('<3Q', 0x1000, row, 0x1120))
    for i in range(3):
        g.add_site('ROFS', children + 8 * i)
    g.set_u32(0xfc0 + 0x0c, 3)
    g.set_u32(0xfc0 + 0x10, 3)
    g.set_u64(0xfc0 + 0x18, (1 << 56) | (1 << 52) | 24)
    g.set_u64(0xfc0 + 0x20, children)


def rebuild(blob):
    if hashlib.sha256(blob).hexdigest() not in GRAPH_SHA256:
        raise ValueError('native Online mode selector is not the supported USA/EU 1.21 asset')
    g = Igz(blob)
    add_race_type(g)
    # The native selector consumes the same option/availability lists as other selectors.
    set_options(g, 0x1880, 0x18a8, ('NORMAL MODE', 'SPEED MODE', 'BATTLE MODE'))
    return g.build()


def lobby_card(blob):
    if hashlib.sha256(blob).hexdigest() not in {
        'bc50c63007445a18a1d57c9c4b1176428330e6686bb4943a86f09463d922f6ea',
        'a468bac4f2141363d934a3ea2cb6540908e0d6481214890ed8a3807caa191c37',
    }:
        raise ValueError('unsupported native private lobby card')
    g = Igz(blob)
    # Run the existing hooked native behavior after every VSC settings binding.
    behaviors = g.u64(0x480 + 0xe8)
    slots = g.u64(behaviors + 0x20)
    count = g.u32(behaviors + 12)
    values = [g.u64(slots + i * 8) for i in range(count)]
    types = dict(g.objects())
    hooks = [v for v in values if types.get(v) == 'CGuiBehaviorReturnToMainMenuOnDisconnect']
    if len(hooks) != 1:
        raise ValueError('unsupported private lobby event order')
    values = [v for v in values if v != hooks[0]] + hooks
    for i, value in enumerate(values):
        g.set_u64(slots + i * 8, value)
    # Tags identify only this menu and its existing postcard; native layout is retained.
    for placeable, name in ((0x480, 'OCTR_PRIVATE_LOBBY'), (0x4180, 'OCTR_PRIVATE_PREVIEW')):
        if g.u64(placeable + 0x28):
            raise ValueError('private lobby marker would overwrite a native binding')
        g.set_u64(placeable + 0x28, g.string_index(name))
        g.add_site('RSTT', placeable + 0x28)
    return g.build()


def current_entry(pak, entries, index, base):
    """The entry's bytes as the game reads them now: base's own replacement (a widescreen layout's
    menu/HUD fix, kept uncompressed) when it already has one for this entry, else the archive's
    own (decompressed as the game would)."""
    if base is not None:
        _size, _guest, ranges = archive_patch.read(base)
        count = len(entries)
        record_at = 0x38 + 4 * count + 16 * index
        record = next((d for o, d in ranges if o == record_at), None)
        if record is not None:
            offset_lo, hi_extra, length, _mode = struct.unpack('<4I', record)
            position = offset_lo | ((hi_extra & 0xFF) << 32)
            return archive_patch.view(pak, base, position, length)
    with pak.open('rb') as f:
        return iga.read_entry(f, *entries[index][1:])



def prepare(game, out, elf, bases):
    out = Path(out)
    assets = out / 'cup-assets'
    site = hook_site(Path(elf).read_bytes())
    pak = resolve(Path(game) / 'archives/update2.pak')
    entries = iga.pak_entries(pak)
    index = next((i for i, e in enumerate(entries) if e[0].lower() == ENTRY), None)
    if index is None:
        raise ValueError('native Online menu graph is missing')
    guest = '/app0/archives/update2.pak'
    patches = {archive_patch.read(p)[1]: Path(p) for p in bases}
    base = patches.get(guest)
    payload = rebuild(current_entry(pak, entries, index, base))
    with pak.open('rb') as f:
        lobby_index = next(i for i, e in enumerate(entries) if e[0].lower() == LOBBY_ENTRY)
        lobby = lobby_card(iga.read_entry(f, *entries[lobby_index][1:]))
        package_index = next(i for i, e in enumerate(entries) if e[0].lower() == ONLINE_PACKAGE)
        source_index = next(i for i, e in enumerate(entries) if e[0].lower() == START_PACKAGE)
        package = cup_package(game, iga.read_entry(f, *entries[package_index][1:]),
                              iga.read_entry(f, *entries[source_index][1:]), assets)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    _, mapped = records()
    mapped = dict(mapped)
    for asset in assets.rglob('*'):
        if asset.is_file():
            mapped.setdefault(asset.relative_to(assets).as_posix(), asset)
    file_map(game, out / 'cup-files.map', out / 'cup-file-dirs',
             [(Path(relative), source) for relative, source in mapped.items()])
    target = out / 'online-mode.bbpatch'
    archive_patch.write_entries_patch(target, pak, guest, {index: payload, lobby_index: lobby, package_index: package}, base, override={index})
    patches[guest] = target
    label_pak = resolve(Path(game) / 'archives/update.pak')
    label_entries = iga.pak_entries(label_pak)
    label_index = next(i for i, e in enumerate(label_entries) if e[0].lower() == LABEL_ENTRY)
    with label_pak.open('rb') as f:
        label = Igz(iga.read_entry(f, *label_entries[label_index][1:]))
    if label.strings.count('Racing Type') != 1:
        raise ValueError('native Online mode label is not recognized')
    label.strings[label.strings.index('Racing Type')] = 'GAME MODE'
    guest = '/app0/archives/update.pak'
    target = out / 'online-mode-label.bbpatch'
    replacements = {label_index: label.build()}
    with label_pak.open('rb') as f:
        timer_index = next(i for i, e in enumerate(label_entries) if e[0].lower() == TIMER_ENTRY)
        replacements[timer_index] = cup_timer(iga.read_entry(f, *label_entries[timer_index][1:]))
        for name, specification in CUP_GRAPHS.items():
            path = 'temporary/octane/data/ps4/output/vsc/ui/' + name + '.igz'
            index = next(i for i, e in enumerate(label_entries) if e[0].lower() == path)
            replacements[index] = cup_route(iga.read_entry(f, *label_entries[index][1:]), specification)
    archive_patch.write_entries_patch(target, label_pak, guest, replacements, patches.get(guest))
    patches[guest] = target
    return list(patches.values()), f'{site:x}'


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--elf', required=True, type=Path)
    p.add_argument('--base', action='append', default=[], type=Path)
    a = p.parse_args()
    patches, hook = prepare(a.game, a.out, a.elf, a.base)
    print('archive_patches=' + '|'.join(map(str, patches)))
    print('online_mode_hook=' + hook)
    print('cup_file_map=' + str((a.out / 'cup-files.map').resolve()))
