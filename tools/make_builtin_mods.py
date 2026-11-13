#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The built-in mods (scripts/builtin_mods) made from their authors' own packages.

Ultimate Customizer and Custom Cup (both by Kyouko8) ship with the launcher as built-in mods.
Their assets are kept as copy deltas against the player's game files (copy-delta-v1, as the
built-in widescreen layouts): only the authors' own bytes are in the repository, never a game
file. This rebuilds the folders from the authors' release ZIPs and the game:

    python tools/make_builtin_mods.py GAME_FOLDER ULTIMATE_CUSTOMIZER.zip CUSTOM_CUP.zip

  ultimate-customizer  Ultimate Customizer (formerly Race and Battle Options): laps up to 250, three-digit HUD and menus, the long race
                    clock, battle points/time/lives, the random cup; with the Copas Adicionales
                    option ("extra-cups", off by default) its 25 new Arcade cups (8 easy,
                    9 medium, 8 hard) and their stars.
  custom-cup        Custom Cup: four tracks of the player's choice, with the track thumbnails.

The added cups' pictures (37 MB of collages of the game's own track pictures) are not kept: the
launcher's "Prepare" draws them on the player's PC from the game files (scripts/cup_art.py), and
Copas Adicionales can be turned on only then.

Ultimate Customizer's systemData.igz holds both the battle values and the added cups; the
battle-only copy is made here from the game's file and the author's three battle lists, so the
cups can be switched off without losing the battle options. Its executable patch is split the
same way (the shared code block stays in the base: nothing reaches its cup parts without the
cup hooks).
"""
import argparse
import hashlib
import json
import re
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))

from graphics_tab import Archive  # noqa: E402
from igz_merge import MASK, File  # noqa: E402
from nitro_components import copy_delta  # noqa: E402

OUT = ROOT / 'scripts' / 'builtin_mods'
MARKER = 'octr-mod.required'
MARKER_TEXT = b'OCTR Mod Package 2\n'
WINDOW = 32
SYSTEM = 'looseData/systemData.igz'
BATTLE = ('SystemData_battleArenaLives', 'SystemData_battleArenaLimitPoints', 'SystemData_battleArenaTimesMinutes')
CUP_ASSETS = ('materialinstances/UI/CupSelect_materials.igz',)
ART_ASSET = 'textures/GuiStandard_diffuse,textures@!ui!Octane!RaceSelect!octane_ui_raceselect_bg_spacecup`tga,101.igz'
# Ultimate Customizer's patch: the lines of the added cups, by their comment.
CUP_LINES = ('Cup stars: an added cup', 'Cup stars: winning', 'Cup stars: the save data', 'Cup menu: find a cup')


def make_delta(original, result):
    """copy-delta-v1 rebuilding `result` from `original`: copies of what the original has, the
    rest as bytes."""
    index = {}
    for i in range(0, len(original) - WINDOW + 1, 4):
        index.setdefault(original[i:i + WINDOW], i)
    ops, literal, i = [], bytearray(), 0
    while i < len(result):
        j = index.get(result[i:i + WINDOW]) if i + WINDOW <= len(result) else None
        if j is None:
            literal.append(result[i])
            i += 1
            continue
        n = WINDOW
        while i + n < len(result) and j + n < len(original) and result[i + n] == original[j + n]:
            n += 1
        if literal:
            ops.append(['bytes', literal.hex()])
            literal = bytearray()
        if ops and ops[-1][0] == 'copy' and ops[-1][1] + ops[-1][2] == j:
            ops[-1][2] += n
        else:
            ops.append(['copy', j, n])
        i += n
    if literal:
        ops.append(['bytes', literal.hex()])
    delta = {'format': 'copy-delta-v1', 'source_bytes': len(original), 'result_bytes': len(result), 'operations': ops}
    if copy_delta(original, delta) != result:
        raise AssertionError('copy delta does not rebuild the asset')
    return delta


def sha(data):
    return hashlib.sha256(data).hexdigest()


def game_entry(game, archive, index, asset, expected):
    a = Archive(Path(game) / 'archives' / archive)
    try:
        if a.logical(index).casefold() != asset.casefold():
            raise ValueError(f'{archive}[{index}] is {a.logical(index)}, not {asset}')
        data = a.read(index)
    finally:
        a.f.close()
    if sha(data) != expected:
        raise ValueError(f'{asset} in {archive} is not the USA 1.21 file the mod was made for')
    return data


def battle_only(stock, mod):
    """The game's systemData.igz with only the mod's three battle lists."""
    z, m = File(stock), File(mod)
    names, mod_names = dict(z.roots()[2]), dict(m.roots()[2])
    for name in BATTLE:
        o, mo = names[name], mod_names[name]
        length = m.u64(mo + 0x18) & 0xFFFFFFFF
        block = m.ref(mo + 0x20)
        if list(m.sites_in(block, length)):
            raise ValueError(f'{name}: its values hold references')
        at = z.append(bytes(m.data[block:block + length]))
        for field in (0x8, 0xC, 0x10, 0x18):
            z.set_u32(field + o, m.u32(field + mo)) if field != 0x18 else z.set_u64(o + 0x18, m.u64(mo + 0x18))
        z.set_u64(o + 0x20, (z.u64(o + 0x20) & ~MASK) | at)
    out = z.build()
    check = File(out)
    for name in BATTLE:
        o, mo = dict(check.roots()[2])[name], mod_names[name]
        n = m.u64(mo + 0x18) & 0xFFFFFFFF
        if check.data[check.ref(o + 0x20):check.ref(o + 0x20) + n] != m.data[m.ref(mo + 0x20):m.ref(mo + 0x20) + n]:
            raise AssertionError(name + ' differs')
    return out


def split_patch(xml):
    """(base XML, added cups XML) of Ultimate Customizer's patch, by the lines' comments."""
    head, body = xml.split('<PatchList>', 1)
    body, tail = body.rsplit('</PatchList>', 1)
    parts = re.findall(r'(\s*<!--.*?-->\s*<Line [^>]*/>)', body, re.S)
    if ''.join(parts).strip() != body.strip():
        raise ValueError('unexpected patch layout: every line needs its comment')
    cups = [p for p in parts if any(c in p for c in CUP_LINES)]
    base = [p for p in parts if p not in cups]
    def make(lines, name, note):
        h = re.sub(r'Name="[^"]*"', f'Name="{name}"', head, count=1)
        h = re.sub(r'Note="[^"]*"', f'Note="{note}"', h, count=1)
        h = re.sub(r'isEnabled="[^"]*"', 'isEnabled="true"', h, count=1)
        return h + '<PatchList>' + ''.join(lines) + '\n        </PatchList>' + tail
    return (make(base, 'Ultimate Customizer',
                 'Laps up to 3 digits in the menu and the HUD; race clock up to 999:59:99; long races without '
                 'running out of network memory; random cup with Square. Kyouko8 (Ultimate Customizer).'),
            make(cups, 'Copas Adicionales', 'The 25 new Arcade cups: their stars (rbo_cups.bin) and the cup menu. '
                                     'Kyouko8 (Ultimate Customizer).'))


def write_package(folder, manifest, files):
    """A format 2 package folder: files {path: bytes}, the inventory filled in."""
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir(parents=True)
    files = {MARKER: MARKER_TEXT, **files}
    for path, data in files.items():
        (folder / path).parent.mkdir(parents=True, exist_ok=True)
        (folder / path).write_bytes(data)
    manifest['files'] = [{'file': p, 'sha256': sha(d), 'bytes': len(d)} for p, d in sorted(files.items())]
    (folder / 'mod.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    import mod_format
    mod_format.validate_folder(folder)


def as_delta(game, op, payload):
    """The operation and its delta file: (operation, path, bytes)."""
    original = game_entry(game, op['archive'], op['index'], op['asset'], op['original_sha256'])
    name = 'payloads/' + Path(op['payload']).stem + '.delta.json'
    data = json.dumps(make_delta(original, payload), separators=(',', ':')).encode()
    return {**op, 'payload': name}, name, data


def unzip(path, work):
    with zipfile.ZipFile(path) as z:
        z.extractall(work)
    found = [p.parent for p in Path(work).rglob('mod.json')]
    if len(found) != 1:
        raise ValueError(f'{path}: expected one mod.json')
    return found[0], json.loads((found[0] / 'mod.json').read_text(encoding='utf-8'))


def race_options(game, uc, out):
    folder, m = uc
    base_ops, sets, files = [], {'battle': [], 'cups': []}, {}
    stock = None
    for op in m['operations']:
        payload = (folder / op['payload']).read_bytes()
        if op['asset'] == ART_ASSET:
            continue
        if op['asset'] == SYSTEM:
            stock = game_entry(game, op['archive'], op['index'], op['asset'], op['original_sha256'])
            battle = battle_only(stock, payload)
            for set_name, data, stem in (('battle', battle, 'systemdata-battle'), ('cups', payload, 'systemdata-battle-cups')):
                delta = json.dumps(make_delta(stock, data), separators=(',', ':')).encode()
                path = f'payloads/{stem}.delta.json'
                files[path] = delta
                sets[set_name].append({**op, 'payload': path, 'patched_sha256': sha(data)})
            continue
        new, path, data = as_delta(game, op, payload)
        files[path] = data
        (sets['cups'] if op['asset'] in CUP_ASSETS else base_ops).append(new)
    if stock is None:
        raise ValueError('Ultimate Customizer: no systemData.igz operation')
    base_xml, cups_xml = split_patch((folder / m['patches'][0]).read_text(encoding='utf-8'))
    files['patches/ultimate-customizer.xml'] = base_xml.encode('utf-8')
    files['patches/copas-adicionales.xml'] = cups_xml.encode('utf-8')
    manifest = {
        'schemaVersion': 2, 'id': m['id'], 'name': 'Ultimate Customizer',
        'version': m['version'], 'author': m['author'],
        'description': 'Built into the launcher: Ultimate Customizer by Kyouko8 (formerly Race and Battle Options).\n'
                       '- Laps 1-12 up to 250, for single races and cups; three-digit menus and HUD; race clock up to '
                       '999:59:99.\n- Battle points limit 5 to 99, time limit and lives up to 60.\n'
                       '- Random cup: Square on the cup menu.\n- Copas Adicionales (option): 25 new Arcade cups '
                       '(8 easy, 9 medium, 8 hard), each built around a character, with their stars. Prepare draws '
                       'their pictures from your game first.',
        'requiresCapabilities': ['archive-assets', 'xml-patches', 'code-signatures', 'mod-options', 'variants'],
        'compatibility': {'online': 'allowed'},
        'conflicts': m.get('conflicts', []),
        'operations': base_ops,
        'patches': ['patches/ultimate-customizer.xml'],
        'operationSets': sets,
        'configuration': {'extra-cups': {
            'type': 'boolean', 'label': 'Copas Adicionales', 'default': False,
            'description': '25 new Arcade cups (8 easy, 9 medium, 8 hard), with their stars. Prepare their pictures first.'}},
        'variants': [
            # The variant's patches are added to the base ones.
            {'id': 'extra-cups', 'whenOptions': {'extra-cups': [True]}, 'useOperations': ['cups'],
             'patches': ['patches/copas-adicionales.xml']},
            {'id': 'game-cups', 'whenOptions': {'extra-cups': [False]}, 'useOperations': ['battle']}],
    }
    write_package(out / 'ultimate-customizer', manifest, files)


def custom_cup(game, cc, out):
    folder, m = cc
    ops, files = [], {}
    for op in m['operations']:
        new, path, data = as_delta(game, op, (folder / op['payload']).read_bytes())
        ops.append(new)
        files[path] = data
    xml = (folder / m['patches'][0]).read_text(encoding='utf-8')
    files['patches/custom-cup.xml'] = re.sub(r'isEnabled="[^"]*"', 'isEnabled="true"', xml).encode('utf-8')
    manifest = {k: v for k, v in m.items() if k not in ('files', 'operations', 'patches')}
    manifest.update(name='Custom Cup', operations=ops, patches=['patches/custom-cup.xml'],
                    description='Built into the launcher, from Custom Cup by Kyouko8.\n' + m['description'])
    write_package(out / 'custom-cup', manifest, files)




def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('game', type=Path)
    parser.add_argument('ultimate_customizer', type=Path)
    parser.add_argument('custom_cup', type=Path)
    parser.add_argument('--out', type=Path, default=OUT)
    args = parser.parse_args(argv)
    with tempfile.TemporaryDirectory() as work:
        uc = unzip(args.ultimate_customizer, Path(work) / 'uc')
        cc = unzip(args.custom_cup, Path(work) / 'cc')
        if uc[1]['id'] != 'battle-point-limit-selector' or cc[1]['id'] != 'custom-cup':
            raise SystemExit('expected Ultimate Customizer and Custom Cup')
        race_options(args.game, uc, args.out)
        custom_cup(args.game, cc, args.out)
    for folder in sorted(p for p in args.out.iterdir() if p.is_dir()):
        size = sum(f.stat().st_size for f in folder.rglob('*') if f.is_file())
        print(f'{folder.name}: {size / 1024:.0f} KB')


if __name__ == '__main__':
    main()
