#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Merges a dumped update into the base game without copying it: the game folder SacredFire runs.

    python tools/merge_update.py <base game folder> <update folder> <merged folder>

The merged folder holds the base game's files with the update's files over them (eboot.bin and
sce_sys included), as hard links to the originals: no extra disk space, and the two dumps stay as
they are. Hard links need the three folders on one drive; across drives the files are copied.
The game never writes to its own folder (/app0 is read-only in the runtime), so the originals are
not changed through the links.

Dumps extracted with PS4 PKG Tool keep the files in Image0/ and the sce_sys files in Sc0/; such a
folder is read in that layout and merged into the usual one (Image0/* at the root, Sc0/* in
sce_sys/), as the launcher does.
"""
import argparse
import os
import shutil
import sys
from pathlib import Path


def link_or_copy(source, target, counts):
    if target.exists():
        target.unlink()
    try:
        os.link(source, target)
        counts['linked'] += 1
    except OSError:
        shutil.copy2(source, target)
        counts['copied'] += 1


def layout(folder):
    """The (source, destination prefix) trees of a dump, or None when it has no game. A folder
    fixed by hand (eboot.bin copied next to Image0) counts as flat only with sce_module in it."""
    folder = Path(folder)
    image = folder / 'Image0'
    flat = (folder / 'eboot.bin').is_file() and (folder / 'sce_sys/param.sfo').is_file()
    if flat and (not (image / 'eboot.bin').is_file() or (folder / 'sce_module').is_dir()):
        return [(folder, Path())]
    if not (image / 'eboot.bin').is_file():
        return None
    for sys_dir in (image / 'sce_sys', folder / 'Sc0', folder / 'sce_sys'):
        if (sys_dir / 'param.sfo').is_file():
            return [(image, Path())] + ([] if sys_dir == image / 'sce_sys' else [(sys_dir, Path('sce_sys'))])
    return None


def add_tree(root, out, counts, replaced=None, prefix=Path()):
    for directory, _subdirs, files in os.walk(root):
        relative = prefix / Path(directory).relative_to(root)
        (out / relative).mkdir(parents=True, exist_ok=True)
        for name in files:
            target = out / relative / name
            if replaced is not None and target.exists():
                replaced.append(str(relative / name))
            link_or_copy(Path(directory) / name, target, counts)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('base', type=Path)
    p.add_argument('update', type=Path)
    p.add_argument('out', type=Path)
    a = p.parse_args()
    trees = {}
    for folder in (a.base, a.update):
        trees[folder] = layout(folder)
        if trees[folder] is None:
            sys.exit(f'No eboot.bin and sce_sys/param.sfo (nor Image0 and Sc0) in {folder}')
    if a.out.resolve() in (a.base.resolve(), a.update.resolve()):
        sys.exit('The merged folder must be a new folder')
    a.out.mkdir(parents=True, exist_ok=True)
    counts = {'linked': 0, 'copied': 0}
    replaced = []
    for root, prefix in trees[a.base]:
        add_tree(root, a.out, counts, prefix=prefix)
    for root, prefix in trees[a.update]:
        add_tree(root, a.out, counts, replaced, prefix)
    print(f'Merged {a.update} over {a.base} into {a.out}: {counts["linked"]} files linked, '
          f'{counts["copied"]} copied, {len(replaced)} replaced by the update')


if __name__ == '__main__':
    main()
