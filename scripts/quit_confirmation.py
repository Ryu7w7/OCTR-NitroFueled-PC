"""Private Quit Game behavior using the game's existing two-option modal dialog."""
import struct

from igz_file import Igz, unpack_offsets, write_strings

ORIGINAL = 'Octane_Button_Open_Level'
NAME = 'Octane_Bbport_Quit_Level'
SOURCE = f'vsc/common/ui/octane_buttons/{ORIGINAL.lower()}.igz'


def graph(blob):
    g = Igz(blob)
    objects = dict(g.objects())
    expected = {0x7b0: 'igVscCppCompoundNode', 0x1028: 'igVscCppActionNode',
                0x17f8: 'igVscCppActionNode', 0x1b28: 'igVscEntryPointNode',
                0x1d78: 'igVscTestBoolNode'}
    if any(objects.get(at) != kind for at, kind in expected.items()):
        raise ValueError('unsupported native Quit confirmation graph')
    if (g.u64(0x1d78 + 0x18), g.u64(0x1d78 + 0x20)) != (0xfb0, 0x1098):
        raise ValueError('unexpected native Open Level branches')
    # This private copy always asks. Other Open Level buttons retain their cheat warning.
    g.set_u64(0x1d78 + 0x18, 0x1028)
    g.set_u64(0x1d78 + 0x20, 0x1028)
    # Confirm loads only the private quit marker; keep native Back/close delay and controls.
    g.set_u64(0x1b28 + 0x30, 0x17f8)
    # Use the native dialog's string pins without changing shared localized warnings.
    for at, text in ((0x960, 'Quit game'), (0xa38, 'Are you sure you want to quit the game?'),
                     (0xb10, 'Confirm'), (0xbe8, 'Back')):
        if objects.get(at) != 'igVscLocalizedStringToStringAccessor':
            raise ValueError('unexpected native dialog text accessor')
        g.set_u64(at + 0x10, g.string_index(text))
        g.sites['ROFS'].remove(at + 0x10)
        g.add_site('RSTT', at + 0x10)
    g.types[g.types.index('igVscLocalizedStringToStringAccessor')] = 'igVscConstStringAccessor'
    types = g.fixup('TMET')
    types[3] = write_strings(g.types)
    types[4] = (types[2] + len(types[3]) + 3) & ~3
    g.strings = [s.replace(ORIGINAL, NAME) for s in g.strings]
    # Renamed exports need matching hashes for native graph metadata lookup.
    from graphics_tab import fnv1a
    for table, kind in g.objects():
        if kind == 'igNameList':
            start = g.u64(table + 32)
            for i in range(g.u32(table + 12)):
                entry = start + i * 16
                g.set_u32(entry + 8, fnv1a(g.string_at(entry)))
    return g.build()


def bind(g, behavior):
    """Bind only Wireless/Quit to the private graph, keeping all other buttons intact."""
    f = g.fixup('EXNM')
    entries = list(struct.iter_unpack('<II', f[3][:f[1] * 8]))
    externals = [(a, b) for a, b in entries if not b & 0x80000000]
    meta = behavior + 0x20
    if (meta not in g.read_list('RNEX', unpack_offsets) or
            g.strings[externals[g.u64(meta)][0]] != ORIGINAL):
        raise ValueError('unexpected Quit behavior type')
    g.set_u64(meta, len(externals))
    entries.append((g.string_index(NAME), g.string_index(NAME)))
    f[1], f[3] = len(entries), b''.join(struct.pack('<II', *e) for e in entries)
    f[4] = (f[2] + len(f[3]) + 3) & ~3
    deps = g.fixup('TDEP')
    raw = deps[3].split(b'\0')[:deps[1] * 2]
    raw.extend((NAME.encode(), f'vsc:/common/UI/Octane_Buttons/{NAME}.igz'.encode()))
    deps[1] += 1
    deps[3] = b''.join(s + b'\0' for s in raw)
    deps[4] = (deps[2] + len(deps[3]) + 3) & ~3
