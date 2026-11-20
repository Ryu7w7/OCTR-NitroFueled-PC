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
import octr_texts
from archive_reader import Archive
from mod_file_paths import resolve

FOLDER = Path(__file__).resolve().parent / 'tacho_pack'
FORMAT = 3
# The pack as shipped: the manifest (which holds every game file's SHA256) and its patches.
EXPECTED = {
    'manifest.json': '0def9a284e171fb1533fddb2115fb75edcb5a64a8f62d8ba1351170bd22bb58f',
    'patches/BarraReservas.xml': '3259ce94fbc6c71a71f913e76fc33f7c4455ec63e0624e192b467415de71bafc',
    'patches/HudOriginal.xml': '1b150e56c06b36619fcca5c49e16f07b5633ce45d96320ea891ecc966c0d621d',
    'patches/ItemsMejorados.xml': 'af2146c67bdde0ccbc27cdbbfc28fbab9d05d64e10d6c7348ef2048506329725',
    'patches/NtropyPosicion.xml': '2d698ba3dd71472f428e1e2df3a8fdc1f37b8049331331986d0828779c2e5115',
    'patches/RaceLeaders_hud.xml': 'ad35f230077e60061518d79c6df0dfd24ff4e790d62e8a10d1620b0bfc03ef0f',
    'patches/SinItemsPortales.xml': '138ed816f5e64ac77b54835364c1eb1be4bee526e10d51dfb8516a2773336d69',
    # The European game's (CUSA14876 1.21) copies, made by tools/port_patch_eu.py from the ones above.
    'patches_eu/BarraReservas.xml': '770c13fd92323b99b5f74b0e1ae8a60a21f7bb692bdfb33298fd1bf737098215',
    'patches_eu/HudOriginal.xml': 'cc263509034bba16b049313b18f37eb99721bb510daabd13bfe9655058b78b13',
    'patches_eu/ItemsMejorados.xml': '85e5c69791ea9b4aecb89100559e55447b031a1ae61f0a2e6e2a1e7b7b536ca8',
    'patches_eu/NtropyPosicion.xml': '24d68d00ad821047cd0e9bddc2d6d90adac5570c9e16f53b846f34fb5ca20a6a',
    'patches_eu/RaceLeaders_hud.xml': 'f1e3961658663fc2e432b1271c9cb725a7a37bb98f7efb51576ea3c415538ae9',
    'patches_eu/SinItemsPortales.xml': '10a6a07a0d04a5712801f8c9ab283227587de0cd342766575dc98e87ebc348e7',
}
# The European game's (CUSA14876 1.21) own copies of the files the pack changes where they are not the
# USA ones (the others are the same files at other indices). They differ only in their language
# dependencies (TDEP, the EU text tables): the pack's file goes in with those.
EU_ORIGINALS = {
    'ui/octane_racer_hud.igz': 'd9233794a00ae2648f85684caf95dff49e3e337fced678307b1ddc1437a9f309',
    'ui/octane_options_menu_page.igz': '38b232d6488db220d497c807313319da9140c36a1cc7c3a25cf078ff57a1cbaf',
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


# The pack's options page (Options > Game) has its texts in Spanish: in the game's language here.
OPTIONS_PAGE = 'ui/octane_options_menu_page.igz'
OPTION_TEXTS = {
    'Barra de reservas': 'Reserves bar',
    'Sí: muestra tus reservas de turbo debajo de la barra de turbo. No: la oculta.':
        'Yes: shows your boost reserves under the boost bar. No: hides it.',
    'Sí: lista de 8 con nombre, wumpas e ítem. No: solo los 4 primeros, sin extras.':
        'Yes: 8-place list with name, wumpas and item. No: only the top 4, no extras.',
    'Sí': 'Yes',   # the options' own values (20/11: "Some of the Yes options come up in Spanish")
    'No': 'No',
}


def fits(op, own):
    """The game's own file is the one the pack was made from (USA) or its European copy."""
    digest = hashlib.sha256(own).hexdigest()
    return digest == op['original_sha256'] or digest == EU_ORIGINALS.get(op['asset'].lower())


def localized(op, own=None):
    """The payload as the game gets it: the options page's texts in the game's language, and on the
    European game (own: its copy, one of EU_ORIGINALS) with that copy's dependencies."""
    data = payload(op)
    column = octr_texts.current_column()
    rebase = own is not None and hashlib.sha256(own).hexdigest() == EU_ORIGINALS.get(op['asset'].lower())
    translate = op['asset'].lower() == OPTIONS_PAGE and column != 'mx'
    if not rebase and not translate:
        return data
    from igz_file import Igz
    page = Igz(data)
    if rebase:
        theirs = Igz(own).fixups
        page.fixups = [t if t[0] == 'TDEP' else m for t, m in zip(theirs, page.fixups)]
    if translate:
        for i, original in enumerate(page.strings):
            english = OPTION_TEXTS.get(original)
            if english:
                page.strings[i] = octr_texts.text(english, None if column == 'en' else column)
    return page.build()


def entry_of(archive, op):
    """The index of op's asset in the game's archive: the pack's (USA) one, or found by its path."""
    files = archive.files
    if op['index'] < len(files) and files[op['index']]['path'].lower() == op['asset'].lower():
        return op['index']
    return next((i for i, f in enumerate(files) if f['path'].lower() == op['asset'].lower()), None)


def stream_replacements():
    """{asset path: bytes} the pack swapped in this launch (none when it is off)."""
    return dict(_ACTIVE)


def _swaps(game, ops, base):
    """({archive file: {index: bytes}}, [(op, bytes)]) of the swaps that fit this game; prints the
    others."""
    found, chosen = {}, []
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
                index = entry_of(archive, op)
                own = archive.extract(archive.files[index]) if index is not None else None
                if own is None or not fits(op, own):
                    say(f'{op["asset"]} in {name} is not the file the pack was made for; the game\'s own')
                    continue
                if 0x38 + 4 * count + 16 * index in taken:
                    say(f'{op["asset"]} in {name} is already changed by another setting; that one stays')
                    continue
                data = localized(op, own)
                found.setdefault(path, {})[index] = data
                chosen.append((op, data))
        finally:
            archive.data.close(); archive._file.close()
    return found, chosen


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
             'base': {g: hashlib.sha256(Path(p).read_bytes()).hexdigest() for g, p in sorted(base.items())},
             'language': octr_texts.current_column()}
    stamp_file = folder / 'stamp.json'
    made = None
    try:
        kept = json.loads(stamp_file.read_text(encoding='utf-8'))
        if kept['stamp'] == stamp and all(Path(p).is_file() for p in kept['patches'].values()) and \
                all((folder / 'applied' / c).is_file() for _, _, c in kept['applied']):
            made = {g: Path(p) for g, p in kept['patches'].items()}
            applied = kept['applied']
    except (OSError, ValueError, KeyError):
        pass
    if made is None:
        made, applied = {}, []
        folder.mkdir(parents=True, exist_ok=True)
        swaps, picked = _swaps(game, ops, base)
        for path, entries in swaps.items():
            guest = f'/app0/archives/{path.name}'
            target = folder / (path.stem + '.bbpatch')
            archive_patch.write_entries_patch(target, path, guest, entries, base.get(guest))
            made[guest] = target
        # each swap's bytes as the game gets them, for the streaming copies (scripts/online_mode.py)
        for op, data in picked:
            copy = folder / 'applied' / hashlib.sha256(data).hexdigest()
            copy.parent.mkdir(parents=True, exist_ok=True)
            copy.write_bytes(data)
            applied.append([op['archive'], op['asset'], copy.name])
        stamp_file.write_text(json.dumps({'stamp': stamp, 'patches': {g: str(p) for g, p in made.items()},
                                          'applied': applied}), encoding='utf-8')
    by_asset = {(op['archive'], op['asset']): op for op in ops}
    chosen, copies = [], {}
    for name, asset, copy in applied:
        if (name, asset) in by_asset:
            chosen.append(by_asset[(name, asset)])
            copies[(name, asset)] = folder / 'applied' / copy
    # The game reads update2.pak over update.pak: the newest archive's copy is the one in use.
    order = {'update2.pak': 0, 'update.pak': 1}
    for op in sorted(chosen, key=lambda o: order.get(o['archive'], 2)):
        _ACTIVE.setdefault(op['asset'].lower(), copies[(op['archive'], op['asset'])].read_bytes())
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
                                     'language': octr_texts.current_column(),
                                     'operations': sorted(relevant.values(), key=lambda o: (o['archive'], o['index']))},
                                    sort_keys=True).encode()).hexdigest()[:16]
    target = Path(out) / 'tacho-pack' / 'graphics-tab' / key / 'update3.pak'
    if not target.is_file():
        def transform(archive, logical, data):
            op = relevant.get((archive.casefold(), logical.casefold()))
            return localized(op, data) if op else data
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
