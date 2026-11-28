#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Connect the game's unused skin portrait materials to race HUD skin data."""
import re
from pathlib import Path

import archive_patch
import item_rules
from archive_reader import Archive
from mod_file_paths import resolve
from racer_registry import Copier, File

ENTRY = 'loosedata/systemdata.igz'
MATERIAL_ENTRY = 'materialinstances/ui/racerportraits_materials.igz'
MATERIAL_DIR = 'materialinstances/ui/skinsportraitmaterials/'
MATERIAL_NAMESPACE = 'RacerPortraits_materials'
OVERRIDES = {
    'Crash_Blue': 'Skin_Crash_BlueHyena',
    'KomodoJoe_Yellow': 'Skin_KomodoJoe_Default',
    'Pinstripe_Yellow': 'Skin_Pinstripe_Summer',
    'Polar_Bear': 'Skin_Polar_Default',
}
DRIVER_ALIASES = {'NTropy': 'NTrophy', 'Papu': 'PapuPapu', 'Penta': 'PentaPenguin', 'Tiny': 'TinyTiger'}


def words(text):
    text = re.sub(r'([a-z0-9])([A-Z])', r'\1 \2', text).replace('_', ' ')
    return [word.lower() for word in re.findall(r'[A-Za-z0-9]+', text)]


def likeness(want, have):
    want = [word for word in want if word not in ('nitro', 'fueled')]
    if ''.join(want) == ''.join(have):
        return 0
    a, b = set(want), set(have)
    if a and (a < b or b < a):
        return 1 + abs(len(want) - len(have))
    return 10 + len(a ^ b) + abs(len(want) - len(have))


def portrait_files(game):
    """{suffix: (namespace, RacerHud export, Minimap export, file)} from update archives."""
    paths = [Path(game) / 'archives' / 'update.pak', *item_rules.loaded_copies(game).values()]
    found = {}
    for path in paths:
        path = resolve(path)
        if not path.is_file():
            continue
        archive = Archive(path)
        try:
            for entry in archive.files:
                logical = entry['path'].replace('\\', '/')
                low = logical.lower()
                if MATERIAL_DIR not in low or not low.endswith('.igz'):
                    continue
                namespace = Path(logical).stem
                suffix = namespace.removeprefix('SkinsPortraitMaterials_').removesuffix('_materials')
                blob = bytes(archive.extract(entry))
                roots = [name for name, _ in File(blob).roots()[2]]
                hud = next((name for name in roots if name.lower().startswith('racerhud_')), None)
                minimap = next((name for name in roots if name.lower().startswith('minimap_')), None)
                if hud and minimap:
                    found[suffix] = namespace, hud, minimap, blob
        finally:
            archive.data.close()
            archive._file.close()
    return found


def materials(game):
    return {suffix: values[:3] for suffix, values in portrait_files(game).items()}


def choose_skin(system, suffix, roots, drivers):
    override = roots.get(OVERRIDES.get(suffix, ''))
    if override:
        return override
    exact = roots.get('Skin_' + suffix)
    if exact and system.typed.get(exact) == 'CDriverSkinData':
        return exact
    driver, _, tail = suffix.partition('_')
    key = DRIVER_ALIASES.get(driver, driver)
    choices = []
    for base, data in drivers:
        if base != key:
            continue
        driver_words = words(system.string_at(data + 0x80))
        for skin in system.items(system.ref(data + 0x98)):
            display = words(system.string_at(skin + 0x10))
            for word in driver_words:
                if word in display:
                    display.remove(word)
            choices.append((likeness(words(tail), display), skin))
    choices.sort()
    if not choices or choices[0][0] != 0 or (len(choices) > 1 and choices[1][0] == 0):
        return None
    return choices[0][1]


def apply(blob, portraits):
    system = File(blob)
    roots = {name: obj for name, obj in system.roots()[2] if system.typed.get(obj) == 'CDriverSkinData'}
    drivers = []
    for data in system.items(system.drivers()):
        try:
            base = system.handle(system.u32(data + 0x70))[0].removeprefix('Racer_')
        except (IndexError, KeyError):
            continue
        drivers.append((base, data))
    changed = 0
    for suffix, (_namespace, hud, _minimap) in sorted(portraits.items()):
        skin = choose_skin(system, suffix, roots, drivers)
        if skin is None:
            continue
        portrait_handle = system.handle_of(hud, MATERIAL_NAMESPACE, True)
        modified = system.u32(skin + 0x78) != portrait_handle
        if modified:
            system.set_u32(skin + 0x78, portrait_handle)
            system.add_site('RHND', skin + 0x78)
        if modified:
            changed += 1
    return system.build(), changed


def merge_materials(blob, portraits):
    target = File(blob)
    root, names, existing = target.roots()
    objects = dict(existing)
    added = []
    for suffix, (_namespace, hud, minimap, source_blob) in sorted(portraits.items()):
        source = File(source_blob)
        source_roots = dict(source.roots()[2])
        copier = Copier(source, target)
        materials = []
        for name in (hud, minimap):
            if name not in objects:
                objects[name] = copier.object(source_roots[name])
                added.append((name, objects[name]))
            materials.append(objects[name])
    target.grow(root, [obj for _, obj in added])
    target.grow(names, [name for name, _ in added], 16)
    return target.build(), len(added)


def prepare(game, out, base, log=print):
    sources = portrait_files(game)
    portraits = {suffix: values[:3] for suffix, values in sources.items()}
    folder = Path(out) / 'skin_portraits'
    made = {}
    for name, path in item_rules.loaded_copies(game).items():
        if not path.is_file():
            continue
        guest = f'/app0/archives/{name}'
        archive = Archive(path)
        try:
            entry = next((entry for entry in archive.files if entry['path'].lower() == ENTRY), None)
            material_entry = next((entry for entry in archive.files if entry['path'].replace('\\', '/').lower()
                                   == MATERIAL_ENTRY), None)
            if entry is None or material_entry is None:
                continue
            current = item_rules.current(archive, path, entry, base.get(guest))
            patched, count = apply(current, portraits)
            material = item_rules.current(archive, path, material_entry, base.get(guest))
            merged, material_count = merge_materials(material, sources)
        finally:
            archive.data.close()
            archive._file.close()
        if not count and not material_count:
            continue
        folder.mkdir(parents=True, exist_ok=True)
        target = folder / (Path(name).stem.lower() + '.bbpatch')
        entries = {entry['index']: patched, material_entry['index']: merged}
        archive_patch.write_entries_patch(target, path, guest, entries, base.get(guest),
                                          override=set(entries))
        made[guest] = target
        log(f'Skin portraits: {count} skins, {material_count} materials in {name}')
    return made
