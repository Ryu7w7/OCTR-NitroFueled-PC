# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's "External content" section (launcher/windows/launcher.cpp: ContentSection).

    python scripts/content_cli.py PORTABLE_ROOT list
        The packages of PORTABLE_ROOT/custom_content (scripts/content_package.py), one per line,
        fields separated by tabs (tabs and line breaks in texts become spaces):
          selection  <1|0 all external content on>  <page: own|bonus>
          package    <id> <1|0 on> <type> <name> <version> <author> <online> <source> <map> <credits>
          message    <text>                         a package that could not be read
    python scripts/content_cli.py PORTABLE_ROOT remove ID
        Moves the package's ZIP or folder to content-state/removed (nothing is deleted).
    python scripts/content_cli.py PORTABLE_ROOT convert GAME_FOLDER MOD
        Converts a classic track mod (its archives and a whole update3.pak) into a package,
        custom_content/<track id>.zip (scripts/content_convert.py).

The launcher writes content-state/selection.json itself (its switches).
"""
import shutil
import sys
import tempfile
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import content_package as cp  # noqa: E402


def clean(value):
    return ' '.join(str(value or '').split())


def name_of(m):
    names = m.get('name') or {}
    return names.get('es') or names.get('en') or next(iter(names.values()), m['id'])


def listing(root):
    folder = Path(root) / cp.FOLDER
    choice = cp.selection(folder)
    lines = ['selection\t%d\t%s' % (choice['enabled'], choice['page'])]
    result = cp.scan(folder, log=lambda message: None)
    for ident, record in sorted(result['packages'].items()):
        m = record['manifest']
        fields = [ident, '0' if ident in choice['disabled'] else '1', m['type'], name_of(m), m['version'],
                  m['author'], m.get('online', 'offline-only'), record['source'], m.get('map', ''),
                  m.get('credits', '')]
        lines.append('package\t' + '\t'.join(clean(f) for f in fields))
    lines += ['message\t' + clean(message) for message in result['messages']]
    return lines


def remove(root, ident):
    folder = Path(root) / cp.FOLDER
    record = cp.scan(folder, log=lambda message: None)['packages'].get(ident)
    if record is None:
        raise ValueError('no package ' + ident)
    source = folder / record['source']
    dest = cp.state(folder) / 'removed' / (time.strftime('%Y%m%d-%H%M%S-') + source.name)
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(source), str(dest))
    return dest


def convert(root, game, mod):
    import content_convert
    folder = Path(root) / cp.FOLDER
    folder.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='convert-', dir=cp.state(folder).parent) as work:
        made, dropped = content_convert.convert(game, mod, work)
        target = folder / (made.name + '.zip')
        if target.exists():
            raise ValueError(target.name + ' is already in custom_content')
        content_convert.pack(made, Path(work) / target.name)
        shutil.move(str(Path(work) / target.name), str(target))
    return target, dropped


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) < 2:
        print(__doc__)
        return 2
    root, command = Path(argv[0]), argv[1]
    try:
        if command == 'list' and len(argv) == 2:
            sys.stdout.reconfigure(encoding='utf-8')
            print('\n'.join(listing(root)))
        elif command == 'remove' and len(argv) == 3:
            print('Moved to:', remove(root, argv[2]))
        elif command == 'convert' and len(argv) == 4:
            target, dropped = convert(root, Path(argv[2]), Path(argv[3]))
            print('Converted:', target.name)
            for path in dropped:
                print('  left out (changes a game file every track uses):', path)
        else:
            print(__doc__)
            return 2
    except (ValueError, OSError, zipfile.BadZipFile) as error:
        print('External content:', error)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
