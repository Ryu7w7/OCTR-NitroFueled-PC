"""Hall of Fame uses a separate copy of the native leaderboard project."""
import struct
from igz_file import Igz, unpack_offsets
from graphics_tab import fnv1a

PROJECT = 'Octane_HallOfFame'
PATH = 'ui/octane_halloffame.igz'


def refresh_export_hashes(g):
    for table, kind in g.objects():
        if kind != 'igNameList': continue
        start = g.u64(table + 32)
        for i in range(g.u32(table + 12)):
            entry = start + i * 16
            g.set_u32(entry + 8, fnv1a(g.strings[g.u64(entry)]))


def dependencies(g, additions):
    f = g.fixup('TDEP')
    strings = f[3].split(b'\0')[:f[1] * 2]
    pairs = dict(zip(strings[::2], strings[1::2]))
    pairs.update({k.encode(): v.encode() for k, v in additions.items()})
    f[1] = len(pairs)
    # Native TDEP parsing stops at an empty namespace, not the entry count.
    f[3] = b''.join(k + b'\0' + v + b'\0' for k, v in sorted(pairs.items())) + b'\0'
    f[4] = (f[2] + len(f[3]) + 3) & ~3


def external(g, name, namespace, handle):
    f = g.fixup('EXNM')
    pairs = list(struct.iter_unpack('<II', f[3][:f[1] * 8]))
    filtered = [(g.strings[a], g.strings[b & 0x7fffffff])
                for a, b in pairs if bool(b & 0x80000000) == handle]
    pair = (name, namespace)
    if pair not in filtered:
        pairs.append((g.string_index(name), g.string_index(namespace) | (0x80000000 if handle else 0)))
        f[1] = len(pairs)
        f[3] = b''.join(struct.pack('<II', *p) for p in pairs)
        f[4] = (f[2] + len(f[3]) + 3) & ~3
        filtered.append(pair)
    return filtered.index(pair) | (0x80000000 if handle else 0)


def marker(g, off, name):
    g.set_u64(off + 0x28, g.string_index(name))
    g.add_site('RSTT', off + 0x28)


def clear_behaviors(g, off):
    g.set_u64(off + 0xe8, 0)
    if off + 0xe8 in g.sites['ROFS']:
        g.sites['ROFS'].remove(off + 0xe8)


def append_traversal(g, root, children):
    """Append to the native next/previous chain as well as the child tables."""
    tail = root
    seen = set()
    while g.u64(tail + 0x50):
        if tail in seen:
            raise ValueError('unsupported cyclic HOF traversal')
        seen.add(tail)
        tail = g.u64(tail + 0x50)
    for child in children:
        g.set_u64(tail + 0x50, child)
        g.add_site('ROFS', tail + 0x50)
        g.set_u64(child + 0x58, tail)
        g.add_site('ROFS', child + 0x58)
        tail = child
    g.set_u64(tail + 0x50, 0)
    if tail + 0x50 in g.sites['ROFS']: g.sites['ROFS'].remove(tail + 0x50)


