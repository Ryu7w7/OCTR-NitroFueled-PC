"""N.Sane Mode HUD: a hidden second item slot in ui/GuiSharedAssets/Octane_PowerUp.igz.

The slot is added to every view of the item HUD (scripts/speed_mode.py) and stays hidden; in a private
N.Sane race src/runtime_nsane.c shows it with the stored second item's icon. Outside N.Sane nothing
changes on screen.

    a background (a copy of the item background, without scripts), smaller and beside the item slot
    13 icons under it, one per item type (0 Bomb .. 12 Invisibility), all hidden (flag 0x400, alpha 0)
    marker: pivot z (+0x74) = 1000 + type for the icons, 1013 for the background (no visual effect)

igGuiPlaceable: +0x38 asset handle (0x80000000 | k = the k-th EXNM entry whose namespace has the high bit),
+0x40 parent, +0x48 children table (+0x0c count, +0x10 capacity, +0x18 bytes, +0x20 -> entries),
+0x50 / +0x58 draw order next / previous, +0x60 position, +0x74 pivot z, +0x84 scale, +0xc flags,
+0xe0 alpha, +0xe8 behaviors, +0xf0 animations.
"""
import struct

from igz_file import Igz, unpack_offsets

ICONS = ('bomb', 'missile', 'tnt', 'beaker_green', 'mask_aku', 'clock', 'turbo', 'warp', 'shield_green',
         'bomb_3', 'missile_3', 'super_engines', 'invisibility')
MARK = 1000.0
SLOT_OFFSET = (130.0, 40.0)   # from the item background (its position, root-relative); was (160, 45)
SLOT_SCALE = 0.22             # the item background is 0.4


def _exnm(g):
    f = g.fixup('EXNM')
    return f, [struct.unpack_from('<2I', f[3], 8 * i) for i in range(f[1])]


def _set_exnm(g, f, entries):
    raw = b''.join(struct.pack('<2I', *e) for e in entries)
    f[1], f[3] = len(entries), raw
    length = f[2] + len(raw)
    f[4] = length + (-length) % 4      # Igz.build keeps an untouched block's length: give it the new one


def _handle_names(g, entries):
    """Handle value -> the asset name it refers to (only high-bit namespaces are handles)."""
    names, k = {}, 0
    for name, space in entries:
        if space & 0x80000000:
            names[0x80000000 | k] = g.strings[name]
            k += 1
    return names


def add_second_slot(blob):
    g = Igz(blob)
    for tag in ('RHND', 'RNEX'):
        if tag not in g.sites:
            g.sites[tag] = g.read_list(tag, unpack_offsets)
    f, entries = _exnm(g)
    names = _handle_names(g, entries)
    placeables = [(off, g.sizes[g.u64(off)]) for off, kind in g.objects() if kind == 'igGuiPlaceable']

    def by_asset(asset):
        hits = [(off, size) for off, size in placeables if names.get(g.u32(off + 0x38)) == asset]
        if len(hits) != 1:
            raise ValueError(f'N.Sane HUD: item slot asset {asset} not recognized')
        return hits[0]

    bg, bg_size = by_asset('powerups_background_tga')
    icon, icon_size = by_asset('ico_powerups_beaker_green_tga')
    root = g.u64(bg + 0x40)
    if g.u64(icon + 0x40) != bg or not root:
        raise ValueError('N.Sane HUD: item slot layout not recognized')
    if any(MARK <= struct.unpack_from('<f', g.data, off + 0x74)[0] <= MARK + 13 for off, _ in placeables):
        raise ValueError('N.Sane HUD: the slot is already there')
    space = next(s for _, s in entries if s & 0x80000000 and g.strings[s & 0x7fffffff] == 'Octane_PowerUp')

    def handle(asset):
        entries.append((g.string_index(asset), space))
        return 0x80000000 | (sum(1 for _, s in entries if s & 0x80000000) - 1)

    def clone(off, size):
        new = g.append(bytes(g.data[off:off + size]))
        for tag, sites in g.sites.items():
            for site in [s for s in sites if off <= s < off + size]:
                g.add_site(tag, new + site - off)
        g.set_u32(new + 8, 1)
        return new

    def drop(site):
        g.set_u64(site, 0)
        if site in g.sites['ROFS']:
            g.sites['ROFS'].remove(site)

    def pointer(site, value):
        g.set_u64(site, value)
        g.add_site('ROFS', site)

    def table(children):
        old = g.u64(root + 0x48)
        new = g.append(bytes(g.data[old:old + 0x28]) + bytes(8 * len(children)))
        if old in g.sites['RVTB']:
            g.add_site('RVTB', new)
        g.set_u32(new + 8, 1)
        n = len(children)
        g.set_u32(new + 0x0c, n)
        g.set_u32(new + 0x10, n)
        g.set_u32(new + 0x18, 8 * n)
        pointer(new + 0x20, new + 0x28)
        for i, child in enumerate(children):
            pointer(new + 0x28 + 8 * i, child)
        return new

    def hide(p, marker):
        struct.pack_into('<f', g.data, p + 0x74, marker)
        struct.pack_into('<f', g.data, p + 0xe0, 0.0)
        g.set_u32(p + 0x0c, g.u32(p + 0x0c) | 0x400)
        drop(p + 0xe8)
        drop(p + 0xf0)

    slot = clone(bg, bg_size)
    pointer(slot + 0x40, root)
    x, y = struct.unpack_from('<ff', g.data, bg + 0x60)
    struct.pack_into('<ff', g.data, slot + 0x60, x + SLOT_OFFSET[0], y + SLOT_OFFSET[1])
    struct.pack_into('<ff', g.data, slot + 0x84, SLOT_SCALE, SLOT_SCALE)
    hide(slot, MARK + 13)
    icons = []
    for kind, name in enumerate(ICONS):
        p = clone(icon, icon_size)
        pointer(p + 0x40, slot)
        g.set_u64(p + 0x38, handle(f'ico_powerups_{name}_tga'))
        g.add_site('RHND', p + 0x38)
        hide(p, MARK + kind)
        drop(p + 0x48)
        icons.append(p)
    pointer(slot + 0x48, table(icons))
    old = g.u64(root + 0x48)
    kids = [g.u64(g.u64(old + 0x20) + 8 * i) for i in range(g.u32(old + 0x0c))]
    pointer(root + 0x48, table(kids + [slot]))
    # draw order: item icon -> slot background -> its icons -> what followed the item icon
    after = g.u64(icon + 0x50)
    chain = [icon, slot] + icons + ([after] if after else [])
    for a, b in zip(chain, chain[1:]):
        pointer(a + 0x50, b)
        pointer(b + 0x58, a)
    if not after:
        drop(chain[-1] + 0x50)
    _set_exnm(g, f, entries)
    return g.build()
