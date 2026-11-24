# SPDX-License-Identifier: GPL-2.0-or-later
"""The built-in mods next to community mods: merge when possible, else they step aside.

The built-in mods (scripts/builtin_mods: Ultimate Customizer, Custom Cup) are on by default, so a
community mod must never be blocked by one of them. When an enabled community mod changes what a
built-in also changes:

1. A game file (an entry of a .pak): the built-in's change is merged over the community mod's
   version (scripts/igz_rebase.py), object by object. A community mod's archive_asset operation
   on the same entry becomes one merged operation; a whole archive of a mod (a racer mod's
   update3.pak with its systemData.igz, a classic track mod's) is written again with the merged
   entry. Texts (localized_strings) go over the built-in's version as they are.
2. When that merge cannot be done (both change the same object, ...):
   - the online lobby menu (ui/Octane_Lobby_PrivateGameSettings.igz): the built-in leaves only
     its change of that file, so a mod can change the lobby;
   - any other file: the built-in is frozen for this session: it is not applied, the launch goes
     on, and it comes back by itself when the other mod is turned off.
3. The executable: two patches writing the same code cannot be merged; the built-in is frozen.
4. A mod that the built-in declares incompatible (or that declares it incompatible): frozen.

A frozen Ultimate Customizer takes its Copas Adicionales' pictures (extra-cups-art) with it.
"""
import contextlib
import io
import re
import xml.etree.ElementTree as ET
from pathlib import Path

from mod_components import payload_bytes, sha_bytes, under

LOBBY = 'ui/octane_lobby_privategamesettings.igz'
UC, ART = 'battle-point-limit-selector', 'extra-cups-art'
UPDATE = re.compile(r'archives/update\d*\.pak\Z', re.I)
LARGEST = 512 * 1024 ** 2  # a whole archive larger than this is not written again to merge


class Plan:
    """What the built-ins do this session."""

    def __init__(self):
        self.frozen = {}    # built-in -> why
        self.yielded = {}   # built-in -> [assets it leaves]
        self.merged = []    # 'asset: built-in + mod'
        self.drop = set()   # id() of operations not applied (merged into others, yielded)
        self.add = []       # (operation, folder) merged operations
        self.archives = {}  # target -> {logical path: bytes} entries to write again

    def freeze(self, name, why):
        self.frozen.setdefault(name, why)
        if name == UC:
            self.frozen.setdefault(ART, 'Ultimate Customizer is frozen')

    def report(self):
        return {'frozen': self.frozen, 'yielded': self.yielded, 'merged': self.merged}


# --- declared incompatibilities and executable patches --------------------------------------

def declared(plan, builtins, manifests):
    """A built-in and a mod one of them declares incompatible."""
    for b in builtins:
        for name, m in manifests.items():
            if name in builtins:
                continue
            if b in m.get('conflicts', []) or name in manifests[b].get('conflicts', []):
                plan.freeze(b, name + ' is a mod of the same kind')


def patch_writes(game, owners):
    """{owner: [(start, end)]} of the executable writes of each owner's patches.
    owners: {owner: [(key, path, metadata)]}. A patch that does not compile has none here (the
    launch's own check reports it)."""
    from prepare import parse_self
    from patches import compile_external, eboot_segments
    elf, *_ = parse_self((Path(game) / 'eboot.bin').read_bytes())
    segments = eboot_segments(elf)
    found = {}
    for owner, selected in owners.items():
        ranges = []
        for item in selected:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                writes = compile_external([item], segments, elf)
            ranges += [(o, o + len(d)) for o, d in writes]
        found[owner] = ranges
    return found


def xml_patches(path, name):
    return [(name + ': ' + meta.get('Name', ''), path, meta) for meta in ET.parse(path).getroot().iter('Metadata')]


def code(plan, game, builtins, package_patches, external):
    """Built-ins whose executable writes overlap a community mod's or another selected patch's.
    package_patches: {mod: [patch xml paths]}; external: [(key, path, metadata)]."""
    owners = {n: [i for p in paths for i in xml_patches(p, n)] for n, paths in package_patches.items()}
    for key, path, meta in external:
        owners.setdefault(key, []).append((key, path, meta))
    if not any(owners.get(b) for b in builtins) or len(owners) < 2:
        return
    writes = patch_writes(game, owners)
    for b in builtins:
        if b in plan.frozen:
            continue
        for other, ranges in writes.items():
            if other in builtins:
                continue
            if any(s < e2 and s2 < e for s, e in writes.get(b, []) for s2, e2 in ranges):
                plan.freeze(b, other + ' changes the same game code')
                break


