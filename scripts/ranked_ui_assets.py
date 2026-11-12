"""Ranked tier, menu, texture, and animated material UI assets."""
import struct
from pathlib import Path

from graphics_tab import fnv1a
from igz_file import Igz, unpack_offsets
from ui_texture import ASSETS, IMG_DATA, IMG_W, SINGLE_RACE_TEXTURE, _objects, single_race_texture, texture_igz as _texture_igz


ART = Path(__file__).resolve().parent / 'tier_icon'
TIERS = tuple((mode, tier) for mode in range(2) for tier in range(0, 8))
QUICK_ICON_TEXTURE = ('textures/GuiStandard_diffuse,'
                      'textures@!ui!Octane!Icons!octane_ui_ico_quick`tga,101.igz')
RANKED_BACKGROUND_MATERIAL = 'octane_ui_online_bg_pattern_ranked_tga__material'
RANKED_BACKGROUND_NAMESPACE = 'juice_Octane_Lobby_Main_materials'
RANKED_BACKGROUND_PATH = ('ui/materialInstances/' +
                          RANKED_BACKGROUND_NAMESPACE + '.igz')
RANKED_BACKGROUND_IMAGE = ('GuiStandard_diffuse,textures@!ui!Octane!Background!'
                           'octane_ui_online_bg_pattern_ranked`tga,101')
RANKED_BACKGROUND_TEXTURE = 'textures/' + RANKED_BACKGROUND_IMAGE + '.igz'
HOF_BACKGROUND_MATERIAL = 'octane_ui_online_bg_pattern_hof_tga__material'
HOF_BACKGROUND_IMAGE = ('GuiStandard_diffuse,textures@!ui!Octane!Background!'
                        'octane_ui_online_bg_pattern_hof`tga,101')
HOF_BACKGROUND_TEXTURE = 'textures/' + HOF_BACKGROUND_IMAGE + '.igz'
HOF_ICON_MATERIAL = 'octane_ui_ico_hof_tga__material'
def _template(name):
    for base in (ASSETS, ART):
        p = base / name
        if p.exists(): return p
    return ASSETS / name



def tier_icon(mode, tier):
    """(PNG path, sprite image name, archive texture path) for one rank."""
    if (mode, tier) not in TIERS:
        raise ValueError('ranked tier must be mode 0..1 and tier 0..7')
    image = f'octane_ranked_tier_{mode}_{tier}'.ljust(34, '_')
    name = f'{"itemless" if mode else "normal"}_{tier}.png'
    png = ASSETS / name
    if not png.exists():
        png = ART / name
    if not tier:
        png = ASSETS / 'unranked.png'  # Unranked players get the neutral badge.
    return (png,
            f'[{image}.tga]',
            f'textures/guistandard_diffuse,textures@!ui!octane!gprix!{image}`tga,001.igz')


def tier_material(mode, tier):
    image = tier_icon(mode, tier)[1][1:-5]
    namespace = f'juice_OCTR_Ranked_{mode}_{tier}_materials'
    return (namespace,
            f'ui/materialInstances/octr/{namespace}.igz',
            image + '_tga__material')


def tier_shadow(mode, tier):
    image = f'octane_ranked_shadow_{mode}_{tier}'.ljust(34, '_')
    namespace = f'juice_OCTR_RankedShadow_{mode}_{tier}_materials'
    return (f'[{image}.tga]',
            f'textures/guistandard_diffuse,textures@!ui!octane!gprix!{image}`tga,001.igz',
            namespace, f'ui/materialInstances/octr/{namespace}.igz',
            image + '_tga__material')


