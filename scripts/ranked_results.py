"""Local Ranked card on the native race ranking panel; no input behaviors."""
import struct
from igz_file import Igz, unpack_offsets
from graphics_tab import fnv1a
from hall_of_fame import dependencies, external, marker, append_traversal
from ranked_ui_assets import TIERS, tier_material


def score_card(blob, reward_blob):
    g, w = Igz(blob), Igz(reward_blob)
    kinds = dict(g.objects())
    if kinds.get(0x6a0) != 'igGuiPlaceable' or kinds.get(0x7cf0) != 'igGuiText':
        raise ValueError('unsupported native race ranking panel')
    for tag in ('RHND', 'RNEX'):
        g.sites[tag] = g.read_list(tag, unpack_offsets)
        w.sites[tag] = w.read_list(tag, unpack_offsets)

    def pointer(at, value):
        g.set_u64(at, value)
        if value: g.add_site('ROFS', at)
        elif at in g.sites['ROFS']: g.sites['ROFS'].remove(at)

    def clone(source):
        size = g.sizes[g.u64(source)]
        dest = g.append(g.data[source:source + size])
        for tag, sites in g.sites.items():
            for at in list(sites):
                if source <= at < source + size: g.add_site(tag, dest + at - source)
        return dest

    def import_block(start, end, remap):
        dest = g.append(w.data[start:end])
        def translate(value):
            if start <= value < end: return dest + value - start
            if value in remap: return remap[value]
            raise ValueError(f'unexpected reward pointer {value:x}')
        for tag, sites in w.sites.items():
            for at in sites:
                if not start <= at < end: continue
                new = dest + at - start
                g.add_site(tag, new)
                if tag == 'RVTB':
                    kind = w.types[w.u64(at)]
                    if kind not in g.types:
                        raise ValueError(f'reward type {kind} is unavailable in the score menu')
                    g.set_u64(new, g.types.index(kind))
                elif tag == 'RSTT': g.set_u64(new, g.string_index(w.string_at(at)))
                elif tag == 'ROFS': g.set_u64(new, translate(w.u64(at)))
                elif tag in ('RHND', 'RNEX'):
                    value = w.u64(at)
                    if tag == 'RNEX' or value & 0x80000000:
                        f = w.fixup('EXNM')
                        pairs = [(a,b) for a,b in struct.iter_unpack('<II', f[3][:f[1]*8]) if bool(b & 0x80000000) == (tag == 'RHND')]
                        a,b = pairs[value & 0x7fffffff]
                        value = external(g, w.strings[a], w.strings[b & 0x7fffffff], tag == 'RHND')
                    else:
                        f, target = w.fixup('EXID'), g.fixup('EXID')
                        raw = f[3][value*8:value*8+8]
                        value = target[1]
                        target[3] = target[3][:target[1]*8] + raw
                        target[1] += 1
                        target[4] = (target[2] + len(target[3]) + 3) & ~3
                    g.set_u64(new, value)
        return dest

    # Native Wumpa coin pop: play forward to shrink, reverse to grow.
    tags = {o: import_block(o, o + 0x18, {}) for o in (0x4a8, 0x4c0)}
    animations = []
    for reverse in (False, True):
        a = import_block(0x2a28, 0x2bd0, {**tags, 0x2bd0: 0})
        tracklist = a + 0x28
        g.set_u32(tracklist + 12, 1); g.set_u32(tracklist + 16, 1)
        g.set_u64(tracklist + 0x18, (g.u64(tracklist + 0x18) & ~0xffffffff) | 8)
        # Drop the native fade track; only the badge scales, never its panel.
        unused = a + 0x58
        if unused in g.sites['ROFS']: g.sites['ROFS'].remove(unused)
        g.set_u64(unused, 0)
        keys = [a + o - 0x2a28 for o in (0x2b08, 0x2b30, 0x2b58, 0x2b80, 0x2ba8)]
        values = [struct.unpack_from('<2f', g.data, key + 0x18) for key in keys]
        times = [g.u32(key + 12) for key in keys]
        if reverse:
            values.reverse(); times = [times[-1] - t for t in reversed(times)]
        for key, time, value in zip(keys, times, values):
            g.set_u32(key + 12, time)
            struct.pack_into('<2f', g.data, key + 0x18, *(v * 1.35 / .85 for v in value))
        pointer(a + 0x10, tags[0x4a8 if reverse else 0x4c0])
        struct.pack_into('<f', g.data, a + 0x20, times[-1] / 4096.0)
        animations.append(a)
    table = import_block(0x29c8, 0x2a08, {0x2a08: 0, 0x30a0: 0})
    values = g.append(struct.pack('<4Q', *animations, 0, 0))
    keys = g.append(struct.pack('<4Q', tags[0x4c0], tags[0x4a8], 0, 0))
    for at in (values, values+8, keys, keys+8): g.add_site('ROFS', at)
    pointer(table+0x18, values); pointer(table+0x28, keys)

    name_list = next(o for o,t in g.objects() if t == 'igNameList')
    assets = next(o for o,t in g.objects() if t == 'igGuiAssetList')
    def items(o): return [g.u64(g.u64(o+0x20)+i*8) for i in range(g.u32(o+12))]
    exports, asset_items = items(0), items(assets)
    names = bytearray(g.data[g.u64(name_list+0x20):g.u64(name_list+0x20)+len(exports)*16])
    def export(obj, name):
        exports.append(obj); asset_items.append(obj)
        names.extend(struct.pack('<QII', g.string_index(name), fnv1a(name), 0))
        return external(g, name, 'Octane_Race_Ranking', True)

    new = []
    def place(name, asset, x, y, width, height, scale=1):
        row = clone(0x4b70)
        marker(g, row, name)
        g.set_u64(row+0x38, asset)
        pointer(row+0x40, 0x6a0)
        for field in (0x48, 0x50, 0x58, 0xe8, 0xf0): pointer(row+field, 0)
        g.set_u32(row+12, (g.u32(row+12) | 0x400) & ~1)
        struct.pack_into('<3f', g.data, row+0x60, x, y, -10)
        struct.pack_into('<3f', g.data, row+0x6c, .5, .5, .5)
        struct.pack_into('<3f', g.data, row+0x78, 0, 0, 0)
        struct.pack_into('<3f', g.data, row+0x84, scale, scale, 1)
        struct.pack_into('<2f', g.data, row+0x90, width, height)
        struct.pack_into('<2I', g.data, row+0xa0, 0, 0)
        struct.pack_into('<4f', g.data, row+0xc0, 1, 1, 1, 1)
        struct.pack_into('<f', g.data, row+0xe0, 0)
        new.append(row)
        return row
    panel = place('OCTR_RESULT_PANEL', external(g, 'octane_ui_buttonlegend_bg_tga',
                  'Octane_Race_Ranking', True), 1595, 865, 610, 220)
    struct.pack_into('<4f', g.data, panel+0xc0, .08, .19, .32, .98)
    for name,asset,scale,color in (
            ('UP','ctr_gui_reward_vfx',.65,(1,.90,.50,.95)),
            ('DOWN','ctr_gui_wumpa_juiced_vfx',.45,(1,.45,.35,.72))):
        effect = place('OCTR_RESULT_FX_'+name, external(g,asset,'Octane_AAR_WumpaOnly',True),
                       1395,884,100,100,scale)
        g.set_u32(effect+12,g.u32(effect+12)|0x400000)
        struct.pack_into('<4f',g.data,effect+0xc0,*color)
    for mode, tier in TIERS:
        sprite = clone(0x7c30)
        material = tier_material(mode, tier)[2]
        f = g.fixup('EXID'); index = f[1]
        f[3] = f[3][:index*8] + struct.pack('<II', fnv1a(material), fnv1a('juice_Octane_LobbyMembers_materials'))
        f[1] += 1; f[4] = (f[2]+len(f[3])+3)&~3
        g.set_u64(sprite+0x20, index)
        badge = place(f'OCTR_RESULT_ICON_{mode}_{tier}', export(sprite, f'OCTR_RESULT_BADGE_{mode}_{tier}'), 1395, 884, 100, 100, 1.35)
        pointer(badge+0xf0, table)
    for name,y,height,font,source,color in (
            ('TITLE',832,24,.28,0x7cf0,(.82,.91,1,1)),
            ('DELTA',873,48,.60,0x7cf0,(1,1,1,1)),
            ('TIER',913,28,.34,0x7cf0,(1,1,1,1)),
            ('MMR',948,22,.26,0x7cf0,(.82,.91,1,1))):
        text = clone(source)
        struct.pack_into('<f',g.data,text+0x64,font)
        g.set_u32(text+0x60, 1)  # Center vertically inside the fitted text box.
        g.data[text+0x68] = 1
        g.data[text+0x69] = 0
        row = place('OCTR_RESULT_'+name, export(text,'OCTR_RESULT_TEXT_'+name),1680,y,350,height)
        struct.pack_into('<4f',g.data,row+0xc0,*color)
    def replace_list(o, values):
        data = g.append(struct.pack(f'<{len(values)}Q',*values))
        for i in range(len(values)): g.add_site('ROFS',data+i*8)
        g.set_u32(o+12,len(values));g.set_u32(o+16,len(values))
        g.set_u64(o+0x18,(g.u64(o+0x18)&~0xffffffff)|len(values)*8)
        pointer(o+0x20,data)
    children = g.u64(0x6a0+0x48)
    replace_list(children,items(children)+new)
    append_traversal(g,0x6a0,new)
    replace_list(0,exports);replace_list(assets,asset_items)
    data = g.append(names)
    for i in range(len(exports)):g.add_site('RSTT',data+i*16)
    g.set_u32(name_list+12,len(exports));g.set_u32(name_list+16,len(exports))
    g.set_u64(name_list+0x18,(g.u64(name_list+0x18)&~0xffffffff)|len(names))
    pointer(name_list+0x20,data)
    dependencies(g,{'juice_Octane_LobbyMembers_materials':'ui:/materialInstances/juice_Octane_LobbyMembers_materials.igz',
                    'Octane_AAR_WumpaOnly':'ui:/Octane_AAR_WumpaOnly.igz',
                    'ui_grandprix_bank':'sounds:/banks/ui_grandprix_bank.igz'})
    # ROFS animation references own their targets; copied counts describe the old graph.
    for obj, count in [(table, len(TIERS)), *((tag, 2) for tag in tags.values())]:
        g.set_u32(obj + 8, (g.u32(obj + 8) & ~0x7ffff) | count)
    return g.build()


def manual_navigation(blob):
    """Use native manual score navigation for Ranked; keep the online background."""
    g = Igz(blob)
    methods = g.fixup('EXNM')
    names = [g.strings[a] for a, b in struct.iter_unpack('<II', methods[3][:methods[1] * 8])
             if not b & 0x80000000]
    method = names.index('CNetworkNodes::isInNetworkGame2_VSC')
    # Only suppress automatic panel delays; keep End_Race_Flow and online controls native.
    nodes = (0x3418, 0x5d60)
    kinds = dict(g.objects())
    if any(kinds.get(o) != 'igVscCppActionNode' or g.u64(o + 0x38) != method or
           g.u32(o + 0x10) != 0 for o in nodes):
        raise ValueError('unsupported native race ranking navigation')
    pins = g.append(bytes(16))
    for node in nodes:
        g.set_u32(node + 0x10, 2)
        g.set_u32(node + 0x14, 2)
        g.set_u64(node + 0x18, (1 << 56) | (1 << 52) | 16)
        g.set_u64(node + 0x20, pins)
        g.add_site('ROFS', node + 0x20)
    return g.build()
