"""Corrected 21:9 and 32:9 layouts: menus, backgrounds, masks and a centered HUD.

The layouts, their data (scripts/widescreen_data) and its checks (nitro_format.py,
nitro_components.py, archive_reader.py) are by tcosi. bbport.ini picks one:

    widescreen_layout=off|21x9|32x9    widescreen_output=WxH (one of OUTPUTS)

What a layout changes, the game's files staying as they are:
- 224 menu, background and HUD assets in 13 archives: archive patches (archive_patch.py,
  BB_ARCHIVE_PATCHES), made from the player's own archives once per layout and kept in
  out/widescreen/; every asset is checked before and after (its SHA-256 in the data);
- the executable: the variant for the internal resolution (render_res), a patch file in
  out/widescreen/patches/ read with the bundled patches;
- the two HUD vertex shaders: their x scaled when they are loaded (BB_HUD_SCALE, the GPU
  library's bb_hud_scale.h), so the shader cache stays as it is;
- the render settings the layouts were made for (scripts/patches.py reads a copy of bbport.ini
  with them, out/widescreen/bbport.ini): the base PS4 path at 1080 (the variant sets the size),
  no temporal upscaler, and the ultrawide of bbport.ini off, since the layout sets the size.

For the verified USA and EU 1.21 executables, in online and offline mode. When an asset
is not the expected one (a mod), the game starts as it would without a layout, and the
reason is printed.

GPL-2.0-or-later.
"""
import copy
import hashlib
import json
import os
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from struct import error as struct_error

import archive_patch
import nitro_format as fmt
import nitro_variant_settings  # noqa: F401  (nitro_format imports it when a layout is resolved)
from archive_reader import Archive
from nitro_components import payload_bytes, read_ini, sha, under
from widescreen_startup import correct_text
from widescreen_online import (NOTICE, REPORT, CHALLENGES, correct_notice, correct_report,
                               correct_challenges, correct_markers)

DATA = Path(__file__).resolve().parent / 'widescreen_data'
OUTPUTS = {'21x9': ('3440x1440', '2560x1080'),
           '32x9': ('5120x1440', '3840x1080', '2560x720', '7680x2160')}
INTERNAL = ('480', '720', '1080', '1440', '1620', '2160')
TITLE_ID, APP_VERSION = 'CUSA13795', '01.21'
# The European edition 1.21 (CUSA14876): GetLostIn's port of the layouts (their 403445c, GPL-2.0;
# widescreen_data/eu). Its UI files are the USA ones with other strings, ported per file and
# checked by SHA-256 like the USA ones; the executable runs the same code with a few addresses
# moved (ultrawide_eu.json, port_meta). The HUD shaders are the same.
EU_TITLE = 'CUSA14876'
EU = DATA / 'eu'
FORMAT = 13  # target GrandPrixReport's native blue sprite; restore standings
MESSAGES = sys.stdout  # run.sh reads the shell lines on stdout: its messages go to stderr


def say(message):
    print('Widescreen: ' + message, file=MESSAGES, flush=True)


def selection(settings):
    """(layout, output, internal) from bbport.ini settings, or None for 16:9."""
    layout = settings.get('widescreen_layout', 'off')
    if layout not in OUTPUTS:
        return None
    output = settings.get('widescreen_output', '')
    if output not in OUTPUTS[layout]:
        output = OUTPUTS[layout][0]
    internal = settings.get('render_res', '1080')
    if internal not in INTERNAL:
        internal = '1080'
    return layout, output, internal


def hud_scale(operations):
    """BB_HUD_SCALE for the variant's shader operations: "hash:permutation:factor,..."."""
    items = []
    for op in operations:
        m = re.fullmatch(r'cache/CUSA13795/0x([0-9a-f]{16})_(\d+)\.spv', op['target'])
        if op['kind'] != 'shader_position_scale' or not m:
            raise ValueError('unsupported shader operation ' + op['kind'])
        items.append(f'{int(m[1], 16):x}:{m[2]}:{float(op["factor"])!r}')
    return ','.join(items)


def eu_port():
    return json.loads((EU / 'ultrawide_eu.json').read_text(encoding='utf-8'))


