# SPDX-License-Identifier: GPL-2.0-or-later
"""What the launcher keeps in mod-state and content-state that nothing uses any more, removed.

  - content-state/packages/<sha256>: a package ZIP unpacked once; a new version of the ZIP is
    unpacked again beside the old one (scripts/content_package.py).
  - mod-state/packages/<sha256>: the same for the mod library (scripts/mod_library.py).
  - mod-state/extensions/overlays/<key>: a prepared session, one per combination of mods,
    options and game files (scripts/mod_content.py), and building-* left by a stopped one.

Only while no game session holds mod-state's launch lock: a running game reads these folders.
"""
import re
import shutil
from pathlib import Path

HASH = re.compile(r'[0-9a-f]{64}\Z')
KEEP_SESSIONS = 3  # prepared sessions kept: the one in use and the two before it


def size(folder):
    return sum(p.stat().st_size for p in Path(folder).rglob('*') if p.is_file())


def remove(folders, what, log=print):
    """Removes folders; logs how much was freed. Returns the bytes freed."""
    freed = 0
    for folder in folders:
        try:
            n = size(folder)
            shutil.rmtree(folder)
            freed += n
        except OSError as error:
            log(f'{what}: could not remove {Path(folder).name}: {error}')
    if freed:
        log(f'{what}: removed {len(folders)} unused copies, {freed / 1024 ** 3:.1f} GB freed')
    return freed


def unpacked(folder, keep, what, log=print):
    """The <sha256> folders of `folder` not in `keep` (names), removed. The caller holds the
    launch lock."""
    folder = Path(folder)
    if not folder.is_dir():
        return 0
    stale = [p for p in folder.iterdir() if p.is_dir() and HASH.match(p.name) and p.name not in keep]
    return remove(stale, what, log) if stale else 0


def unpacked_unless_running(folder, keep, mod_state, what, log=print):
    """unpacked(), taking the launch lock first; nothing is removed while a session holds it."""
    folder = Path(folder)
    if not folder.is_dir() or not any(p.is_dir() and HASH.match(p.name) and p.name not in keep
                                      for p in folder.iterdir()):
        return 0
    from mod_session import launch_lock
    try:
        with launch_lock(mod_state):
            return unpacked(folder, keep, what, log)
    except ValueError:  # a game session is running: next time
        return 0


def sessions(overlays, current, log=print):
    """Prepared sessions (overlays/<key>) but `current` and the newest others, up to
    KEEP_SESSIONS in all, and unfinished building-* folders, removed. The caller holds the
    launch lock."""
    overlays = Path(overlays)
    if not overlays.is_dir():
        return 0
    from cache_cleanup import linked  # never into a link or junction
    if linked(overlays):
        return 0
    ready = [p for p in overlays.iterdir() if p.is_dir() and not linked(p) and (p / 'ready.json').is_file()
             and p.name != Path(current).name]
    ready.sort(key=lambda p: (p / 'ready.json').stat().st_mtime, reverse=True)
    stale = ready[KEEP_SESSIONS - 1:]
    stale += [p for p in overlays.iterdir() if p.is_dir() and not linked(p) and p.name.startswith('building-')]
    return remove(stale, 'Prepared sessions', log) if stale else 0
