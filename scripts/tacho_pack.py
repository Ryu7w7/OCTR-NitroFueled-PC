"""Pack Tacho (scripts/tacho_pack/, by Tacho; the reserves bar adapts Onur's "Reserves Meter", MIT):
built into the port with his permission. See scripts/tacho_pack/README.txt.

Two halves:
- Executable patches in patches/ (RaceLeaders_hud.xml, HudOriginal.xml, ItemsMejorados.xml,
  NtropyPosicion.xml, SinItemsPortales.xml, BarraReservas.xml), on by default like any bundled
  patch with isEnabled (scripts/patches.py; the launcher's Patches page turns each off).
- Game files (this script): the 8-place race leaders list, the racer HUD and minimap, and the main
  menu's logo and backgrounds, swapped by archive patch (scripts/archive_patch.py), each keeping
  the base patches of its archive. A swap applies only where the game holds the exact file the
  pack was made from (USA 1.21); anything else keeps the game's own file.

The Graphics tab's update3.pak (scripts/graphics_tab.py) carries its own copy of the main menu,
which the game reads last, so it is rebuilt from the pack's menu. Online mode (scripts/online_mode.py)
builds streaming copies of the leaders list from the game's files; stream_replacements() gives
it the pack's.

bbport.ini tacho_pack = 0 (or BB_TACHO_PACK=0) leaves the whole pack out, patches included.
tacho_pack_menu = 1 adds the pack's "Hud Power Charged" and "Barra de reservas" entries to
Options > Game (another Options page; without it the new HUD and the bar are always on).
SPDX-License-Identifier: GPL-2.0-or-later
"""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path

import archive_patch
from archive_reader import Archive
from mod_file_paths import resolve

FOLDER = Path(__file__).resolve().parent / 'tacho_pack'
OFF = ('0', 'off', 'no', 'false')
FORMAT = 1
# (Its executable patches are listed in scripts/patches.py, TACHO_PACK_FILES.)
# {asset path (lower case): pack bytes} of this launch's swaps, for scripts/online_mode.py.
_ACTIVE = {}


def say(text):
    print(f'Pack Tacho: {text}', flush=True)


def _setting(config, env, key, variable, default):
    value = env.get(variable, '') if env is not None else ''
    if not value and config and Path(config).is_file():
        for line in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
            name, sep, setting = line.partition('=')
            if sep and name.strip() == key:
                value = setting.strip()
    return (value or default).strip().lower()


def wanted(config, env=None):
    return _setting(config, env, 'tacho_pack', 'BB_TACHO_PACK', '1') not in OFF


def menu_wanted(config, env=None):
    return _setting(config, env, 'tacho_pack_menu', 'BB_TACHO_PACK_MENU', '0') not in OFF


def manifest():
    return json.loads((FOLDER / 'manifest.json').read_text(encoding='utf-8'))


def operations(config, env=None):
    m = manifest()
    return m['operations'] + (m['menu_operations'] if menu_wanted(config, env) else [])


def payload(op):
    data = (FOLDER / op['payload']).read_bytes()
    if hashlib.sha256(data).hexdigest() != op['patched_sha256']:
        raise ValueError(f'damaged file {op["payload"]}')
    return data


def stream_replacements():
    """{asset path: bytes} the pack swapped in this launch (none when it is off)."""
    return dict(_ACTIVE)


def _swaps(game, ops, base):
    """{archive file: {index: bytes}} of the swaps that fit this game; prints the others."""
    found = {}
    for name in sorted({op['archive'] for op in ops}):
        path = resolve(Path(game) / 'archives' / name)
        if not path.is_file():
            say(f'{name} is not in the game; its files stay the game\'s')
            continue
        guest = f'/app0/archives/{name}'
        # Entries a base patch already replaces (its 16-byte records, as scripts/mod_overlays.py).
        taken = {o for o, d in archive_patch.read(base[guest])[2] if len(d) == 16} if base.get(guest) else set()
        archive = Archive(path)
        count = len(archive.files)
        try:
            for op in (o for o in ops if o['archive'] == name):
                entry = archive.files[op['index']] if op['index'] < len(archive.files) else None
                if entry is None or entry['path'] != op['asset'] or \
                        hashlib.sha256(archive.extract(entry)).hexdigest() != op['original_sha256']:
                    say(f'{op["asset"]} in {name} is not the file the pack was made for; the game\'s own')
                    continue
                if 0x38 + 4 * count + 16 * op['index'] in taken:
                    say(f'{op["asset"]} in {name} is already changed by another setting; that one stays')
                    continue
                found.setdefault(path, {})[op['index']] = payload(op)
        finally:
            archive.data.close(); archive._file.close()
    return found


