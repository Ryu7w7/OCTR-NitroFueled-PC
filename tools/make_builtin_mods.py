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
import struct
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
# Capture the Flag and Steal the Bacon's points limit: as long as the lives' and minutes' (5.1.1)
CTF_POINTS = 'SystemData_battleArenaPoints'
MORE_POINTS = [3, 6, 9, 12, 15, 20, 25, 30, 40, 50, 60]
UC_VERSION = '5.1.3'  # the built-in: Ultimate Customizer 5.1.0 with MORE_POINTS and save_on_win
# save_on_win: 5.1.0's code block, the end of its cup win (the jmp's rel32), and the code added
CAVE, WIN_END, WRITE_SKIP, SAVE_AT = 0x2338000, 0x8f, 0x119def9, 0x2338800
# push rbp..r13, aligned, 0x50 bytes of stack. When STATE (0x2bee700) bit 2 (changed) is set:
# - not yet loaded (bit 1; 5.1.0 loads at an unmount, which never comes here): read
#   /savedata0/rbo_cups.bin to the stack and, with its magic, OR its 64 cup bytes into the table
#   (BUF + 8, 0x2bee718: this session's wins stay); a full read marks it loaded;
# - BUF (0x2bee710) = "RBOCUPS1" + the table, open(path, 0x601, 0x1b6), pwrite 0x48 bytes; all
#   written: clear bit 2 and mark it loaded (the save is mounted: a missing file was no file).
# With the save not mounted both opens fail and nothing changes (the unmount does it, as 5.1.0).
# Then pop and jmp WRITE_SKIP. The strings are 5.1.0's (0x23383cf, 0x23383fd); open, pread,
# pwrite and close its imports (0x1beab78, 0x1bea7b8, 0x1bea868, 0x1beaa38).
SAVE_CODE = bytes.fromhex(
    '554889e55051525657415041514152415353415441554883e4f04883ec50488d1ddb5e8b00f703020000000f84020100'
    '00f703010000000f859e000000488d3db9fbffff31f631d2e82b238bff4189c44585e40f88820000004489e74889e6ba'
    '4800000031c9e84d1f8bff4189c54489e7e8c2218bff4183fd48755f488b054cfbffff48390424754f488d0d805e8b00'
    '488b44240848094108488b44241048094110488b44241848094118488b44242048094120488b44242848094128488b44'
    '243048094130488b44243848094138488b44244048094140830b01488b05edfaffff488d0d275e8b00488901488d3d0a'
    'fbffffbe01060000bab6010000e876228bff4189c44585e478294489e7488d35fc5d8b00ba4800000031c9e8481f8bff'
    '83f84875068323fd830b014489e7e805218bff488d65a0415d415c5b415b415a415941585f5e5a59585de9aa55e6fe')
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


def more_battle_points(blob):
    """systemData.igz with the points limit of Capture the Flag and Steal the Bacon
    (SystemData_battleArenaPoints: 3, 6, 9) as long as the lives' and minutes' (MORE_POINTS);
    Ultimate Customizer 5.1.0 left it as the game has it."""
    z = File(blob)
    o = dict(z.roots()[2])[CTF_POINTS]
    length = z.u64(o + 0x18) & 0xFFFFFFFF
    if list(z.sites_in(z.ref(o + 0x20), length)):
        raise ValueError(f'{CTF_POINTS}: its values hold references')
    old = list(struct.unpack_from(f'<{length // 4}i', z.data, z.ref(o + 0x20)))
    if old != [3, 6, 9]:
        raise ValueError(f'{CTF_POINTS}: expected the game\'s 3, 6, 9, found {old}')
    at = z.append(struct.pack(f'<{len(MORE_POINTS)}i', *MORE_POINTS))
    z.set_u32(o + 0xC, len(MORE_POINTS))
    z.set_u32(o + 0x10, len(MORE_POINTS))
    z.set_u64(o + 0x18, (z.u64(o + 0x18) & ~0xFFFFFFFF) | 4 * len(MORE_POINTS))
    z.set_u64(o + 0x20, (z.u64(o + 0x20) & ~MASK) | at)
    out = z.build()
    check = File(out)
    c = dict(check.roots()[2])[CTF_POINTS]
    got = list(struct.unpack_from(f'<{len(MORE_POINTS)}i', check.data, check.ref(c + 0x20)))
    if got != MORE_POINTS or check.u32(c + 0xC) != len(MORE_POINTS):
        raise AssertionError(CTF_POINTS + ' differs')
    return out


