"""Add the clan tag to the shared native customization menu, immediately after Karts."""
import struct

from hall_of_fame import marker
from igz_file import Igz, unpack_offsets

ASSET = 'ui/guisharedassets/octane_customization_mainoptions.igz'
BUTTON = 'OCTR_CLAN_TAG_BUTTON'


def add_clan_tag(blob):
    g = Igz(blob)
    objects = dict(g.objects())
    if BUTTON in g.strings:
        return blob
    # Each version uses a different object layout. Identify Karts by its icon
    # and navigation behavior, and preserve the surrounding menu's own links.
    ex = g.fixup('EXNM')
    handles = [g.strings[name] for name, ns in struct.iter_unpack('<II', ex[3][:8 * ex[1]])
               if ns & 0x80000000]
    navs = {}
    kart = None
    for row, kind in objects.items():
        if kind != 'igGuiPlaceable' or row + 0xe8 not in g.sites['ROFS']:
            continue
        behaviors = g.u64(row + 0xe8)
        first = g.u64(g.u64(behaviors + 0x20))
        if objects.get(first) != 'CGuiBehaviorOctaneNavigationItem':
            continue
        navs[row] = first
        icon = g.u32(first + 0x158) & 0x7fffffff
        if icon < len(handles) and handles[icon] == 'octane_ui_ico_kart':
            if kart is not None:
                raise ValueError('ambiguous Karts row')
            kart = row
    if kart is None:
        raise ValueError('missing native Karts row')
    nav = navs[kart]
    parent = g.u64(kart + 0x40)
    table = g.u64(parent + 0x48)
    count = g.u32(table + 12)
    children = [g.u64(g.u64(table + 0x20) + 8*i) for i in range(count)]
    behaviors = g.u64(kart + 0xe8)
    behavior_data = g.u64(behaviors + 0x20)
    script = g.u64(behavior_data + 8)
    if (children.count(kart) != 1 or count > 32 or g.u32(behaviors + 12) != 2 or
            objects.get(script) != 'igGuiVscBehavior' or not behaviors < nav < script):
        raise ValueError('unsupported native Karts layout')
    # Clone the prefab and its navigation state (timers and animation categories).
    # Its second behavior opens the kart picker and belongs only to the original.
    row_end = next(off for off in sorted(objects) if off > kart)
    blocks = [(lo, hi, g.append(g.data[lo:hi])) for lo, hi in
              ((kart, row_end), (behaviors, script))]

    def relocated(value):
        return next((new + value - lo for lo, hi, new in blocks if lo <= value < hi), value)

    for fix in ('RHND', 'RNEX'):
        g.sites[fix] = g.read_list(fix, unpack_offsets)
    for fix, sites in g.sites.items():
        for old in list(sites):
            new = relocated(old)
            if new != old:
                g.add_site(fix, new)
                if fix == 'ROFS':
                    g.set_u64(new, relocated(g.u64(old)))

    row, new_nav, new_list = map(relocated, (kart, nav, behaviors))
    marker(g, row, BUTTON)
    g.set_u32(new_list + 12, 1)
    g.set_u32(new_list + 16, 1)
    g.set_u32(new_list + 24, 8)
    unused = relocated(behavior_data + 8)
    g.sites['ROFS'].remove(unused)
    g.set_u64(unused, 0)
    exports = [g.u64(g.u64(0x20) + i*8) for i in range(g.u32(12))]
    for obj in exports:
        copy = relocated(obj)
        if copy != obj:
            g.set_u32(copy + 8, g.u32(copy + 8) - 1)

    # The visual sibling list runs in reverse. The input graph runs forward.
    sibling = g.u64(kart + 0x58)
    successor = g.u64(nav + 0x118)
    g.set_u64(kart + 0x58, row)
    g.set_u64(row + 0x50, kart)
    g.set_u64(sibling + 0x50, row)
    g.set_u64(nav + 0x118, row)
    g.set_u64(new_nav + 0x110, kart)
    for other in navs.values():
        if other != new_nav and g.u64(other + 0x110) == kart:
            g.set_u64(other + 0x110, row)
    g.set_u64(new_nav + 0x118, successor)
    y = struct.unpack_from('<f', g.data, kart + 0x64)[0]
    step = y - struct.unpack_from('<f', g.data, g.u64(nav + 0x110) + 0x64)[0]
    for child in children:
        pos = struct.unpack_from('<f', g.data, child + 0x64)[0]
        if pos > y:
            struct.pack_into('<f', g.data, child + 0x64, pos + step)
    struct.pack_into('<f', g.data, row + 0x64, y + step)
    children.insert(children.index(kart) + 1, row)
    array = g.append(struct.pack(f'<{len(children)}Q', *children))
    for i in range(len(children)):
        g.add_site('ROFS', array + 8*i)
    g.set_u32(table + 12, len(children))
    g.set_u32(table + 16, len(children))
    g.set_u32(table + 24, 8*len(children))
    g.set_u64(table + 32, array)
    return g.build()


def style_signed_in_footer(blob):
    """Use the game's outlined font for the player label on the light footer."""
    g = Igz(blob)
    imports = g.fixup('EXNM')
    handles = [(g.strings[a], g.strings[b & 0x7fffffff])
               for a, b in struct.iter_unpack('<II', imports[3][:8 * imports[1]])
               if b & 0x80000000]
    placeable, = (p for p, kind in g.objects() if kind == 'igGuiPlaceable'
                  and p + 0x28 in g.sites['RSTT'] and g.string_at(p + 0x28) == 'CurrentUser')
    text, = (p for p, kind in g.objects() if kind == 'igGuiText'
             and g.string_at(p + 0x10) == 'SignedInUserText')
    original = 0x80000000 | handles.index(('GeekSpeak', 'GeekSpeak'))
    outlined = 0x80000000 | handles.index(('GeekSpeak_Outline', 'GeekSpeak_Outline'))
    if g.u64(text + 0x20) not in (original, outlined):
        raise ValueError('unrecognized signed-in footer typeface')
    g.set_u64(text + 0x20, outlined)
    struct.pack_into('<3f', g.data, placeable + 0xc0, 1, 1, 1)
    return g.build()