def prepare(game, out, config, env, base):
    """({guest archive: patch file}, {environment overrides}); nothing when the pack is off."""
    _ACTIVE.clear()
    if not wanted(config, env) or not (FOLDER / 'manifest.json').is_file():
        return {}, {}
    ops = operations(config, env)
    folder = Path(out) / 'tacho-pack'
    names = sorted({op['archive'] for op in ops})
    archives = [resolve(Path(game) / 'archives' / n) for n in names]
    stamp = {'format': FORMAT, 'script': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
             'manifest': hashlib.sha256((FOLDER / 'manifest.json').read_bytes()).hexdigest(),
             'menu': menu_wanted(config, env),
             'archives': {a.name: [a.stat().st_size, a.stat().st_mtime_ns] for a in archives if a.is_file()},
             'base': {g: hashlib.sha256(Path(p).read_bytes()).hexdigest() for g, p in sorted(base.items())}}
    stamp_file = folder / 'stamp.json'
    made = None
    try:
        kept = json.loads(stamp_file.read_text(encoding='utf-8'))
        if kept['stamp'] == stamp and all(Path(p).is_file() for p in kept['patches'].values()):
            made = {g: Path(p) for g, p in kept['patches'].items()}
            applied = kept['applied']
    except (OSError, ValueError, KeyError):
        pass
    if made is None:
        made, applied = {}, []
        folder.mkdir(parents=True, exist_ok=True)
        for path, entries in _swaps(game, ops, base).items():
            guest = f'/app0/archives/{path.name}'
            target = folder / (path.stem + '.bbpatch')
            archive_patch.write_entries_patch(target, path, guest, entries, base.get(guest))
            made[guest] = target
            applied += [[path.name, index] for index in sorted(entries)]
        stamp_file.write_text(json.dumps({'stamp': stamp, 'patches': {g: str(p) for g, p in made.items()},
                                          'applied': applied}), encoding='utf-8')
    by_place = {(op['archive'], op['index']): op for op in ops}
    chosen = [by_place[(name, index)] for name, index in applied if (name, index) in by_place]
    # The game reads update2.pak over update.pak: the newest archive's copy is the one in use.
    order = {'update2.pak': 0, 'update.pak': 1}
    for op in sorted(chosen, key=lambda o: order.get(o['archive'], 2)):
        _ACTIVE.setdefault(op['asset'].lower(), payload(op))
    say(f'{len(chosen)} of {len(ops)} game files' + (' (Options menu entries)' if menu_wanted(config, env) else ''))
    return made, graphics_tab_override(game, out, env, chosen)


def graphics_tab_override(game, out, env, chosen):
    """The Graphics tab's update3.pak rebuilt from the pack's copies of the files it carries
    (the main menu): {'BB_GRAPHICS_TAB_ARCHIVE': file}, or nothing."""
    source = env.get('BB_GRAPHICS_TAB_ARCHIVE')
    if not source or not Path(source).is_file():
        return {}
    import graphics_tab
    carried = set(graphics_tab.CHANGES) | set(graphics_tab.OPTIONAL) | set(graphics_tab.WIDGET_RESOURCES)
    relevant = {(op['archive'].casefold(), op['asset'].casefold()): op for op in chosen
                if op['asset'].casefold() in carried}
    if not relevant:
        return {}
    generated = hashlib.sha256(Path(source).read_bytes()).hexdigest()
    key = hashlib.sha256(json.dumps({'format': FORMAT, 'generated': generated,
                                     'operations': sorted(relevant.values(), key=lambda o: (o['archive'], o['index']))},
                                    sort_keys=True).encode()).hexdigest()[:16]
    target = Path(out) / 'tacho-pack' / 'graphics-tab' / key / 'update3.pak'
    if not target.is_file():
        def transform(archive, logical, data):
            op = relevant.get((archive.casefold(), logical.casefold()))
            return payload(op) if op else data
        target.parent.mkdir(parents=True, exist_ok=True)
        temp = target.with_suffix('.tmp')
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                graphics_tab.build(Path(game), temp, target.parent / 'dump', asset_transform=transform)
            os.replace(temp, target)
        except (OSError, ValueError) as error:
            say(f'Graphics tab kept the game\'s main menu ({error})')
            return {}
        finally:
            temp.unlink(missing_ok=True)
        from cache_cleanup import discard_files
        discard_files(target.parent.parent, [p for p in target.parent.parent.iterdir()
                                             if p.is_dir() and p != target.parent])
    return {'BB_GRAPHICS_TAB_ARCHIVE': str(target.resolve())}