def texture_igz(template, mode, tier):
    png, image_name, _path = tier_icon(mode, tier)
    objects, _, file_offset = _objects(template)
    image = next(off for off, kind in objects if kind == 'igImage2')
    width, height = struct.unpack_from('<HH', template, image + IMG_W)
    if (width, height) == (100, 100):
        packed, data = struct.unpack_from('<QQ', template, image + IMG_DATA)
        old_length = packed & 0xffffffff
        new_length = 256 * 256 * 4
        if file_offset(data) + old_length != len(template):
            raise ValueError('tier texture payload is not final section')
        enlarged = bytearray(template)
        struct.pack_into('<HH', enlarged, image + IMG_W, 256, 256)
        struct.pack_into('<Q', enlarged, image + IMG_DATA, (packed & ~0xffffffff) | new_length)
        struct.pack_into('<I', enlarged, 0x14 + 2 * 16 + 8, new_length)
        enlarged.extend(bytes(new_length - old_length))
        template = bytes(enlarged)
    elif (width, height) != (256, 256):
        raise ValueError('unsupported tier texture template size')
    return _texture_igz(template, png, image_name)


def _material_igz(template, texture_path, export):
    """Retarget HUD Customizer's complete compiled material to one texture."""
    g = Igz(template)
    texture_name = texture_path.removeprefix('textures/').removesuffix('.igz')

    deps = g.fixup('TDEP')
    dependencies = [s.decode() for s in deps[3].split(b'\0')[:deps[1] * 2]]
    indexes = [i for i, value in enumerate(dependencies)
               if 'octane_ui_hud_boost_' in value and i % 2 == 0]
    if len(indexes) != 2:
        raise ValueError('material template boost textures changed')
    for index in indexes:
        dependencies[index] = texture_name
        dependencies[index + 1] = texture_path.replace('textures/', 'textures:/', 1)
    deps[3] = b''.join(value.encode() + b'\0' for value in dependencies)
    deps[4] = (deps[2] + len(deps[3]) + 3) & ~3

    handles = g.fixup('EXNM')
    entries = list(struct.iter_unpack('<II', handles[3][:handles[1] * 8]))
    image_handles = [i for i, (name, space) in enumerate(entries)
                     if g.strings[name] == 'image' and
                     'octane_ui_hud_boost_' in g.strings[space & 0x7fffffff]]
    if len(image_handles) != 2:
        raise ValueError('material template image handles changed')
    for handle in image_handles:
        entries[handle] = (entries[handle][0], g.string_index(texture_name) | 0x80000000)
    handles[3] = b''.join(struct.pack('<II', *entry) for entry in entries)

    name_list = next(off for off, kind in g.objects() if kind == 'igNameList')
    names = g.u64(name_list + 0x20) + 16
    g.set_u64(names, g.string_index(export))
    g.set_u32(names + 8, fnv1a(export))
    return g.build()


def compiled_asset(png, name):
    """Compile one PNG into creator-built texture/material IGZ files."""
    if not name or len(name) > 20 or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789_' for c in name):
        raise ValueError('name must be 1..20 lowercase letters, digits, or underscores')
    image = f'octane_custom_{name}'.ljust(34, '_')
    image_name = f'[{image}.tga]'
    texture_path = f'textures/guistandard_diffuse,textures@!ui!octane!gprix!{image}`tga,001.igz'
    namespace = f'juice_OCTR_Custom_{name}_materials'
    material_path = f'ui/materialInstances/octr/{namespace}.igz'
    export = image + '_tga__material'
    texture = _texture_igz(_template('texture_template.igz').read_bytes(), Path(png), image_name)
    material = _material_igz(_template('material_template.igz').read_bytes(), texture_path, export)
    return texture_path, texture, namespace, material_path, export, material


def material_igz(template, mode, tier):
    _namespace, _path, export = tier_material(mode, tier)
    return _material_igz(template, tier_icon(mode, tier)[2], export)


def menu_icon(kind):
    if kind not in ('ranked', 'hof'):
        raise ValueError('ranked menu icon must be ranked or hof')
    image = f'octane_ranked_menu_{kind}'.ljust(34, '_')
    namespace = f'juice_OCTR_RankedMenu_{kind.title()}_materials'
    texture = f'textures/guistandard_diffuse,textures@!ui!octane!gprix!{image}`tga,001.igz'
    filename = 'hof_icon.png' if kind == 'hof' else 'ranked_icon.png'
    png = ASSETS / filename
    if not png.exists():
        png = Path(__file__).resolve().parent / 'custom_icon' / filename
    return (png,
            f'[{image}.tga]', texture, namespace,
            f'ui/materialInstances/octr/{namespace}.igz',
            image + '_tga__material')


