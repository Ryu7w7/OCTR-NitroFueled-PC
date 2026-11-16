"""Pack PNG art into creator-compiled CTR UI textures."""
import struct
from pathlib import Path

from button_prompts import (IMG_DATA, IMG_W, _objects, _sections_and_fixups,
                            _strings, read_png, swap_red_blue, tile)


ASSETS = Path(__file__).resolve().parent / 'assets'
SINGLE_RACE_TEXTURE = ('textures/GuiStandard_diffuse,'
                       'textures@!ui!Octane!Icons!octane_ui_ico_singlerace`tga,101.igz')


def _replace_texture_name(blob, name):
    sections, fixups = _sections_and_fixups(blob)
    strings = _strings(*fixups['TSTR'])
    strings[0] = name
    data = bytearray()
    for value in strings:
        data += value.encode() + b'\0'
        if len(data) & 1:
            data += b'\0'
    data += bytes((-len(data)) & 3)
    block = struct.pack('<4sIII', b'TSTR', len(strings), 16 + len(data), 16) + data

    fixup_at, fixup_length = sections[0]
    old_length = struct.unpack_from('<I', blob, fixup_at + 8)[0]
    fixups_blob = block + blob[fixup_at + old_length:fixup_at + fixup_length]
    next_alignment = struct.unpack_from('<I', blob, 0x14 + 16 + 12)[0]
    fixups_blob += bytes((-len(fixups_blob)) % next_alignment)
    delta = len(fixups_blob) - fixup_length
    result = bytearray(blob[:fixup_at] + fixups_blob + blob[fixup_at + fixup_length:])
    struct.pack_into('<I', result, 0x14 + 8, len(fixups_blob))
    for section in range(1, len(sections)):
        offset_at = 0x14 + section * 16 + 4
        struct.pack_into('<I', result, offset_at,
                         struct.unpack_from('<I', result, offset_at)[0] + delta)
    return bytes(result)


def texture_igz(template, png, image_name, black=False, texture_name=None):
    """Put PNG into HUD Customizer's creator-compiled RGBA8 texture IGZ."""
    icon_width, icon_height, rgba = read_png(png)
    if icon_width != icon_height:
        raise ValueError(f'{png}: expected square RGBA art')
    # The sprite samples the texture bottom-up, so pack the rows upside-down.
    rgba = b''.join(rgba[y * icon_width * 4:(y + 1) * icon_width * 4]
                    for y in range(icon_height - 1, -1, -1))
    if black:
        rgba = bytes(0 if i % 4 != 3 else value for i, value in enumerate(rgba))
    objects, _strings_list, file_offset = _objects(template)
    images = [at for at, kind in objects if kind == 'igImage2']
    if len(images) != 1:
        raise ValueError('tier texture template must contain one image')
    image_at = images[0]
    width, height = struct.unpack_from('<HH', template, image_at + IMG_W)
    packed, data = struct.unpack_from('<QQ', template, image_at + IMG_DATA)
    length = packed & 0xffffffff
    expected = ((width + 7) // 8 * 8) * ((height + 7) // 8 * 8) * 4
    if length != expected:
        raise ValueError('tier texture template is not creator RGBA8')

    side = min(width, height)
    linear = bytearray(width * height * 4)
    for y in range(side):
        source_y = y * icon_height // side
        for x in range(side):
            source_x = x * icon_width // side
            source = (source_y * icon_width + source_x) * 4
            target = (y * width + x + (width - side) // 2) * 4
            linear[target:target + 4] = rgba[source:source + 4]
    at = file_offset(data)
    result = template[:at] + tile(swap_red_blue(linear), width, height,
                                  template[at:at + length]) + template[at + length:]
    desired = texture_name or 'data/Octane/textures/ui/Octane/GPrix/' + image_name[1:-1]
    return _replace_texture_name(result, desired)