# --- game files -----------------------------------------------------------------------------

class Entries:
    """The game's copy of archive entries, read once."""

    def __init__(self, game):
        self.game = Path(game)
        self.found = {}

    def stock(self, op):
        key = (op['archive'], op['index'])
        if key not in self.found:
            from archive_reader import Archive
            archive = Archive(under(self.game, 'archives/' + op['archive']))
            try:
                entry = archive.files[op['index']]
                if entry['path'] != op['asset']:
                    raise ValueError('Archive asset/index mismatch: ' + op['asset'])
                self.found[key] = archive.extract(entry)
            finally:
                archive.data.close()
                archive._file.close()
        return self.found[key]


def archive_entries(path):
    """{logical path (casefolded): (logical path, bytes)} of an archive."""
    from racer_registry import read_archive
    return {k.casefold(): (k, v) for k, v in read_archive(path).items()}


def merge_builtins(store, game, builtins, log=print):
    """Built-ins changing one archive entry (Custom Cup's race menu thumbnails and Ultimate
    Customizer's lap list, both in ui/Octane_MainMenu_RaceSetting.igz): each later change is
    rebased over the first one's version, as one operation of the first built-in.

    builtins: [(name, [(operation, folder)])] in load order. Returns ({id(operation): (merged
    operation, folder)}, {id(operation) left out}); a change that cannot be merged is left out
    (and logged) and the first built-in's version stays."""
    from igz_rebase import Unmergeable, rebase
    entries = Entries(game)
    first, swap, gone = {}, {}, set()
    for name, operations in builtins:
        for op, folder in operations:
            if op['kind'] != 'archive_asset':
                continue
            key = (op['archive'].casefold(), op['asset'].casefold())
            if key not in first:
                first[key] = (name, op, folder)
                continue
            owner, base, base_folder = first[key]
            stock = entries.stock(op)
            current, current_folder = swap.get(id(base), (base, base_folder))
            try:
                result = rebase(stock, payload_bytes(op, folder, stock), payload_bytes(current, current_folder, stock))
            except Unmergeable as error:
                gone.add(id(op))
                log(f'OCTR mods: {name} leaves out its change of {op["asset"]}: {owner} changes it too ({error})')
                continue
            digest = sha_bytes(result)
            target = under(store, digest + '.igz')
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.is_file():
                target.write_bytes(result)
            swap[id(base)] = ({'kind': 'archive_asset', 'archive': base['archive'], 'asset': base['asset'],
                               'index': base['index'], 'original_sha256': base['original_sha256'],
                               'patched_sha256': digest, 'payload': digest + '.igz'}, Path(store))
            gone.add(id(op))
            log('OCTR mods: merged ' + op['asset'] + ': ' + owner + ' + ' + name)
    return swap, gone


