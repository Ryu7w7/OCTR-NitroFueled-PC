# SPDX-License-Identifier: GPL-2.0-or-later
"""run.sh's mod packages: the steps run.py does in its own process on Windows (mod_packages.py),
one command each, as run.sh runs separate programs.

  prepare   mod_packages.prepare: the session of the enabled packages (or nothing). Writes the
            changed environment to --env-file (NUL-separated KEY=VALUE, "-KEY" to unset), with
            BB_MOD_SESSION / BB_MOD_STATE naming the session for `finish`.
  overlays  the packages' archive entries over the built-in archive patches (BB_ARCHIVE_PATCHES):
            prints the new list.
  preflight the executable patches checked for overlaps, and the widescreen layout the packages
            need (as run.py's apply_layout).
  finish    the session's result; after a normal exit its saves go back to the mod profile.
"""
import argparse
import os
import sys
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent


def prepare(a):
    import mod_packages
    env = dict(os.environ)
    before = dict(env)
    log = lambda message: print(message, file=sys.stderr, flush=True)  # noqa: E731
    try:
        game, config = mod_packages.prepare(PORT, Path(a.data), env, Path(a.game), Path(a.config), log=log)
    except (ValueError, RuntimeError, OSError) as error:
        print('Mod launch blocked: ' + str(error) + '\nChoose compatible settings or disable the indicated mod in Mods.',
              file=sys.stderr)
        return 3
    if mod_packages.ACTIVE:
        session, state = mod_packages.ACTIVE
        env['BB_MOD_SESSION'], env['BB_MOD_STATE'] = str(session), str(state)
    lines = [f'{k}={v}' for k, v in env.items() if before.get(k) != v]
    lines += [f'-{k}' for k in before if k not in env]
    lines.append(f'@game={game}')
    Path(a.env_file).write_text(''.join(line + '\0' for line in lines), encoding='utf-8')
    return 0


def overlays(a):
    import archive_patch
    from mod_overlays import for_launch
    archives = os.environ.get('BB_ARCHIVE_PATCHES', '')
    builtins = {archive_patch.read(p)[1]: Path(p) for p in archives.split('|') if p}
    composed = for_launch(Path(a.game), Path(a.out) / 'mod-overlays', os.environ['BB_MOD_ARCHIVE_PLAN'], builtins)
    print('|'.join(str(p) for p in composed.values()))
    return 0


def preflight(a):
    import mod_packages
    import widescreen
    if widescreen.selection(widescreen.read_ini(a.config)) and not a.layout_patches:
        print('Mod launch blocked: the selected mods cannot use this widescreen layout; see the Widescreen message above.',
              file=sys.stderr)
        return 3
    try:
        mod_packages.preflight(Path(os.environ['BB_PATCHES_DIR']).parent, a.settings, Path(os.environ['BB_PATCHES_CONFIG']),
                               [a.layout_patches] if a.layout_patches else [], a.title_id, a.app_version, game=a.game)
    except (ValueError, OSError) as error:
        print('Mod launch blocked: ' + str(error), file=sys.stderr)
        return 3
    return 0


def finish(a):
    import mod_packages
    mod_packages.ACTIVE = (Path(a.session), Path(a.state))
    mod_packages.finish(a.code)
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='step', required=True)
    s = sub.add_parser('prepare')
    s.add_argument('--data', required=True)
    s.add_argument('--game', required=True)
    s.add_argument('--config', required=True)
    s.add_argument('--env-file', required=True)
    s = sub.add_parser('overlays')
    s.add_argument('--game', required=True)
    s.add_argument('--out', required=True)
    s = sub.add_parser('preflight')
    s.add_argument('--game', required=True)
    s.add_argument('--config', required=True, help="the session's bbport.ini")
    s.add_argument('--settings', required=True, help='the settings patches.py reads (the layout\'s, if any)')
    s.add_argument('--layout-patches', default='')
    s.add_argument('--title-id', default='CUSA13795')
    s.add_argument('--app-version', default='01.21')
    s = sub.add_parser('finish')
    s.add_argument('--session', required=True)
    s.add_argument('--state', required=True)
    s.add_argument('--code', type=int, required=True)
    a = p.parse_args()
    return {'prepare': prepare, 'overlays': overlays, 'preflight': preflight, 'finish': finish}[a.step](a)


if __name__ == '__main__':
    sys.exit(main())