def menu_asset_replacements():
    texture = _template('texture_template.igz').read_bytes()
    material = _template('material_template.igz').read_bytes()
    result = {}
    for kind in ('ranked', 'hof'):
        png, image_name, texture_path, _namespace, material_path, export = menu_icon(kind)
        result[texture_path] = _texture_igz(texture, png, image_name)
        result[material_path] = _material_igz(material, texture_path, export)
    return result


def quick_icon_texture(template=None):
    template = template or _template('texture_template.igz').read_bytes()
    return _texture_igz(
        template, menu_icon('ranked')[0], '[octane_ui_ico_quick.tga]',
        texture_name='data/Octane/textures/ui/Octane/Icons/octane_ui_ico_quick.tga')


def clone_animated_material(blob, new_name, image_name=RANKED_BACKGROUND_IMAGE):
    """Clone Matchmaking's complete animated material inside its native material file."""
    g = Igz(blob)
    name_list = next(off for off, kind in g.objects() if kind == 'igNameList')
    obj_list = next(off for off, kind in g.objects() if kind == 'igObjectList')
    nptr, ncnt = g.u64(name_list + 0x20), g.u32(name_list + 0x0c)
    optr = g.u64(obj_list + 0x20)
    names = {g.strings[g.u32(nptr + i * 16)]: (i, g.u64(optr + i * 8))
             for i in range(ncnt)}
    source_name = 'octane_ui_online_bg_pattern_matchmaking_tga__material'
    background = names[source_name][1]
    transform = names[source_name + '__textureTransform_Transform'][1]
    end = min(off for _, off in names.values() if off > transform)
    if not background < transform < end or not g.u64(background + 0x50):
        raise ValueError('unsupported native animated material layout')
    background_texture = g.u64(g.u64(g.u64(background + 0x58) + 0x20))
    g.sites['RHND'] = g.read_list('RHND', unpack_offsets)
    if background_texture + 0x28 not in g.sites['RHND']:
        raise ValueError('unsupported native animated texture bindings')
    clone = g.append(g.data[background:end])
    for tag, sites in g.sites.items():
        for site in list(sites):
            if background <= site < end:
                target = clone + site - background
                g.add_site(tag, target)
                if tag == 'ROFS':
                    value = g.u64(site)
                    if background <= value < end:
                        g.set_u64(target, clone + value - background)
    deps = g.fixup('TDEP')
    dependencies = deps[3].split(b'\0')[:deps[1] * 2]
    deps[3] = b'\0'.join((*dependencies, image_name.encode(),
                          ('textures:/' + image_name + '.igz').encode())) + b'\0'
    deps[1] += 1
    deps[4] = (deps[2] + len(deps[3]) + 3) & ~3
    handles = g.fixup('EXNM')
    g.set_u32(clone + background_texture - background + 0x28, 0x80000000 | handles[1])
    handles[3] += struct.pack('<II', g.string_index('image'),
                              g.string_index(image_name) | 0x80000000)
    handles[1] += 1
    handles[4] = (handles[2] + len(handles[3]) + 3) & ~3
    animated = [off for off, kind in g.objects()
                if background <= off < end and kind == 'igAnimatedTransformSource']
    if len(animated) != 1:
        raise ValueError('unsupported native background animation')
    for source, positions in ((animated[0], (0x18, 0x48)),
                              (transform, (0x20, 0x50))):
        data = g.u64(clone + source - background + 0x20)
        if any(struct.unpack_from('<2f', g.data, data + position) != (4.0, 5.0)
               for position in positions):
            raise ValueError('unsupported native background repeat scale')
    exports = (new_name, new_name + '__textureTransform_Transform')
    objects = [g.u64(optr + i * 8) for i in range(ncnt)]
    objects.extend((clone, clone + transform - background))
    object_array = g.append(struct.pack(f'<{len(objects)}Q', *objects))
    for i in range(len(objects)):
        g.add_site('ROFS', object_array + i * 8)
    name_entries = g.data[nptr:nptr + ncnt * 16]
    for export in exports:
        name_entries += struct.pack('<QII', g.string_index(export), fnv1a(export), 0)
    name_array = g.append(name_entries)
    for i in range(len(objects)):
        g.add_site('RSTT', name_array + i * 16)
    for off, array, stride in ((obj_list, object_array, 8), (name_list, name_array, 16)):
        g.set_u32(off + 0x0c, len(objects))
        g.set_u32(off + 0x10, len(objects))
        g.set_u64(off + 0x18, (g.u64(off + 0x18) & ~0xffffffff) | len(objects) * stride)
        g.set_u64(off + 0x20, array)
    return g.build()