def merge_entries(plan, store, game, builtins, community, archives, log=print):
    """Merges of the built-ins' archive_asset operations with the community mods' versions of
    those entries.

    builtins: {name: [(operation, folder)]} (their resolved operations); community: [(mod,
    operation, folder)] of the community mods' archive operations; archives: [(owner, target,
    path)] of the whole archives the game loads from the community mods (in load order: a racer
    registration, a classic mod's update3.pak). store: the folder for merged entries."""
    from igz_rebase import Unmergeable, rebase
    entries = Entries(game)
    read = {}
    for name, operations in builtins.items():
        if name in plan.frozen:
            continue
        drop, add, written, merged, yielded, why = set(), [], {}, [], [], None
        for op, folder in operations:
            if op['kind'] != 'archive_asset':
                continue
            key = (op['archive'].casefold(), op['asset'].casefold())
            actions, failed = [], None
            stock = ours = None
            for mod, other, other_folder in community:
                if (other['archive'].casefold(), other['asset'].casefold()) != key:
                    continue
                if other['kind'] == 'localized_strings':
                    merged.append(op['asset'] + ': ' + name + ' + ' + mod + ' (texts)')
                    continue
                if stock is None:
                    stock = entries.stock(op)
                    ours = payload_bytes(op, folder, stock)
                try:
                    result = rebase(stock, ours, payload_bytes(other, other_folder, stock))
                except Unmergeable as error:
                    failed = (mod, str(error))
                    break
                actions.append(('op', other, result, mod))
            for owner, target, path in archives if failed is None else ():
                same = target.casefold() == 'archives/' + key[0]
                if not (same or UPDATE.match(target)):
                    continue
                if path not in read:
                    read[path] = archive_entries(path) if Path(path).stat().st_size <= LARGEST else None
                if read[path] is None:
                    if same:
                        failed = (owner, 'it replaces the whole ' + op['archive'])
                        break
                    continue
                current = written.get(target, {})
                logical, theirs = read[path].get(key[1], (None, None))
                if logical in current:
                    continue  # merged already, by another operation of this entry
                if theirs is None:
                    if same:
                        failed = (owner, 'its ' + op['archive'] + ' has no ' + op['asset'])
                        break
                    continue
                if stock is None:
                    stock = entries.stock(op)
                    ours = payload_bytes(op, folder, stock)
                if theirs == stock and not same:
                    continue  # an unchanged copy of the game's
                try:
                    result = rebase(stock, ours, theirs)
                except Unmergeable as error:
                    failed = (owner, str(error))
                    break
                actions.append(('archive', target, logical, result, owner, same))
            if failed is not None:
                if key[1] == LOBBY:
                    yielded.append(op['asset'])
                    drop.add(id(op))
                    continue
                why = failed[0] + ' changes ' + op['asset'] + ' (' + failed[1] + ')'
                break
            for action in actions:
                if action[0] == 'op':
                    _, other, result, mod = action
                    digest = sha_bytes(result)
                    under(store, digest + '.igz').parent.mkdir(parents=True, exist_ok=True)
                    if not under(store, digest + '.igz').is_file():
                        under(store, digest + '.igz').write_bytes(result)
                    add.append(({'kind': 'archive_asset', 'archive': op['archive'], 'asset': op['asset'],
                                 'index': op['index'], 'original_sha256': op['original_sha256'],
                                 'patched_sha256': digest, 'payload': digest + '.igz'}, Path(store)))
                    drop.update((id(op), id(other)))
                    merged.append(op['asset'] + ': ' + name + ' + ' + mod)
                else:
                    _, target, logical, result, owner, same = action
                    written.setdefault(target, {})[logical] = result
                    if same:
                        drop.add(id(op))
                    merged.append(op['asset'] + ': ' + name + ' + ' + owner + ' (' + target + ')')
        if why is not None:
            plan.freeze(name, why)
            continue
        plan.drop |= drop
        plan.add += add
        for target, items in written.items():
            plan.archives.setdefault(target, {}).update(items)
        plan.merged += merged
        if yielded:
            plan.yielded[name] = yielded
    for name, why in plan.frozen.items():
        log('OCTR mods: ' + name + ' is frozen this session: ' + why)
    for name, assets in plan.yielded.items():
        log('OCTR mods: ' + name + ' leaves ' + ', '.join(assets) + ' to the other mods')
    for line in plan.merged:
        log('OCTR mods: merged ' + line)


def write_archives(plan, files, sources):
    """The community archives with merged entries, written to files/<target>.
    sources: {target: the archive the game would load}."""
    from graphics_tab import Archive, write_archive
    for target, items in plan.archives.items():
        source = Path(sources[target])
        archive = Archive(source)
        try:
            out = []
            for i, (full, logical) in enumerate(archive.names):
                path = archive.logical(i)
                data = items.get(path, archive.read(i))
                out.append((full, logical, archive.infos[i][1] & ~0xFF, data))
        finally:
            archive.f.close()
        dest = under(files, target)
        dest.parent.mkdir(parents=True, exist_ok=True)
        template = Archive(source)
        template.f.close()
        write_archive(dest, out, template)


def write_status(path, report):
    """The launcher's view of this session (mod-state/builtin-status.txt), shown on the built-ins'
    cards: one line per fact, kind<TAB>built-in<TAB>text (frozen: why; yielded: the file it left;
    merged: the file and the other mod)."""
    lines = ['frozen\t%s\t%s' % item for item in report['frozen'].items()]
    lines += ['yielded\t%s\t%s' % (name, asset) for name, assets in report['yielded'].items() for asset in assets]
    for text in report['merged']:
        asset, rest = text.split(': ', 1)
        name, other = rest.split(' + ', 1)
        lines.append('merged\t%s\t%s: %s' % (name, asset, other))
    Path(path).write_text(''.join(line.replace('\n', ' ') + '\n' for line in lines), encoding='utf-8')
