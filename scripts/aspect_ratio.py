"""Aspect ratio selection for a launch: 16:9, 16:10, 21:9 or 32:9.

bbport.ini uses aspect_ratio=16x9|16x10|21x9|32x9 and aspect_output=WxH.
The older widescreen_layout / widescreen_output settings are still accepted.
16:10 prepares display/camera patches and its HUD asset corrections here;
the corrected 21:9/32:9 menus and HUD continue through widescreen.py.

GPL-2.0-or-later.
"""
import argparse
import copy
import os
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from struct import error as struct_error

import archive_patch
import aspect_hud
import patches
import widescreen
from nitro_components import read_ini
from widescreen import archive_list

DATA = Path(__file__).resolve().parent / 'aspect_ratio_data'
OUTPUTS = {'16x10': ('1920x1200', '1280x800', '2560x1600'), **widescreen.OUTPUTS}
INTERNAL = ('480', '720', '1080', '1440', '1620', '2160')
TITLE_ID, EU_TITLE, APP_VERSION = 'CUSA13795', 'CUSA14876', '01.21'
MESSAGES = sys.stdout  # run.sh reads shell lines on stdout; messages go to stderr


def say(message):
    print('Aspect ratio: ' + message, file=MESSAGES, flush=True)


def selection(settings):
    """(ratio, output, internal) from saved settings, or None for the default 16:9."""
    if 'aspect_ratio' not in settings:
        return widescreen.selection(settings)
    ratio = settings['aspect_ratio']
    if ratio not in OUTPUTS:
        return None
    output = settings.get('aspect_output', settings.get('widescreen_output', ''))
    if output not in OUTPUTS[ratio]:
        output = OUTPUTS[ratio][0]
    internal = settings.get('render_res', '1080')
    if internal not in INTERNAL:
        internal = '1080'
    return ratio, output, internal


def prepare(game, out, config, env, title_id, app_version, base_patches):
    """The selected aspect ratio's private config, executable/asset patches and environment.

    Return None for 16:9 or an unsupported executable; keep the saved settings unchanged.
    """
    settings = read_ini(config) if Path(config).is_file() else {}
    chosen = selection(settings)
    if chosen is None:
        return None
    ratio, output, internal = chosen
    if ratio != '16x10':
        return widescreen.prepare(game, out, config, env, title_id, app_version, base_patches)
    if app_version != APP_VERSION or title_id not in (TITLE_ID, EU_TITLE):
        say(f'16:10 is made for the 1.21 update ({TITLE_ID} or {EU_TITLE}); this game is {title_id} {app_version}: 16:9')
        return None
    try:
        xml = ET.parse(DATA / '16x10.xml').getroot()
        target = xml.find('./TitleID/ID')
        meta = xml.find('Metadata')
        lines = xml.find('./Metadata/PatchList')
        if (target is None or meta is None or lines is None or len(lines) != 1 or
                meta.get('AppVer') != APP_VERSION or meta.get('AppElf') != 'eboot.bin'):
            raise ValueError('Unexpected 16:10 projection XML')
        target.text = title_id
        # Reuse the output-size loads, not the ultrawide mode's menu/HUD changes.
        # The camera's relative operands stay inside its replacement on both editions.
        for _, _, meta in patches.render_patches({'ultrawide': '1', 'ultrawide_size': output}):
            for line in meta.iter('Line'):
                lines.append(copy.deepcopy(line))
        elf = (Path(out) / 'eboot.elf').read_bytes()
        for line in xml.iter('Line'):
            if not line.get('Signature'):
                raise ValueError(f'{line.get("Address")} has no signature')
            patches.locate(line, int(line.get('Address'), 0) - patches.EBOOT_BASE, elf)
        hud = aspect_hud.prepare(game, out, base_patches)
    except (OSError, ET.ParseError, ValueError, struct_error) as error:
        say(f'16:10 not used ({error}): 16:9')
        return None

    root = Path(out) / 'aspect_ratio'
    patches_dir = root / 'patches'
    patches_dir.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(xml).write(patches_dir / 'AspectRatio.xml', encoding='utf-8', xml_declaration=True)
    effective = dict(render_res='1080', patch_pro_1080p='0', temporal='off', upscaler='off',
                     ultrawide='0', ultrawide_frontend='0', ultrawide_cinematics='0',
                     layout_internal=internal)
    kept = [line for line in Path(config).read_text(encoding='utf-8').splitlines()
            if line.partition('=')[0].strip() not in effective] if Path(config).is_file() else []
    effective_config = root / 'bbport.ini'
    effective_config.write_text('\n'.join(kept + [f'{k}={v}' for k, v in effective.items()]) + '\n',
                                encoding='utf-8')
    say(f'16:10 at {output}, internal {internal}p: experimental display and camera; '
        f'{"corrected collectibles and boost gauge" if hud else "original HUD (no archives)"}')
    return {'config': effective_config, 'patches_dir': patches_dir, 'archive_patches': hud,
            'env': {'BB_TEMPORAL': 'off', 'BB_HUD_SCALE': ''}}


def main():
    """run.sh's KEY=VALUE launch plan, with the same interface as widescreen.py."""
    p = argparse.ArgumentParser(description='Select the aspect ratio of a launch (run.sh).')
    p.add_argument('game')
    p.add_argument('--out', required=True)
    p.add_argument('--settings', required=True, help='bbport.ini')
    p.add_argument('--title-id', default=TITLE_ID)
    p.add_argument('--app-version', default=APP_VERSION)
    p.add_argument('--base', action='append', default=[], help='an existing archive patch (repeatable)')
    a = p.parse_args()
    global MESSAGES
    MESSAGES = sys.stderr
    widescreen.MESSAGES = sys.stderr
    base = {archive_patch.read(path)[1]: Path(path) for path in a.base}
    plan = prepare(a.game, a.out, a.settings, dict(os.environ), a.title_id, a.app_version, base)
    print(f'settings={plan["config"] if plan else a.settings}')
    print(f'patches_dir={plan["patches_dir"] if plan else ""}')
    print(f'archive_patches={archive_list(base, plan)}')
    for key, value in (plan['env'] if plan else {}).items():
        print(f'env.{key}={value}')


if __name__ == '__main__':
    main()