def save_on_win(xml):
    """Ultimate Customizer 5.1.0's patch with the added cups' stars saved when a cup is won (5.1.2; 5.1.3 loads the file first).

    5.1.0 writes /savedata0/rbo_cups.bin where the game unmounts its save data; this runtime
    mounts it once a session and never unmounts it, so the stars were lost on closing the game.
    The win's code (in the cave, 0x2338000) now goes on to SAVE_CODE at SAVE_AT, which writes the
    table at once, reading the file first when it was not loaded (the unmount still writes it too)."""
    m = re.search(r'(Address="0x%x" Value=")([0-9A-F]+)"' % (CAVE + 0x400000), xml)
    body = bytearray(bytes.fromhex(m.group(2)))
    # lea rsi, [STATE]; or dword ptr [rsi], 2; jmp WRITE_SKIP: the end of the win's code
    if bytes(body[WIN_END - 11:WIN_END]) != bytes.fromhex('488d3575668b00830e02e9') or \
            struct.unpack_from('<i', body, WIN_END)[0] + CAVE + WIN_END + 4 != WRITE_SKIP:
        raise ValueError('Ultimate Customizer: unexpected code at the end of a cup win')
    struct.pack_into('<i', body, WIN_END, SAVE_AT - (CAVE + WIN_END + 4))
    xml = xml[:m.start(2)] + body.hex().upper() + xml[m.end(2):]
    line = (f'\n            <!-- Cup stars: winning an added cup saves its table at once -->\n'
            f'            <Line Type="bytes" Address="{SAVE_AT + 0x400000:#x}" Value="{SAVE_CODE.hex().upper()}"\n'
            f'                  ExpectedSHA256="{sha(bytes(len(SAVE_CODE)))}"/>')
    head, tail = xml.rsplit('\n        </PatchList>', 1)
    return head + line + '\n        </PatchList>' + tail


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
            battle = more_battle_points(battle_only(stock, payload))
            payload = more_battle_points(payload)
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
    base_xml, cups_xml = split_patch(save_on_win((folder / m['patches'][0]).read_text(encoding='utf-8')))
    files['patches/ultimate-customizer.xml'] = base_xml.encode('utf-8')
    files['patches/copas-adicionales.xml'] = cups_xml.encode('utf-8')
    manifest = {
        'schemaVersion': 2, 'id': m['id'], 'name': 'Ultimate Customizer',
        'version': UC_VERSION, 'author': m['author'],
        'description': 'Built into the launcher: Ultimate Customizer by Kyouko8 (formerly Race and Battle Options).\n'
                       '- Laps 1-12 up to 250, for single races and cups; three-digit menus and HUD; race clock up to '
                       '999:59:99.\n- Battle points limit 5 to 99, time limit and lives up to 60; Capture the Flag '
                       'and Steal the Bacon points up to 60.\n'
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
    files = {}

    def deltas(operations):
        out = []
        for op in operations:
            new, path, data = as_delta(game, op, (folder / op['payload']).read_bytes())
            out.append(new)
            files[path] = data
        return out

    ops = deltas(m['operations'])
    # its options' operations (the race settings thumbnails: Miniaturas en ajustes de carrera)
    sets = {name: deltas(operations) for name, operations in m.get('operationSets', {}).items()}
    xml = (folder / m['patches'][0]).read_text(encoding='utf-8')
    files['patches/custom-cup.xml'] = re.sub(r'isEnabled="[^"]*"', 'isEnabled="true"', xml).encode('utf-8')
    # its options' patches (Copas Extendidas), as they are: an option turns them on
    for rule in m.get('configuration', {}).values():
        for name in {p for paths in rule.get('patches', {}).values() for p in paths}:
            files[name] = (folder / name).read_bytes()
    manifest = {k: v for k, v in m.items() if k not in ('files', 'operations', 'patches', 'operationSets')}
    manifest.update(name='Custom Cup', operations=ops, patches=['patches/custom-cup.xml'],
                    description='Built into the launcher, from Custom Cup by Kyouko8.\n' + m['description'])
    if sets:
        manifest['operationSets'] = sets
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
