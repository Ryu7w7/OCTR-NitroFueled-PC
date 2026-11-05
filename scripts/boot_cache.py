"""Verified boot preparation shared by run.py and run.sh; original game files stay untouched."""
import argparse
from contextlib import redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

import game_check
import launch_lock
import link_libc
import link_modules
import prepare
from mod_components import long_path

SCHEMA = 1
MODULES = ('libc.prx', 'libSceFios2.prx', 'libSceNpToolkit2.prx')
GENERATORS = ('boot_cache.py', 'prepare.py', 'game_check.py', 'link_libc.py', 'link_modules.py', 'mod_components.py')
OUTPUTS = ('boot.bin', 'eboot.elf', 'entry.bin', 'profile.txt', 'analysis.json',
           'boot-libc.bin', 'libc-link.json', 'boot-linked.bin', 'link.json')
MAGICS = {'boot.bin': b'BBPROBE2', 'boot-libc.bin': b'BBPROBE4',
          'boot-linked.bin': b'BBPROBE5', 'eboot.elf': b'\x7fELF'}
KEY = re.compile(r'[0-9a-f]{64}')
KEEP_PER_TITLE = 2


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def linked(path):
    return path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction())


def fingerprint(game):
    files = ('eboot.bin', 'sce_sys/param.sfo', *('sce_module/' + name for name in MODULES))
    inputs = {name: digest(game / name) for name in files}
    scripts = Path(__file__).resolve().parent
    value = dict(schema=SCHEMA, inputs=inputs, modules=list(MODULES),
                 generators={name: digest(scripts / name) for name in GENERATORS},
                 skip_game_check=os.environ.get('BB_SKIP_GAME_CHECK') == '1')
    key = hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    return key, value


def output_records(folder):
    records = {}
    for name in OUTPUTS:
        path = folder / name
        if linked(path) or not path.is_file():
            raise ValueError('Missing or linked boot output: ' + name)
        size = path.stat().st_size
        if not size:
            raise ValueError('Empty boot output: ' + name)
        if name in MAGICS:
            with path.open('rb') as stream:
                if stream.read(len(MAGICS[name])) != MAGICS[name]:
                    raise ValueError('Invalid boot header: ' + name)
        records[name] = dict(size=size, sha256=digest(path))
    return records


def valid(folder, inputs):
    try:
        if linked(folder) or linked(folder / 'manifest.json'):
            return False
        manifest = json.loads((folder / 'manifest.json').read_text(encoding='utf-8'))
        return (manifest['inputs'] == inputs and manifest['outputs'] == output_records(folder))
    except PermissionError:
        raise  # An unreadable file is not a reason to overwrite a potentially good cache.
    except (OSError, ValueError, KeyError, TypeError):
        return False


def build(game, target):
    prepare.prepare(game, target)
    link_libc.link(game, target)
    link_modules.link(game, target, MODULES)
    # Asset inventories are diagnostics at build time, not inputs to executable linking.
    report = target / 'analysis.json'
    value = json.loads(report.read_text(encoding='utf-8'))
    value['resource_inventory_scope'] = 'Snapshot at boot preparation; not a live inventory of mods or assets.'
    report.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def prune(region, current):
    """Only managed fingerprints, under the full launch lock; keep two builds per edition."""
    candidates = []
    for folder in region.iterdir():
        if (folder != current and KEY.fullmatch(folder.name) and folder.is_dir()
                and not linked(folder) and not linked(folder / 'manifest.json')
                and (folder / 'manifest.json').is_file()):
            try:
                manifest = json.loads((folder / 'manifest.json').read_text(encoding='utf-8'))
                owned_key = hashlib.sha256(json.dumps(manifest['inputs'], sort_keys=True,
                                                     separators=(',', ':')).encode()).hexdigest()
                if owned_key == folder.name and set(manifest['outputs']) == set(OUTPUTS):
                    candidates.append(folder)
            except (OSError, ValueError, KeyError, TypeError):
                continue
    candidates.sort(key=lambda p: p.stat().st_mtime_ns, reverse=True)
    for folder in candidates[KEEP_PER_TITLE - 1:]:
        shutil.rmtree(folder)


def ensure(game, out, log=print):
    game, out = long_path(Path(game).resolve()), long_path(Path(out).resolve())
    # Reentrant in run.py; run.sh inherits its OS lock through exec. Standalone CLI is protected too.
    with launch_lock.output_session(out):
        key, inputs = fingerprint(game)
        title = game_check.sfo_info(game).get('TITLE_ID', 'unknown')
        if title not in ('CUSA13795', 'CUSA14876'):
            title = 'unknown'  # Validated by prepare; never use untrusted SFO text as a path.
        # Archive damage is checked live, even though assets do not invalidate executable linking.
        if not inputs['skip_game_check'] and title not in game_check.EXPERIMENTAL:
            if game_check.damaged_archives(game):
                raise prepare.GameCheckError(game_check.explain(
                    'damaged_archives', title, game_check.sfo_info(game).get('APP_VER', '?')))
        root = out / 'boot-cache'
        region = root / title
        if linked(root) or linked(region):
            raise ValueError('Boot cache directory must not be a symlink')
        region.mkdir(parents=True, exist_ok=True)
        target = region / key
        if linked(target):
            raise ValueError('Boot cache entry must not be a symlink or junction')
        if valid(target, inputs):
            log('Boot cache: hit (' + title + '): ' + str(target))
            return target
        log('Boot cache: rebuilding (' + title + '; inputs changed or output missing/invalid)')
        # A killed build leaves only staging. No reader uses it and the session lock excludes writers.
        for stale in region.glob('.building-*'):
            if stale.is_dir() and not linked(stale):
                try:
                    shutil.rmtree(stale)
                except OSError as error:
                    log('Boot cache: stale staging cleanup deferred (' + str(error) + ')')
        staging = Path(tempfile.mkdtemp(prefix='.building-', dir=region))
        try:
            messages = io.StringIO()
            try:
                with redirect_stdout(messages):
                    build(game, staging)
            finally:
                for line in messages.getvalue().splitlines():
                    log(line)
            records = output_records(staging)
            if fingerprint(game) != (key, inputs):
                raise ValueError('Game or preparation code changed during boot preparation; launch again')
            (staging / 'manifest.json').write_text(
                json.dumps(dict(inputs=inputs, outputs=records), sort_keys=True) + '\n', encoding='utf-8')
            if linked(target):
                raise ValueError('Boot cache entry must not be a symlink')
            if target.exists():
                # An invalid entry is never served; no active reader exists under the session lock.
                if target.is_dir():
                    shutil.rmtree(target)
                else:
                    target.unlink()
            staging.rename(target)
            log('Boot cache: ready: ' + str(target))
        finally:
            if staging.exists():
                try:
                    shutil.rmtree(staging)
                except OSError as error:
                    log('Boot cache: staging cleanup deferred (' + str(error) + ')')
        try:
            prune(region, target)
        except OSError as error:
            log('Boot cache: old cache cleanup deferred (' + str(error) + ')')
        return target


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    try:
        # run.sh captures only the chosen directory; diagnostics remain visible on stderr.
        with redirect_stdout(sys.stderr):
            target = ensure(a.game, a.out)
        print(target)
    except (OSError, ValueError, RuntimeError, StopIteration, KeyError, IndexError, prepare.GameCheckError) as error:
        print('Boot preparation failed: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