def clone_lobby_rank_materials(blob):
    """Keep all rank materials in Lobby Members' native material namespace."""
    g = Igz(blob)
    objects = dict(g.objects())
    end = next(off for off, kind in objects.items() if kind == 'igNameList')
    source = next(g.u64(g.u64(0x20) + i * 8) for i in range(g.u32(end + 0x0c))
                  if g.string_at(g.u64(end + 0x20) + i * 16) ==
                  'octane_ui_ico_singlerace_tga__material')
    texture = g.u64(g.u64(g.u64(source + 0x58) + 0x20))
    if objects.get(texture) != 'igGraphicsTexture':
        raise ValueError('unsupported Lobby Members material layout')
    g.sites['RHND'] = g.read_list('RHND', unpack_offsets)
    deps = g.fixup('TDEP')
    dependencies = deps[3].split(b'\0')[:deps[1] * 2]
    handles = g.fixup('EXNM')
    handle_entries = handles[3][:handles[1] * 8]
    name_list = end
    obj_list = next(off for off, kind in objects.items() if kind == 'igObjectList')
    count = g.u32(name_list + 0x0c)
    named = [g.u64(g.u64(obj_list + 0x20) + i * 8) for i in range(count)]
    names = bytearray(g.data[g.u64(name_list + 0x20):g.u64(name_list + 0x20) + count * 16])
    for mode, tier in TIERS:
        clone = g.append(g.data[source:end])
        for tag, sites in g.sites.items():
            for site in list(sites):
                if source <= site < end:
                    target = clone + site - source
                    g.add_site(tag, target)
                    if tag == 'ROFS':
                        value = g.u64(site)
                        if source <= value < end:
                            g.set_u64(target, clone + value - source)
        image = tier_icon(mode, tier)[2].removeprefix('textures/').removesuffix('.igz')
        dependencies.extend((image.encode(), ('textures:/' + image + '.igz').encode()))
        g.set_u32(clone + texture - source + 0x28, 0x80000000 | handles[1])
        handle_entries += struct.pack('<II', g.string_index('image'),
                                      g.string_index(image) | 0x80000000)
        handles[1] += 1
        export = tier_material(mode, tier)[2]
        named.append(clone)
        names += struct.pack('<QII', g.string_index(export), fnv1a(export), 0)
    deps[1] = len(dependencies) // 2
    deps[3] = b'\0'.join(dependencies) + b'\0'
    deps[4] = (deps[2] + len(deps[3]) + 3) & ~3
    handles[3] = handle_entries
    handles[4] = (handles[2] + len(handle_entries) + 3) & ~3
    object_array = g.append(struct.pack(f'<{len(named)}Q', *named))
    name_array = g.append(names)
    for i in range(len(named)):
        g.add_site('ROFS', object_array + i * 8)
        g.add_site('RSTT', name_array + i * 16)
    for off, array, stride in ((obj_list, object_array, 8), (name_list, name_array, 16)):
        g.set_u32(off + 0x0c, len(named))
        g.set_u32(off + 0x10, len(named))
        g.set_u64(off + 0x18, (g.u64(off + 0x18) & ~0xffffffff) | len(named) * stride)
        g.set_u64(off + 0x20, array)
    return g.build()


