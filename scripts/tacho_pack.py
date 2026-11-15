"""Pack Tacho (scripts/tacho_pack/, by Tacho; the reserves bar adapts Onur's "Reserves Meter", MIT):
built into the port with his permission. See scripts/tacho_pack/README.txt.

Two halves:
- Executable patches in tacho_pack/patches/ (RaceLeaders_hud.xml, HudOriginal.xml,
  ItemsMejorados.xml, NtropyPosicion.xml, SinItemsPortales.xml, BarraReservas.xml), always applied
  (scripts/patches.py tacho_pack_patches): part of the port, not in the launchers' mod lists.
- Game files (this script): the 8-place race leaders list, the racer HUD and minimap, swapped by
  archive patch (scripts/archive_patch.py), each keeping the base patches of its archive. The
  pack's Power Charged main-menu logo is left out: the main menu is the game's own. A swap applies
  only where the game holds the exact file the pack was made from (USA 1.21); anything else keeps
  the game's own file.

The Graphics tab's update3.pak (scripts/graphics_tab.py) carries its own copy of the main menu,
which the game reads last; it would be rebuilt from a pack copy of a file it carries (none now). Online mode (scripts/online_mode.py)
builds streaming copies of the leaders list from the game's files; stream_replacements() gives
it the pack's.

It is fixed: no launcher or bbport.ini setting turns it off or changes it, so every player
races under the same rules. Its files are checked against SHA256 values written here
(verify()); a changed or missing file stops the launch until the port is installed again.
Its Options > Game entries ("Hud Power Charged", "Barra de reservas", as in Pack Tacho 1.3.0) are
always in: they change only how a player's own HUD looks. Both start off
(src/runtime_reserves_bar.c) and are turned on there, never from the launcher or the overlay.
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
FORMAT = 2
# The pack as shipped: the manifest (which holds every game file's SHA256) and its patches.
EXPECTED = {
    'manifest.json': '0def9a284e171fb1533fddb2115fb75edcb5a64a8f62d8ba1351170bd22bb58f',
    'patches/BarraReservas.xml': '3259ce94fbc6c71a71f913e76fc33f7c4455ec63e0624e192b467415de71bafc',
    'patches/HudOriginal.xml': '1b150e56c06b36619fcca5c49e16f07b5633ce45d96320ea891ecc966c0d621d',
    'patches/ItemsMejorados.xml': 'af2146c67bdde0ccbc27cdbbfc28fbab9d05d64e10d6c7348ef2048506329725',
    'patches/NtropyPosicion.xml': '2d698ba3dd71472f428e1e2df3a8fdc1f37b8049331331986d0828779c2e5115',
    'patches/RaceLeaders_hud.xml': 'ad35f230077e60061518d79c6df0dfd24ff4e790d62e8a10d1620b0bfc03ef0f',
    'patches/SinItemsPortales.xml': '138ed816f5e64ac77b54835364c1eb1be4bee526e10d51dfb8516a2773336d69',
}
# (Its executable patches are listed in scripts/patches.py, TACHO_PACK_FILES.)
# {asset path (lower case): pack bytes} of this launch's swaps, for scripts/online_mode.py.
_ACTIVE = {}


def say(text):
    print(f'Pack Tacho: {text}', flush=True)


class Modified(ValueError):
    """A file of the pack is not the one shipped: the launch stops (run.py)."""


def verify():
    """Raises Modified when the manifest or a patch differs from the shipped one (the manifest
    pins every game file's SHA256, which payload() checks)."""
    for name, digest in EXPECTED.items():
        path = FOLDER / name
        try:
            # LF line ends, as git stores them: a Windows checkout has CRLF in these text files.
            actual = hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()
        except OSError:
            actual = None
        if actual != digest:
            raise Modified(f'scripts/tacho_pack/{name} is missing or was changed; install the port again')


def manifest():
    verify()
    return json.loads((FOLDER / 'manifest.json').read_text(encoding='utf-8'))


def operations():
    """Every game file of the pack, its Options > Game page included (Pack Tacho 1.3.0)."""
    m = manifest()
    return m['operations'] + m['menu_operations']


def payload(op):
    try:
        data = (FOLDER / op['payload']).read_bytes()
    except OSError:
        data = None
    if data is None or hashlib.sha256(data).hexdigest() != op['patched_sha256']:
        raise Modified(f'scripts/tacho_pack/{op["payload"]} is missing or was changed; install the port again')
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
    """({guest archive: patch file}, {environment overrides}). Raises Modified when the pack's
    files were changed. config is unused: the pack has no settings."""
    _ACTIVE.clear()
    ops = operations()
    folder = Path(out) / 'tacho-pack'
    names = sorted({op['archive'] for op in ops})
    archives = [resolve(Path(game) / 'archives' / n) for n in names]
    stamp = {'format': FORMAT, 'script': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
             'manifest': hashlib.sha256((FOLDER / 'manifest.json').read_bytes()).hexdigest(),
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
    say(f'{len(chosen)} of {len(ops)} game files')
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
