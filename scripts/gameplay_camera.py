"""Extend the shipped 1.21 Gameplay page with the same native selector as Reserves."""
import struct
from igz_file import unpack_offsets

CAMERA_OPTION = 0x43414D
TEXT = {
    'en': ('Camera', 'Default', 'Far', 'Choose the starting race camera. You can still switch views during the race.'),
    'mx': ('Cámara', 'Normal', 'Lejos', 'Elige la cámara al iniciar la carrera. Puedes cambiarla durante la carrera.'),
    'es': ('Cámara', 'Normal', 'Lejos', 'Elige la cámara al iniciar la carrera. Puedes cambiarla durante la carrera.'),
    'fr': ('Caméra', 'Par défaut', 'Éloignée', 'Choisissez la vue de départ. Vous pouvez changer de vue en course.'),
    'pt': ('Câmera', 'Padrão', 'Longe', 'Escolha a câmera inicial. Você pode mudar a visão durante a corrida.'),
    'de': ('Kamera', 'Standard', 'Fern', 'Wähle die Startansicht. Du kannst die Ansicht im Rennen wechseln.'),
    'it': ('Telecamera', 'Predefinita', 'Lontana', 'Scegli la visuale iniziale. Puoi cambiarla durante la gara.'),
}

def add_camera(g, column):
    # These objects are the self-contained Reserves selector in the pinned Pack Tacho page.
    row, nav, toggle, end = 0x17360, 0x174b8, 0x176d8, 0x17830
    kinds = dict(g.objects())
    if (kinds.get(row) != 'igGuiPlaceable' or kinds.get(nav) != 'CGuiBehaviorOctaneNavigationItem'
            or kinds.get(toggle) != 'CGuiBehaviorGlobalToggleOption' or g.u32(toggle+0xdc) != 12):
        raise ValueError('unexpected Gameplay reserves selector')
    parent = g.u64(row+0x40)
    table = g.u64(parent+0x48)
    data = g.u64(table+0x20)
    children = [g.u64(data+i*8) for i in range(g.u32(table+0x10))]
    successor = g.u64(row+0x50)
    if row not in children or g.u64(successor+0x58) != row:
        raise ValueError('unexpected Gameplay navigation links')

    shift = g.append(g.data[row:end]) - row
    # Clone pointers, external handles, strings and type fixups. Only references
    # internal to the selector move. Resources, descriptions and parent are shared.
    for name in ('RHND', 'RNEX'):
        g.sites[name] = g.read_list(name, unpack_offsets)
    for name, offsets in g.sites.items():
        for at in [off for off in offsets if row <= off < end]:
            new = at+shift
            g.add_site(name, new)
            if name == 'ROFS' and row <= g.u64(at) < end:
                g.set_u64(new, g.u64(at)+shift)
    new = row+shift
    g.set_u64(new+0x58,row)
    g.set_u64(row+0x50,new)
    g.set_u64(successor+0x58,new)
    previous = g.u64(nav+0x110)
    y = struct.unpack_from('<f',g.data,row+0x64)[0]
    pitch = y - struct.unpack_from('<f',g.data,previous+0x64)[0]
    # Make room above the existing help separator. All six original controls
    # share the Reserves widget resource; the separator and hint have their own.
    widget = g.u32(row + 0x38)
    for child in children:
        if g.u32(child + 0x38) == widget:
            position = struct.unpack_from('<f', g.data, child + 0x64)[0]
            struct.pack_into('<f', g.data, child + 0x64, position - pitch)
    # The cloned Camera row already occupies the old Reserves position.
    g.set_u64(nav+shift+0x110,row)
    g.set_u64(nav+0x118,new)
    for off, kind in kinds.items():
        if kind == 'CGuiBehaviorOctaneNavigationItem' and g.u64(off+0x110) == row:
            g.set_u64(off+0x110,new)
    children.append(new)
    data = g.append(b''.join(struct.pack('<Q', item) for item in children))
    g.set_u64(table+0x20,data)
    for i in range(len(children)): g.add_site('ROFS',data+i*8)
    for offset in (12,16): g.set_u32(table+offset,len(children))
    g.set_u32(table+24,len(children)*8)
    title, default, far, hint = TEXT.get(column,TEXT['en'])
    for offset, value in ((nav+0xe8,title),(nav+0x1b8,hint),(toggle+0xa8,far),(toggle+0xb0,default)):
        g.set_u64(offset+shift,g.string_index(value))
    g.set_u32(toggle+shift+0xdc,CAMERA_OPTION)
