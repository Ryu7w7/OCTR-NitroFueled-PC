"""Prebuilt badge siblings for native Online result nicknames."""
import struct
from igz_file import Igz, unpack_offsets
from hall_of_fame import append_traversal, dependencies, external, marker
from ranked_ui_assets import TIERS, tier_material
from graphics_tab import fnv1a


def add_badges(blob, project):
    g = Igz(blob)
    for tag in ('RHND','RNEX'):
        g.sites[tag] = g.read_list(tag,unpack_offsets)
    objects = dict(g.objects())
    if project not in g.strings:raise ValueError('native GUI namespace mismatch')
    names = [o for o,t in objects.items() if t == 'igGuiPlaceable' and
             o+0x28 in g.sites['RSTT'] and g.string_at(o+0x28) in
             ('[PlayerTagText]',)]
    if not names:
        raise ValueError('native race/podium nickname fields missing')
    def pointer(at,value):
        g.set_u64(at,value)
        if value:g.add_site('ROFS',at)
        elif at in g.sites['ROFS']:g.sites['ROFS'].remove(at)
    def clone(source):
        size = g.sizes[g.u64(source)]
        off = g.append(g.data[source:source+size])
        for tag,sites in g.sites.items():
            for at in list(sites):
                if source <= at < source+size:g.add_site(tag,off+at-source)
        return off
    sprite_template = next(o for o,t in objects.items() if t=='igGuiSprite')
    asset_list = next(o for o,t in objects.items() if t=='igGuiAssetList')
    name_list = next(o for o,t in objects.items() if t=='igNameList')
    exports = [g.u64(g.u64(32)+8*i) for i in range(g.u32(12))]
    assets = [g.u64(g.u64(asset_list+32)+8*i) for i in range(g.u32(asset_list+12))]
    entries = bytearray(g.data[g.u64(name_list+32):g.u64(name_list+32)+16*len(exports)])
    named = {g.string_at(g.u64(name_list+32)+16*i):exports[i] for i in range(len(exports))}
    handles = {}
    for mode,tier in TIERS:
        sprite = clone(sprite_template)
        g.set_u32(sprite+0x30,0)
        struct.pack_into('<2f',g.data,sprite+0x34,0,0)
        ids = g.fixup('EXID')
        index = ids[1]
        ids[3] = ids[3][:index*8]+struct.pack('<II',fnv1a(tier_material(mode,tier)[2]),
                                          fnv1a('juice_Octane_LobbyMembers_materials'))
        ids[1]+=1;ids[4]=(ids[2]+len(ids[3])+3)&~3
        g.set_u64(sprite+0x20,index)
        export = f'OCTR_NAME_RANK_{mode}_{tier}'
        exports.append(sprite);assets.append(sprite)
        entries.extend(struct.pack('<QII',g.string_index(export),fnv1a(export),0))
        handles[mode,tier]=external(g,export,project,True)
    def replace_list(obj,values):
        array=g.append(struct.pack('<'+'Q'*len(values),*values))
        for i in range(len(values)):g.add_site('ROFS',array+8*i)
        g.set_u32(obj+12,len(values));g.set_u32(obj+16,len(values))
        g.set_u64(obj+24,(g.u64(obj+24)&~0xffffffff)|(8*len(values)))
        pointer(obj+32,array)
    replace_list(0,exports);replace_list(asset_list,assets)
    array=g.append(entries)
    for i in range(len(exports)):g.add_site('RSTT',array+16*i)
    g.set_u32(name_list+12,len(exports));g.set_u32(name_list+16,len(exports))
    g.set_u64(name_list+24,(g.u64(name_list+24)&~0xffffffff)|len(entries))
    pointer(name_list+32,array)
    for name in names:
        f=g.fixup('EXNM')
        own=[(g.strings[a],g.strings[b&0x7fffffff]) for a,b in
             struct.iter_unpack('<II',f[3][:f[1]*8]) if b&0x80000000]
        handle=g.u64(name+0x38)
        asset,namespace=own[handle&0x7fffffff]
        text=named.get(asset) if namespace==project else None
        if text is None or objects.get(text)!='igGuiText':
            raise ValueError('nickname text must use a local native text asset')
        g.data[text+0x68]=1;g.data[text+0x69]=0
        parent = g.u64(name+0x40)
        table = g.u64(parent+0x48)
        if not table:raise ValueError('nickname parent has no child table')
        items = [g.u64(g.u64(table+32)+i*8) for i in range(g.u32(table+12))]
        added = []
        for mode,tier in TIERS:
            icon = clone(name)
            marker(g,icon,f'OCTR_NAME_ICON_{mode}_{tier}')
            g.set_u64(icon+0x38,handles[mode,tier])
            for field in (0x30,0x48,0x50,0x58,0xe8,0xf0):pointer(icon+field,0)
            # A nickname can have a localized display string; sprites never own one.
            for tag in ('RSTT','RHND','RNEX'):
                if icon+0x30 in g.sites[tag]:g.sites[tag].remove(icon+0x30)
            g.set_u32(icon+12,g.u32(icon+12)|0x400)
            struct.pack_into('<3f',g.data,icon+0x84,.32,.32,1)
            struct.pack_into('<3f',g.data,icon+0x6c,.5,.5,.5)
            struct.pack_into('<2f',g.data,icon+0x90,100,100)
            struct.pack_into('<4f',g.data,icon+0xc0,1,1,1,1)
            struct.pack_into('<f',g.data,icon+0xe0,0)
            added.append(icon)
        array = g.append(struct.pack('<'+'Q'*(len(items)+len(added)),*(items+added)))
        for i in range(len(items)+len(added)):g.add_site('ROFS',array+8*i)
        g.set_u32(table+12,len(items)+len(added));g.set_u32(table+16,len(items)+len(added))
        g.set_u64(table+24,(g.u64(table+24)&~0xffffffff)|((len(items)+len(added))*8))
        pointer(table+32,array)
        append_traversal(g,parent,added)
    dependencies(g,{'juice_Octane_LobbyMembers_materials':
                    'ui:/materialInstances/juice_Octane_LobbyMembers_materials.igz'})
    return g.build()
