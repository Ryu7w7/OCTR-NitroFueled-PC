"""Add a fifth native Gameplay selector, using the existing boost-bar selector template."""
import struct
from igz_file import Igz, unpack_offsets

CAMERA_OPTION = 0x42424341


def gameplay_page(blob):
    g = Igz(blob)
    # USA/EU 1.21 share this object layout; reject another layout before touching it.
    objects = dict(g.objects())
    for at, kind in ((0x2910, "igGuiPlaceable"),
                     (0x3760, "CGuiBehaviorOctaneGlobalOptionSelector"),
                     (0x1c30, "igGuiPlaceableTable")):
        if objects.get(at) != kind:
            raise ValueError("unexpected Gameplay selector layout")
    for tag in ("RHND", "RNEX", "REXT", "RMHN", "RSTT"):
        if tag not in g.sites:
            try:
                g.sites[tag] = g.read_list(tag, unpack_offsets)
            except KeyError:
                pass
    # Gameplay's actual help text placeable is a native object in this page.
    if objects.get(0x1db0) == "igGuiPlaceable":
        g.set_u64(0x1db0 + 0x30,
                  g.string_index("Choose the default camera distance used when a race starts."))
        g.add_site("RSTT", 0x1db0 + 0x30)
    original_sites = {tag: list(sites) for tag, sites in g.sites.items()}
    ranges = [(0x2910, 0x2a30), (0x34b8, 0x3a10)]
    copies = [(a, b, g.append(g.data[a:b])) for a, b in ranges]

    def remap(value):
        return next((new + value - a for a, b, new in copies if a <= value < b), value)

    for tag, sites in original_sites.items():
        for site in sites:
            if any(a <= site < b for a, b in ranges):
                new = remap(site)
                g.add_site(tag, new)
                if tag == "ROFS":
                    g.set_u64(new, remap(g.u64(site)))

    def pointer(at, value):
        g.set_u64(at, value)
        g.add_site("ROFS", at)

    def string(at, value):
        g.set_u64(at, g.string_index(value))
        g.add_site("RSTT", at)

    row, selector = remap(0x2910), remap(0x3760)
    g.set_u32(selector + 0x1c8, CAMERA_OPTION)
    g.data[selector + 0x1cc] = 0  # Port owns this setting, never the game profile.
    options = remap(0x39d0)
    for off, value in ((0xc, 2), (0x10, 2), (0x18, 16)):
        g.set_u32(options + off, value)
    string(remap(0x39f8), "Near")
    string(remap(0x3a00), "Far")
    # Insert into both the child table and native sibling/navigation rings.
    table = 0x1c30
    count, old = g.u32(table + 0x10), g.u64(table + 0x20)
    values = [g.u64(old + 8*i) for i in range(count)] + [row]
    data = g.append(struct.pack("<" + "Q"*len(values), *values))
    for i in range(len(values)):
        g.add_site("ROFS", data + 8*i)
    for off, value in ((0xc, len(values)), (0x10, len(values)), (0x18, 8*len(values))):
        g.set_u32(table + off, value)
    pointer(table + 0x20, data)
    pointer(0x1b10 + 0x50, row)
    pointer(row + 0x58, 0x1b10)
    pointer(row + 0x50, 0x2b50)
    pointer(0x2b50 + 0x58, row)
    pointer(selector + 0x110, 0x2b50)
    pointer(selector + 0x118, 0x1c90)
    pointer(0x2ca8 + 0x118, row)
    pointer(0x3a48 + 0x110, row)
    # The native page leaves room for the help panel below the list.  Place the
    # five rows on that native vertical rhythm; without this, the appended row
    # lands over the description area.
    for i, placeable in enumerate((0x1c90, 0x2910, 0x2a30, 0x2b50, row)):
        struct.pack_into("<2f", g.data, placeable + 0x60, -10 + 11*i, -380 + 110*i)
    struct.pack_into("<f", g.data, row + 0x64, 60.0)
    for sites in g.sites.values():
        sites.sort()
    return g.build()