def leaderboard(blob):
    g = Igz(blob)
    kinds = dict(g.objects())
    if kinds.get(0x4180) != 'CGuiBehaviorLeaderboardList' or kinds.get(0xc008) != 'CGuiBehaviorLeaderboardListItem':
        raise ValueError('unsupported native leaderboard layout')
    for tag in ('RHND', 'RNEX'):
        g.sites[tag] = g.read_list(tag, unpack_offsets)
    # Only the project namespace changes. Keep native graph and material dependencies.
    g.strings = [PROJECT if s == 'Octane_Leaderboards' else s for s in g.strings]
    refresh_export_hashes(g)
    # Native Back opens this instance's asset directly, bypassing named-open hooks.
    identifiers = g.fixup('EXID')
    target = g.u64(0x4a38) * 8
    highscore = fnv1a('Octane_Highscore_Select')
    if struct.unpack_from('<II', identifiers[3], target) != (highscore, highscore):
        raise ValueError('unsupported native leaderboard Back target')
    lobby = fnv1a('Octane_Lobby_Main')
    identifiers[3] = (identifiers[3][:target] + struct.pack('<II', lobby, lobby) +
                      identifiers[3][target + 8:])
    marker(g, 0x12f0, 'OCTR_HOF_ROOT')
    # The original list remains the focus target; rows are native Comp instances populated by C.
    clear_behaviors(g, 0x3df0)
    clear_behaviors(g, 0x6a0)
    marker(g, 0x3df0, 'OCTR_HOF_LIST')
    for off, tag in ((0x2af0, 'OCTR_HOF_MODE'), (0x2750, 'OCTR_HOF_POPULATION'),
                     (0x23b0, 'OCTR_HOF_SEASON')):
        marker(g, off, tag)
        li = g.u64(off + 0xe8)
        g.set_u32(li + 12, 1)
        g.set_u32(li + 16, 1)
        # Retain the native navigation behavior, remove the time-trial selector.
        second = g.u64(li + 0x20) + 8
        g.set_u64(second, 0)
        g.sites['ROFS'].remove(second)
    # Keep the selector's native focus animations after removing its time-trial data behavior.
    for at in (0xb0, 0xb8):
        g.set_u64(0x2ec8 + at, g.u64(0x31f0 + at))
        g.add_site('RNEX', 0x2ec8 + at)
    marker(g, 0x2170, 'OCTR_HOF_TRACK')
    clear_behaviors(g, 0x2170)
    # Track's container cannot receive focus; Area/Season becomes the last selector.
    g.set_u32(0x2010 + 12, g.u32(0x2010 + 12) & ~0x400)
    marker(g, 0x2010, 'OCTR_HOF_TRACK_CONTAINER')
    # Tag row fields inside the native Comp template, retaining fonts, shadows and materials.
    for off, tag in ((0xcd0, 'OCTR_HOF_RANK'), (0xa90, 'OCTR_HOF_NAME'),
                     (0x970, 'OCTR_HOF_TIER')):
        marker(g, off, tag)
    marker(g, 0xbb0, 'OCTR_HOF_PORTRAIT')
    marker(g, 0xf10, 'OCTR_HOF_STYLE')
    marker(g, 0x3f10, 'OCTR_HOF_SELF')
    marker(g, 0x4030, 'OCTR_HOF_LOADING')
    struct.pack_into('<2f', g.data, 0x970 + 0x60, 140.0, 0.0)
    struct.pack_into('<3f', g.data, 0x970 + 0x6c, 0.0, 0.5, 0.5)
    struct.pack_into('<2f', g.data, 0x970 + 0x90, 360.0, 50.0)
    struct.pack_into('<3f', g.data, 0x970 + 0x84, 1.0, 1.0, 1.0)
    for at in (0xa0, 0xa4):
        g.set_u32(0x970 + at, g.u32(0xa90 + at))
    # Native forceFitInBox scales each label to its own column without clipping.
    for name in (0xa90, 0x8b30):
        struct.pack_into('<2f', g.data, name + 0x60, -270.0, 0.0)
        struct.pack_into('<2f', g.data, name + 0x90, 330.0, 50.0)
    for key, kind in g.objects():
        if 0x92f0 <= key < 0x9840 and kind == 'igGuiVec3LinearKeyframe':
            struct.pack_into('<3f', g.data, key + 0x18, -270.0, 0.0, 0.0)
    # Focus must not enlarge the shared content container into adjacent columns.
    for key in (0xbe50, 0xbe78, 0xbf60, 0xbf88):
        struct.pack_into('<3f', g.data, key + 0x18, 0.9, 0.9, 1.0)
    for key, kind in g.objects():
        if 0xa2b0 <= key < 0xb6c0 and kind == 'igGuiVec3LinearKeyframe':
            struct.pack_into('<3f', g.data, key + 0x18, 0.8, 0.8, 1.0)
    for rank, name in ((0xcd0, 0xa90), (0x88f0, 0x8b30)):
        struct.pack_into('<2f', g.data, rank + 0x60, -460.0, 0.0)
        struct.pack_into('<2f', g.data, rank + 0x90, 150.0, 56.0)
        for at in (0xa0, 0xa4):
            g.set_u32(rank + at, g.u32(name + at))
    for key in (0x9c08, 0x9c30, 0x9df0, 0x9e18, 0x9e40):
        struct.pack_into('<3f', g.data, key + 0x18, -460.0, 0.0, 0.0)
    # The time asset is right-aligned; use the name asset's left-aligned, fitted text style.
    for at in (0x20, 0x5c, 0x60, 0x68):
        g.set_u32(0x7ff0 + at, g.u32(0x7f40 + at))
    # Reuse the native selectable row, including its focus/defocus animations.
    for off, tag in ((0x8b30, 'OCTR_HOF_NAME'), (0x8c50, 'OCTR_HOF_TIER'),
                     (0x88f0, 'OCTR_HOF_RANK'), (0x8a10, 'OCTR_HOF_PORTRAIT'),
                     (0xb6c0, 'OCTR_HOF_STYLE')):
        marker(g, off, tag)
    struct.pack_into('<2f', g.data, 0x8c50 + 0x60, 140.0, 0.0)
    struct.pack_into('<3f', g.data, 0x8c50 + 0x6c, 0.0, 0.5, 0.5)
    struct.pack_into('<2f', g.data, 0x8c50 + 0x90, 360.0, 50.0)
    struct.pack_into('<3f', g.data, 0x8c50 + 0x84, 1.0, 1.0, 1.0)
    for at in (0xa0, 0xa4):
        g.set_u32(0x8c50 + at, g.u32(0x8b30 + at))
    # Keep the tier column fixed while native focus animations change its colors.
    for key in (0x8ea8, 0x8ed0, 0x9180, 0x91a8, 0x91d0):
        struct.pack_into('<3f', g.data, key + 0x18, 140.0, 0.0, 0.0)
    # The removed data behavior supplied these native row focus animation tags.
    for target, source in ((0xb0, 0xd0), (0xb8, 0xd8), (0xc8, 0xe0), (0xd0, 0xe8)):
        g.set_u64(0x7a40 + target, g.u64(0xc008 + source))
        g.add_site('ROFS', 0x7a40 + target)
    native_behaviors = g.u64(0x84f0 + 0xe8)
    native_array = g.u64(native_behaviors + 0x20)
    g.set_u32(native_behaviors + 12, 1)
    g.set_u32(native_behaviors + 16, 1)
    g.set_u64(native_behaviors + 0x18, (g.u64(native_behaviors + 0x18) & ~0xffffffff) | 8)
    g.set_u64(native_behaviors + 0x20, native_array + 8)
    g.set_u64(native_array, 0)
    g.sites['ROFS'].remove(native_array)
    g.set_u32(0x3df0 + 12, g.u32(0x3df0 + 12) | 0x400)
    root_table = g.u64(0x12f0 + 0x48)
    array = g.u64(root_table + 0x20)
    children = [g.u64(array + i * 8) for i in range(g.u32(root_table + 12))]
    source = 0x3df0
    size = g.sizes[g.u64(source)]
    exports = []
    rows = []
    for i in range(8):
        lo, hi = 0x84f0, 0xc258
        row_root = g.append(g.data[lo:hi])
        for tag, sites in g.sites.items():
            for site in list(sites):
                if lo <= site < hi:
                    target = row_root + site - lo
                    g.add_site(tag, target)
                    if tag == 'ROFS' and lo <= g.u64(site) < hi:
                        g.set_u64(target, row_root + g.u64(site) - lo)
        composition = g.append(g.data[0x670:0x6a0])
        for tag, sites in g.sites.items():
            for site in list(sites):
                if 0x670 <= site < 0x6a0:
                    g.add_site(tag, composition + site - 0x670)
        g.set_u64(composition + 0x20, row_root)
        g.add_site('ROFS', composition + 0x20)
        row = g.append(g.data[source:source + size])
        for tag, sites in g.sites.items():
            for site in list(sites):
                if source <= site < source + size:
                    g.add_site(tag, row + site - source)
        marker(g, row, 'OCTR_HOF_ROW_' + str(i))
        g.set_u32(row + 12, (g.u32(row + 12) | 1) & ~0x400)
        name = 'OCTR_HOF_ROW_COMP_' + str(i)
        exports.append((composition, name))
        g.set_u64(row + 0x38, external(g, name, PROJECT, True))
        g.set_u64(row + 0xe8, g.u64(row_root + 0xe8))
        g.add_site('ROFS', row + 0xe8)
        clear_behaviors(g, row_root)
        # The source is a leaf instance; detach its sibling navigation links.
        for at in (0x48, 0x50, 0x58):
            g.set_u64(row + at, 0)
            if row + at in g.sites['ROFS']: g.sites['ROFS'].remove(row + at)
        x, y = struct.unpack_from('<2f', g.data, source + 0x60)
        struct.pack_into('<2f', g.data, row + 0x60, x, y + i * struct.unpack_from('<f', g.data, 0x6a0 + 0x94)[0])
        children.append(row)
        rows.append(row)
    # Reuse the same exported tier sprites as Lobby Members in each native row.
    for composition in [0x670] + [asset for asset, _ in exports]:
        root = g.u64(composition + 0x20)
        label = 0x970 if composition == 0x670 else root + (0x8c50 - 0x84f0)
        parent = g.u64(label + 0x40)
        table = g.u64(parent + 0x48)
        start = g.u64(table + 0x20)
        items = [g.u64(start + i * 8) for i in range(g.u32(table + 12))]
        icons = []
        for mode in range(2):
            for tier in range(1, 8):
                source = 0xbb0
                size = g.sizes[g.u64(source)]
                icon = g.append(g.data[source:source + size])
                for tag, sites in g.sites.items():
                    for site in list(sites):
                        if source <= site < source + size:
                            g.add_site(tag, icon + site - source)
                marker(g, icon, f'OCTR_HOF_ICON_{mode}_{tier}')
                g.set_u64(icon + 0x38, external(g, f'OCTR_RANK_{mode}_{tier}_ASSET',
                                               'Octane_LobbyMembers', True))
                for at in (0x40, 0x48, 0x50, 0x58, 0xe8, 0xf0, 0xf8):
                    g.set_u64(icon + at, parent if at == 0x40 else 0)
                    if icon + at in g.sites['ROFS']: g.sites['ROFS'].remove(icon + at)
                g.add_site('ROFS', icon + 0x40)
                g.set_u32(icon + 12, g.u32(icon + 12) & ~0x400)
                struct.pack_into('<2f', g.data, icon + 0x60, 88.0, 0.0)
                struct.pack_into('<3f', g.data, icon + 0x6c, 0.0, 0.5, 0.5)
                # Exported tier sprites render at their intrinsic 100-pixel size.
                struct.pack_into('<3f', g.data, icon + 0x84, 0.4, 0.4, 1.0)
                struct.pack_into('<2f', g.data, icon + 0x90, 40.0, 40.0)
                for at in (0xa0, 0xa4):
                    g.set_u32(icon + at, g.u32(label + at))
                struct.pack_into('<4f', g.data, icon + 0xc0, 1.0, 1.0, 1.0, 1.0)
                items.append(icon)
                icons.append(icon)
        append_traversal(g, root, icons)
        array = g.append(struct.pack('<' + 'Q' * len(items), *items))
        for i in range(len(items)): g.add_site('ROFS', array + i * 8)
        g.set_u32(table + 12, len(items)); g.set_u32(table + 16, len(items))
        g.set_u64(table + 24, (g.u64(table + 24) & ~0xffffffff) | len(items) * 8)
        g.set_u64(table + 32, array)
    append_traversal(g, 0x12f0, rows)
    dependencies(g, {'Octane_LobbyMembers': 'ui:/Octane_LobbyMembers.igz'})
    # Asset fields hold native handles; their targets must be named exports.
    names = next(o for o, t in g.objects() if t == 'igNameList')
    for table, stride, tag in ((0, 8, 'ROFS'), (names, 16, 'RSTT')):
        count = g.u32(table + 12)
        start = g.u64(table + 0x20)
        data = bytes(g.data[start:start + count * stride])
        for composition, name in exports:
            data += (struct.pack('<Q', composition) if stride == 8 else
                     struct.pack('<QQ', g.string_index(name), fnv1a(name)))
        target = g.append(data)
        for index in range(count + len(exports)):
            g.add_site(tag, target + index * stride)
        g.set_u32(table + 12, count + len(exports))
        g.set_u32(table + 16, count + len(exports))
        g.set_u64(table + 24, (g.u64(table + 24) & ~0xffffffff) | len(data))
        g.set_u64(table + 32, target)
    array = g.append(struct.pack('<' + 'Q' * len(children), *children))
    for i in range(len(children)): g.add_site('ROFS', array + i * 8)
    g.set_u32(root_table + 12, len(children))
    g.set_u32(root_table + 16, len(children))
    g.set_u64(root_table + 0x18, (g.u64(root_table + 0x18) & ~0xffffffff) | len(children) * 8)
    g.set_u64(root_table + 0x20, array)
    return g.build()
