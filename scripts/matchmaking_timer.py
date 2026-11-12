"""Passive search timer using the native lobby countdown's anchor and artwork."""
import math
import struct
from collections import Counter
from igz_file import Igz, unpack_offsets
from hall_of_fame import append_traversal, dependencies, external, marker
from graphics_tab import fnv1a


def add_timer(blob, timer_blob):
    g, native = Igz(blob), Igz(timer_blob)
    for image in (g, native):
        for tag in ('RHND', 'RNEX'):
            image.sites[tag] = image.read_list(tag, unpack_offsets)
    kinds = dict(native.objects())
    if kinds.get(0x140) != 'igGuiPlaceable' or native.string_at(0x4d0+0x28) != 'TimerValue':
        raise ValueError('unsupported native lobby countdown')
    parent = next(o for o,t in g.objects() if t=='igGuiPlaceable' and o+0x28 in g.sites['RSTT']
                  and g.string_at(o+0x28)=='OCTR_PUBLIC_LOBBY')
    first_new = len(g.data)
    old_counts=Counter(g.u64(at) for at in g.sites['ROFS'])
    null_fields = {0x30,0x38,0x40,0x48,0x50,0x58,0xe8,0xf0,0xf8}

    def pointer(at, value):
        g.set_u64(at,value)
        if value:g.add_site('ROFS',at)
        elif at in g.sites['ROFS']:g.sites['ROFS'].remove(at)

    def clone(source, placeable=False):
        kind = native.types[native.u64(source)]
        size = native.sizes[native.u64(source)]
        if kind not in g.types or g.sizes[g.types.index(kind)]!=size:
            raise ValueError('native timer object layout mismatch')
        dest=g.append(native.data[source:source+size])
        for tag,sites in native.sites.items():
            for at in sites:
                if not source<=at<source+size:continue
                field=at-source
                if placeable and field in null_fields:continue
                target=dest+field
                g.add_site(tag,target)
                if tag=='RVTB':g.set_u64(target,g.types.index(kind))
                elif tag=='RSTT':g.set_u64(target,g.string_index(native.string_at(at)))
                elif tag=='RHND':
                    value=native.u64(at)
                    if value&0x80000000:
                        table=native.fixup('EXNM')
                        handles=[(a,b) for a,b in struct.iter_unpack('<II',table[3][:table[1]*8]) if b&0x80000000]
                        a,b=handles[value&0x7fffffff]
                        value=external(g,native.strings[a],native.strings[b&0x7fffffff],True)
                    else:
                        old,new=native.fixup('EXID'),g.fixup('EXID')
                        raw=old[3][value*8:value*8+8]
                        value=new[1];new[1]+=1
                        new[3]=new[3][:value*8]+raw;new[4]=(new[2]+len(new[3])+3)&~3
                    g.set_u64(target,value)
                else:raise ValueError('unexpected native timer reference: '+tag)
        if placeable:
            for field in null_fields:g.set_u64(dest+field,0)
            g.set_u32(dest+12,g.u32(dest+12)&~0x400)
        return dest

    def replace_list(obj, values):
        array=g.append(struct.pack('<'+'Q'*len(values),*values))
        for i in range(len(values)):g.add_site('ROFS',array+8*i)
        g.set_u32(obj+12,len(values));g.set_u32(obj+16,len(values))
        g.set_u64(obj+24,(g.u64(obj+24)&~0xffffffff)|len(values)*8)
        pointer(obj+32,array)

    asset_list=next(o for o,t in g.objects() if t=='igGuiAssetList')
    name_list=next(o for o,t in g.objects() if t=='igNameList')
    exports=[g.u64(g.u64(32)+8*i) for i in range(g.u32(12))]
    assets=[g.u64(g.u64(asset_list+32)+8*i) for i in range(g.u32(asset_list+12))]
    names=bytearray(g.data[g.u64(name_list+32):g.u64(name_list+32)+16*len(exports)])
    def export(obj,name):
        exports.append(obj);assets.append(obj)
        names.extend(struct.pack('<QII',g.string_index(name),fnv1a(name),0))
        return external(g,name,'Octane_Lobby_Matchmaking',True)

    panel=clone(0x140,True);marker(g,panel,'OCTR_SEARCH_PANEL');pointer(panel+0x40,parent)
    g.set_u32(panel+12,g.u32(panel+12)|0x400)
    background=clone(0x3b0,True);marker(g,background,'OCTR_SEARCH_BACKGROUND');pointer(background+0x40,panel)
    sprite=clone(0x12e0)
    g.set_u64(background+0x38,export(sprite,'OCTR_SEARCH_BACKGROUND_ASSET'));g.add_site('RHND',background+0x38)
    children=[background]
    angle=struct.unpack_from('<f',g.data,background+0x80)[0]
    radians=math.radians(angle)
    for tag,source,offset,font in [('TITLE',0x5f0,-36,.5),('TIME',0x4d0,0,.72),('RANGE',0x5f0,36,.35)]:
        text=clone(0x11f0)
        g.set_u32(text+0x5c,1);g.set_u32(text+0x60,1)
        struct.pack_into('<f',g.data,text+0x64,font)
        g.data[text+0x68]=1;g.data[text+0x69]=0
        label=clone(source,True);marker(g,label,'OCTR_SEARCH_'+tag);pointer(label+0x40,panel)
        # Native timer labels use center anchors; the artwork uses top-left anchors.
        g.data[label+0x98:label+0xb0]=g.data[background+0x98:background+0xb0]
        struct.pack_into('<3f',g.data,label+0x78,0,0,angle)
        struct.pack_into('<2f',g.data,label+0x60,-39-math.sin(radians)*offset,84+math.cos(radians)*offset)
        struct.pack_into('<2f',g.data,label+0x90,520,70)
        g.set_u64(label+0x38,export(text,'OCTR_SEARCH_'+tag+'_ASSET'));g.add_site('RHND',label+0x38)
        children.append(label)
    table=g.append(g.data[g.u64(parent+0x48):g.u64(parent+0x48)+g.sizes[g.u64(g.u64(parent+0x48))]])
    g.add_site('RVTB',table)
    replace_list(table,children);pointer(panel+0x48,table)
    old_table=g.u64(parent+0x48)
    old_children=[g.u64(g.u64(old_table+32)+8*i) for i in range(g.u32(old_table+12))]
    replace_list(old_table,old_children+[panel]);append_traversal(g,parent,[panel,*children])
    replace_list(0,exports);replace_list(asset_list,assets)
    array=g.append(names)
    for i in range(len(exports)):g.add_site('RSTT',array+16*i)
    g.set_u32(name_list+12,len(exports));g.set_u32(name_list+16,len(exports))
    g.set_u64(name_list+24,(g.u64(name_list+24)&~0xffffffff)|len(names));pointer(name_list+32,array)
    counts=Counter(g.u64(at) for at in g.sites['ROFS'])
    for obj,kind in g.objects():
        count=counts[obj] if obj>=first_new else (g.u32(obj+8)&0x7ffff)+counts[obj]-old_counts[obj]
        g.set_u32(obj+8,(g.u32(obj+8)&~0x7ffff)|count)
    dependencies(g,{'juice_Octane_Lobby_Timer_materials':'ui:/materialInstances/juice_Octane_Lobby_Timer_materials.igz'})
    return g.build()
