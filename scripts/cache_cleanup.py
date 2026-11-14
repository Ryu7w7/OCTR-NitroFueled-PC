"""Remove only obsolete generated cache files; never traverse links or junctions."""
from pathlib import Path


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