def port_meta(meta, port):
    """A copy of a USA layout <Metadata> for the EU executable: each line moved, its RIP-relative
    and branch operands re-aimed, and the EU bytes as its signature (patches.py checks them)."""
    meta = copy.deepcopy(meta)
    line_delta = {int(k, 0): v for k, v in port['line_delta'].items()}
    target_delta = {int(k, 0): v for k, v in port['target_delta'].items()}
    operands = {int(k, 0): v for k, v in port['operands'].items()}
    signatures = {int(k, 0): v for k, v in port['signatures'].items()}
    lines = [(int(l.get('Address'), 0), l) for l in meta.iter('Line')]
    spans = [(a, a + len(l.get('Value')) // 2) for a, l in lines]

    def moved(target):
        for start, end in spans:
            if start <= target < end:
                return target + line_delta.get(start, 0)
        return target + target_delta.get(target, 0)
    for a, line in lines:
        if a not in operands:
            raise ValueError(f'no EU port data for the patch at {a:#x}')
        blob = bytearray.fromhex(line.get('Value'))
        site = a + line_delta.get(a, 0)
        for field, size, end in operands[a]:
            disp = int.from_bytes(blob[field:field + size], 'little', signed=True)
            new = moved(a + end + disp) - (site + end)
            if not -(1 << (8 * size - 1)) <= new < 1 << (8 * size - 1):
                raise ValueError(f'EU operand at {a:#x}+{field} out of range')
            blob[field:field + size] = new.to_bytes(size, 'little', signed=True)
        line.set('Address', f'{site:#x}')
        line.set('Value', blob.hex())
        line.set('Signature', signatures[a])
    return meta


def archive_patches(game, out, folder, operations, base, data='nitro-mod.json', text_factor=1.0):
    """{guest: patch file} of the layout's assets, made from the player's archives (and kept in
    them the patches of base, {guest: patch file}, for the same archives)."""
    groups = {}
    operations = list(operations)
    online_fixes = folder != EU
    if online_fixes:
        operations += json.loads((DATA / 'online-notices.json').read_text(encoding='utf-8'))
    for op in operations:
        groups.setdefault(op['archive'], []).append(op)
    archives = Path(game) / 'archives'
    from mod_file_paths import resolve
    stamp = {'format': FORMAT, 'data': sha(folder / data), 'startup_text_factor': text_factor,
             'archives': {name: [str(resolve(archives / name)), resolve(archives / name).stat().st_size, resolve(archives / name).stat().st_mtime_ns]
                          for name in sorted(groups)},
             'operations': hashlib.sha256(json.dumps(operations, sort_keys=True).encode()).hexdigest(),
             'base': {g: sha(p) for g, p in sorted(base.items())}}
    key = hashlib.sha256(json.dumps(stamp, sort_keys=True).encode()).hexdigest()[:16]
    target = Path(out) / 'widescreen' / key
    made = {f'/app0/archives/{name}': target / f'{name}.bbpatch' for name in groups}
    try:
        if json.loads((target / 'stamp.json').read_text()) == stamp and all(p.is_file() for p in made.values()):
            return made
    except (OSError, ValueError):
        pass
    say(f'preparing {len(operations)} menu and HUD assets from the game\'s archives (once)')
    target.mkdir(parents=True, exist_ok=True)
    for name, ops in sorted(groups.items()):
        a = Archive(archives / name)
        try:
            entries = {}
            for op in ops:
                entry = a.files[op['index']]
                if entry['path'] != op['asset']:
                    raise ValueError(f'{name}: entry {op["index"]} is not {op["asset"]}')
                # Checked against the original's and the result's SHA-256 (nitro_components.py).
                original = a.extract(entry)
                if op['kind'] == 'online_notice':
                    if hashlib.sha256(original).hexdigest() != op['original_sha256']:
                        raise ValueError(f'{name}: unexpected notification asset')
                    payload = correct_notice(original, text_factor)
                else:
                    payload = payload_bytes(op, folder, original)
                if op['asset'].casefold() == 'ui/legal.igz':
                    payload = correct_text(payload, text_factor)
                if online_fixes and op['asset'] == REPORT:
                    payload = correct_report(payload, text_factor)
                if online_fixes and op['asset'] == CHALLENGES:
                    payload = correct_challenges(payload, text_factor)
                entries[op['index']] = payload
        finally:
            a.data.close()
            a._file.close()
        guest = f'/app0/archives/{name}'
        archive_patch.write_entries_patch(made[guest], archives / name, guest, entries, base.get(guest))
    (target / 'stamp.json').write_text(json.dumps(stamp))
    return made


def misfit(xml, out):
    """Why the layout's executable patches do not fit this game's executable (out/eboot.elf, from
    scripts/prepare.py), or None when every line finds its one place in it as scripts/patches.py
    places signed lines. Any 1.21 build qualifies (the 5.05 and 6.72+ firmware builds have the code
    at other addresses), but not a base-game executable under a 1.21 param.sfo: there patches.py
    would skip a line's whole patch, and the menus and HUD made for the layout would show over a
    16:9 game."""
    import patches
    try:
        elf = (Path(out) / 'eboot.elf').read_bytes()
    except OSError as error:
        return f'no eboot.elf ({error.strerror})'
    for meta in xml.iter('Metadata'):
        for line in meta.iter('Line'):
            if not line.get('Signature'):
                return f'{line.get("Address")} has no signature'
            try:
                patches.locate(line, int(line.get('Address') or '', 0) - patches.EBOOT_BASE, elf)
            except (ValueError, struct_error) as error:
                return str(error)
    return None


def prepare(game, out, config, env, title_id, app_version, base_patches):
    """The launch with the layout bbport.ini picks: a dict with `config` (the settings file
    scripts/patches.py and the render path read), `patches_dir` (the executable variant),
    `archive_patches` ({guest: file}, replacing base_patches' for the same archives) and `env`;
    None for 16:9 or when the layout cannot be used (the reason is printed)."""
    settings = read_ini(config) if Path(config).is_file() else {}
    chosen = selection(settings)
    if chosen is None:
        return None
    layout, output, internal = chosen
    label = layout.replace('x', ':')
    eu = title_id == EU_TITLE
    if app_version != APP_VERSION or title_id not in (TITLE_ID, EU_TITLE):
        say(f'{label} is made for the 1.21 update ({TITLE_ID} or {EU_TITLE}); this game is {title_id} {app_version}: 16:9')
        return None
    try:
        folder = DATA / ('ultrawide-' + layout)
        manifest = fmt.validate_folder(folder)
        variant, flat = fmt.resolve(manifest, dict(settings, output_res=output, render_res=internal))
        shaders = [op for op in flat['operations'] if op['kind'].startswith('shader_')]
        port = eu_port() if eu else None
        if eu:
            # Any EU 1.21 build the executable patches fit (misfit, below).
            sets = json.loads((EU / 'assets.json').read_text(encoding='utf-8'))['sets']
            used = next(v for v in manifest['variants'] if v['id'] == variant)['useOperations']
            assets = [op for name in used for op in sets[name]]
            made = archive_patches(game, out, EU, assets, base_patches, 'assets.json', shaders[0]['factor'])
        else:
            # Any USA 1.21 build the executable patches fit (misfit, below).
            assets = [op for op in flat['operations'] if op['kind'] == 'archive_asset']
            made = archive_patches(game, out, folder, assets, base_patches, text_factor=shaders[0]['factor'])
        hud = hud_scale(shaders)
    except (OSError, ValueError, KeyError) as error:
        say(f'{label} not used ({error}): 16:9')
        return None

    root = Path(out) / 'widescreen'
    patches_dir = root / 'patches'
    patches_dir.mkdir(parents=True, exist_ok=True)
    xml = ET.Element('Patch')
    ET.SubElement(ET.SubElement(xml, 'TitleID'), 'ID').text = title_id
    for rel in flat['patches']:
        for meta in ET.parse(under(folder, rel)).getroot().iter('Metadata'):
            meta.set('isEnabled', 'true')
            xml.append(port_meta(meta, port) if eu else meta)
    if not eu:
        correct_markers(xml, shaders[0]['factor'])
    reason = misfit(xml, out)
    if reason:
        say(f'{label} not used (the executable is not a 1.21 build the layout fits: {reason}): 16:9')
        return None
    ET.ElementTree(xml).write(patches_dir / 'Widescreen.xml', encoding='utf-8', xml_declaration=True)

    # The settings the layout was made for, over the player's: they stay in bbport.ini.
    effective = dict(flat.get('settings', {}), ultrawide='0', ultrawide_frontend='0', ultrawide_cinematics='0')
    lines = [line for line in Path(config).read_text(encoding='utf-8').splitlines()
             if line.partition('=')[0].strip() not in effective] if Path(config).is_file() else []
    effective_config = root / 'bbport.ini'
    effective_config.write_text('\n'.join(lines + [f'{k}={v}' for k, v in effective.items()]) + '\n',
                                encoding='utf-8')
    extra = dict(flat.get('environment', {}), BB_TEMPORAL='off', BB_HUD_SCALE=hud)
    say(f'{label} at {output}, internal {internal}p ({variant}): menus, backgrounds and a centered HUD')
    return {'config': effective_config, 'patches_dir': patches_dir, 'archive_patches': made, 'env': extra}


def archive_list(base_patches, layout):
    """BB_ARCHIVE_PATCHES: the layout's patches, and base patches for the other archives."""
    patches = dict(base_patches)
    if layout:
        patches.update(layout['archive_patches'])
    return '|'.join(str(Path(p).resolve()) for p in patches.values())


def main():
    """run.sh's launch: KEY=VALUE lines (settings, patches_dir, archive_patches, env.NAME)."""
    import argparse
    p = argparse.ArgumentParser(description='The 21:9 or 32:9 layout of a launch (run.sh).')
    p.add_argument('game')
    p.add_argument('--out', required=True)
    p.add_argument('--settings', required=True, help='bbport.ini')
    p.add_argument('--title-id', default=TITLE_ID)
    p.add_argument('--app-version', default=APP_VERSION)
    p.add_argument('--base', action='append', default=[], help='an archive patch made before (repeatable)')
    a = p.parse_args()
    global MESSAGES
    MESSAGES = sys.stderr
    base = {archive_patch.read(path)[1]: Path(path) for path in a.base}
    layout = prepare(a.game, a.out, a.settings, dict(os.environ), a.title_id, a.app_version, base)
    print(f'settings={layout["config"] if layout else a.settings}')
    print(f'patches_dir={layout["patches_dir"] if layout else ""}')
    print(f'archive_patches={archive_list(base, layout)}')
    for key, value in (layout['env'] if layout else {}).items():
        print(f'env.{key}={value}')


if __name__ == '__main__':
    main()