def clone_hof_icon_material(blob):
    """Clone native Quick material for the HOF icon in the same material namespace."""
    g = Igz(blob)
    name_list = next(off for off, kind in g.objects() if kind == 'igNameList')
    obj_list = next(off for off, kind in g.objects() if kind == 'igObjectList')
    nptr, ncnt = g.u64(name_list + 0x20), g.u32(name_list + 0x0c)
    optr = g.u64(obj_list + 0x20)
    names = {g.strings[g.u32(nptr + i * 16)]: g.u64(optr + i * 8) for i in range(ncnt)}
    source = names['octane_ui_ico_quick_tga__material']
    end = min(off for off in names.values() if off > source)
    texture = g.u64(g.u64(g.u64(source + 0x58) + 0x20))
    g.sites['RHND'] = g.read_list('RHND', unpack_offsets)
    if texture + 0x28 not in g.sites['RHND']:
        raise ValueError('unsupported native Quick texture binding')
    clone = g.append(g.data[source:end])
    for tag, sites in g.sites.items():
        for site in list(sites):
            if source <= site < end:
                target = clone + site - source
                g.add_site(tag, target)
                if tag == 'ROFS':
                    value = g.u64(site)
                    if source <= value < end:
                        g.set_u64(target, clone + value - source)
    image_name = menu_icon('hof')[2].removeprefix('textures/').removesuffix('.igz')
    deps = g.fixup('TDEP')
    dependencies = deps[3].split(b'\0')[:deps[1] * 2]
    deps[3] = b'\0'.join((*dependencies, image_name.encode(),
                          ('textures:/' + image_name + '.igz').encode())) + b'\0'
    deps[1] += 1
    deps[4] = (deps[2] + len(deps[3]) + 3) & ~3
    handles = g.fixup('EXNM')
    g.set_u32(clone + texture - source + 0x28, 0x80000000 | handles[1])
    handles[3] += struct.pack('<II', g.string_index('image'),
                              g.string_index(image_name) | 0x80000000)
    handles[1] += 1
    handles[4] = (handles[2] + len(handles[3]) + 3) & ~3
    objects = [g.u64(optr + i * 8) for i in range(ncnt)] + [clone]
    object_array = g.append(struct.pack(f'<{len(objects)}Q', *objects))
    for i in range(len(objects)):
        g.add_site('ROFS', object_array + i * 8)
    name_entries = (g.data[nptr:nptr + ncnt * 16] +
                    struct.pack('<QII', g.string_index(HOF_ICON_MATERIAL),
                                fnv1a(HOF_ICON_MATERIAL), 0))
    name_array = g.append(name_entries)
    for i in range(len(objects)):
        g.add_site('RSTT', name_array + i * 16)
    for off, array, stride in ((obj_list, object_array, 8), (name_list, name_array, 16)):
        g.set_u32(off + 0x0c, len(objects))
        g.set_u32(off + 0x10, len(objects))
        g.set_u64(off + 0x18, (g.u64(off + 0x18) & ~0xffffffff) | len(objects) * stride)
        g.set_u64(off + 0x20, array)
    return g.build()


def clone_lobby_materials(blob):
    blob = clone_animated_material(blob, RANKED_BACKGROUND_MATERIAL)
    blob = clone_animated_material(blob, HOF_BACKGROUND_MATERIAL, HOF_BACKGROUND_IMAGE)
    return clone_hof_icon_material(blob)


def asset_replacements(single_race_template=None):
    texture = _template('texture_template.igz').read_bytes()
    material = _template('material_template.igz').read_bytes()
    result = {SINGLE_RACE_TEXTURE: single_race_texture(single_race_template or texture)}
    for mode, tier in TIERS:
        result[tier_icon(mode, tier)[2]] = texture_igz(texture, mode, tier)
        result[tier_material(mode, tier)[1]] = material_igz(material, mode, tier)
    return result


def texture_replacements(template=None):
    """Compatibility helper used by focused asset tests."""
    source = _template('texture_template.igz').read_bytes() if template is None else template
    return {tier_icon(mode, tier)[2]: texture_igz(source, mode, tier)
            for mode, tier in TIERS}


