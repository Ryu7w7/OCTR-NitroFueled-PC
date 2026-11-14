"""Remove only obsolete generated cache files; never traverse links or junctions."""
from pathlib import Path
import re
import shutil


def linked(path):
    path = Path(path)
    try:
        return path.is_symlink() or bool(getattr(path.lstat(), 'st_file_attributes', 0) & 0x400)
    except OSError:
        return False


def discard_files(root, files, log=print):
    root = Path(root)
    if linked(root):
        return
    for value in files:
        path = Path(value)
        try:
            path.resolve().relative_to(root.resolve())
            if path == root or any(linked(parent) for parent in (path, *path.parents) if parent != root):
                continue
            path.unlink(missing_ok=True)
        except ValueError:
            continue
        except OSError as error:
            log(f'Cache cleanup deferred: {path.name}: {error}')


def discard_tree(folder, log=print):
    folder = Path(folder)
    if linked(folder):
        return
    try:
        shutil.rmtree(folder)
    except FileNotFoundError:
        pass
    except OSError as error:
        log(f'Cache cleanup deferred: {folder.name}: {error}')


def prune(current, marker, pattern, log=print):
    """After a successful build, remove older managed siblings, including cache hits."""
    current = Path(current)
    if linked(current) or linked(current.parent) or not (current / marker).is_file():
        return
    try:
        folders = list(current.parent.iterdir())
    except OSError as error:
        log(f'Cache cleanup deferred: {current.parent.name}: {error}')
        return
    for folder in folders:
        if folder == current or not re.fullmatch(pattern, folder.name) or linked(folder):
            continue
        manifest = folder / marker
        if not folder.is_dir() or linked(manifest):
            continue
        try:
            if not manifest.is_file():
                continue
            shutil.rmtree(folder)
        except (FileNotFoundError, ValueError):
            continue
        except OSError as error:
            log(f'Cache cleanup deferred: {folder.name}: {error}')
