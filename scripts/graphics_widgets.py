"""Native visual children for the Graphics list; no replacement VSC objects."""
import struct

from igz_file import unpack_offsets, pack_offsets


def add_widgets(g):
    deps = g.fixup('TDEP')
    # TDEP is packed NUL-terminated pairs, unlike the aligned TSTR table.
    names = [s.decode('utf-8') for s in deps[3].split(b'\0')[:deps[1] * 2]]
    if len(names) != deps[1] * 2 or any(not s for s in names):
        raise ValueError('invalid Graphics dependency table')
    for name, path in (
        ('SettingSelector', 'ui:/GuiSharedAssets/SettingSelector.igz'),
        ('Menu_ButtonWithIcon', 'ui:/GuiSharedAssets/Menu_ButtonWithIcon.igz'),
        ('Octane_LobbyMembers', 'ui:/Octane_LobbyMembers.igz'),
        ('Octane_Options_Menu_Page', 'ui:/Octane_Options_Menu_Page.igz'),
    ):
        if name not in names[::2]: names.extend((name, path))
    deps[1] = len(names)//2
    deps[3] = b''.join(n.encode()+b'\0' for n in names)
    deps[4] = (deps[2]+len(deps[3])+3)&~3
    handles = g.fixup('EXNM')
    entries = list(struct.iter_unpack('<II', handles[3][:handles[1] * 8]))
    sites = g.read_list('RHND', unpack_offsets)
    externals = g.read_list('RNEX', unpack_offsets)

    def handle(name, namespace):
        index = sum(bool(b & 0x80000000) for _, b in entries)
        entries.append((g.string_index(name), g.string_index(namespace) | 0x80000000))
        return index | 0x80000000

    def reference(at, name, namespace):
        index = sum(not (b & 0x80000000) for _,b in entries)
        entries.append((g.string_index(name),g.string_index(namespace)))
        g.set_u64(at,index)
        if at in g.sites['ROFS']: g.sites['ROFS'].remove(at)
        externals.append(at)

    def pointer(at, value):
        g.set_u64(at, value)
        if value:
            g.add_site('ROFS', at)
        elif at in g.sites['ROFS']:
            g.sites['ROFS'].remove(at)

    def placeable(asset, namespace, label, xy, size, scale=1.0):
        # Copy only a plain text placeable, never its animation/behavior objects.
        at = g.append(g.data[0x1b10:0x1c30])
        g.add_site('RVTB', at)
        for off in (0x28, 0x30):
            g.set_u64(at+off, g.string_index(label if off == 0x28 else ' '))
            g.add_site('RSTT', at+off)
        g.set_u64(at+0x38, handle(asset, namespace))
        sites.append(at+0x38)
        for off in (0x40, 0x48, 0x50, 0x58, 0xe8, 0xf0, 0xf8):
            g.set_u64(at+off, 0)
        struct.pack_into('<2f', g.data, at+0x60, *xy)
        struct.pack_into('<3f', g.data, at+0x84, scale, scale, 1)
        struct.pack_into('<2f', g.data, at+0x90, *size)
        struct.pack_into('<4f', g.data, at+0xc0, 1, 1, 1, 1)
        return at

    def children(parent, anchor, new):
        table = g.u64(parent+0x48)
        count = g.u32(table+0x10)
        old = g.u64(table+0x20)
        values = [g.u64(old+8*i) for i in range(count)] + new
        data = g.append(struct.pack('<'+'Q'*len(values), *values))
        for i in range(len(values)):
            g.add_site('ROFS', data+8*i)
        g.set_u32(table+0xc, len(values))
        g.set_u32(table+0x10, len(values))
        g.set_u32(table+0x18, len(values)*8)
        pointer(table+0x20, data)
        # Preserve the native traversal order (it differs from child-table order).
        successor = g.u64(anchor+0x50)
        chain = [anchor] + new
        for i, child in enumerate(new):
            pointer(child+0x40, parent)
            pointer(child+0x58, chain[i])
            pointer(chain[i]+0x50, child)
        pointer(new[-1]+0x50, successor)
        if successor:
            pointer(successor+0x58, new[-1])

    # Instantiate shared components exactly as native Gameplay/Options do.
    # No copied project, merged type table, or hand-built bar/arrow/title children.
    selector = 0x1520
    g.set_u64(selector+0x38, handle('SettingSelector_asset','SettingSelector'))
    pointer(selector+0x48,0)
    # The native traversal is postorder: remove the old visual subtree from
    # both the traversal chain and the language item's clone reference.
    pointer(0x13d0+0x50,selector)
    pointer(selector+0x58,0x13d0)
    pointer(0x4280+0x10,selector)
    g.set_u64(selector+0x28,g.string_index('BbGraphicsNativeSelector'))
    g.add_site('RSTT',selector+0x28)
    struct.pack_into('<3f',g.data,selector+0x60,350,0,0)
    struct.pack_into('<3f',g.data,selector+0x78,0,0,0)
    struct.pack_into('<2f',g.data,selector+0x90,450,150)
    struct.pack_into('<3f',g.data,selector+0x84,1,1,1)
    button = placeable('Menu_ButtonWithIcon_asset','Menu_ButtonWithIcon',
                       'BbGraphicsNativeRestart',(301,32),(616,128))
    g.set_u64(button+0x30,0)
    g.sites['RSTT'].remove(button+0x30)
    struct.pack_into('<3f',g.data,button+0x78,0,0,0)
    children(0x13d0, selector, [button])
    # Native navigation installs both icon states, as on the Graphics tab.
    nav = 0x42b8
    for field,name in ((0x148,'Sprite_octane_ui_raceselect_ico_crashcove_tga'),
                       (0x150,'Sprite_octane_ui_raceselect_ico_crashcove_tga001')):
        reference(nav+field,name,'Menu_ButtonWithIcon')
    # Navigation expects material handles, not igGuiSprite handles.
    material_ids = g.fixup('EXID')
    ready_material = material_ids[1]
    material_ids[3] = material_ids[3][:ready_material*8] + struct.pack('<II', 0x77c5b5ee, 0x3985f623)
    material_ids[1] += 1
    material_ids[4] = material_ids[2] + len(material_ids[3])
    for field in (0x158,0x178):
        g.set_u64(nav+field,ready_material)
        sites.append(nav+field)
    reference(nav+0x160,'AnimationTag_BTN_Deselected_Blue','Menu_ButtonWithIcon')
    reference(nav+0x168,'AnimationTag_BTN_LOCKED_Initialize','Menu_ButtonWithIcon')
    # Use the components' native focus animations; both actions may run while
    # one whole component is hidden for this row.
    for table,action,tag,button_tag in (
        (0x47f8,0x4828,'AnimationTag_Focus','AnimationTag_BTN_Selected_Blue'),
        (0x4760,0x4790,'AnimationTag_Defocus','AnimationTag_BTN_Deselected_Blue'),
    ):
        size = g.sizes[g.u64(action)]
        other = g.append(g.data[action:action+size])
        g.add_site('RVTB',other)
        reference(other+0x28,button_tag,'Menu_ButtonWithIcon')
        pointer(other+0x30,button)
        reference(action+0x28,tag,'SettingSelector')
        pointer(action+0x30,selector)
        data = g.append(struct.pack('<QQ',action,other))
        g.add_site('ROFS',data); g.add_site('ROFS',data+8)
        g.set_u32(table+0xc,2);g.set_u32(table+0x10,2)
        g.set_u32(table+0x18,16);pointer(table+0x20,data)
    description = placeable('ModeDescription', 'Octane_Options_Menu_Page',
                            'BbGraphicsDescription', (494, 950), (860, 96))
    children(0x3e0, 0x2c0, [description])
    struct.pack_into('<3f', g.data, description+0x78, 0, 0, 0)
    struct.pack_into('<f', g.data, description+0xe0, 1)
    # Match Gameplay's 140-unit selector pitch without extra list margins.
    struct.pack_into('<f', g.data, 0x13d0+0x94, 140)
    struct.pack_into('<4f', g.data, 0x1080+0x50, 150, 0, 0, 0)
    struct.pack_into('<f', g.data, 0x1080+0x4c, 0)
    handles[1] = len(entries)
    handles[3] = b''.join(struct.pack('<II', *e) for e in entries)
    handles[4] = (handles[2] + len(handles[3]) + 3) & ~3
    for tag, offsets in (('RHND',sites),('RNEX',externals)):
        f = g.fixup(tag)
        f[1],f[3] = len(offsets),pack_offsets(sorted(offsets))
        f[4] = (f[2]+len(f[3])+3)&~3
