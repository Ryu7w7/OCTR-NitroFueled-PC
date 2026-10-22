"""Session lock and save commits confined to the separate mod profile. GPL-2.0-or-later."""
from pathlib import Path
from contextlib import contextmanager
import os,uuid
from mod_components import copy_tree,sha

@contextmanager
def launch_lock(state):
    path=Path(state)/'extensions/launch.lock';path.parent.mkdir(parents=True,exist_ok=True)
    with path.open('a+b') as stream:
        stream.seek(0);stream.write(b'0');stream.flush();stream.seek(0)
        try:
            if os.name=='nt':
                import msvcrt
                msvcrt.locking(stream.fileno(),msvcrt.LK_NBLCK,1)
            else:
                import fcntl
                fcntl.flock(stream,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except OSError as error:raise ValueError('A mod session is already running') from error
        try:yield
        finally:
            if os.name=='nt':msvcrt.locking(stream.fileno(),msvcrt.LK_UNLCK,1)
            else:fcntl.flock(stream,fcntl.LOCK_UN)

def sync_saves(source,target,backups):
    source=Path(source);target=Path(target);backups=Path(backups)
    if not source.is_dir():return
    target.parent.mkdir(parents=True,exist_ok=True)
    stage=target.parent/('save-staging-'+uuid.uuid4().hex);stage.mkdir()
    copy_tree(source,stage)
    for p in source.rglob('*'):
        if p.is_file() and sha(p)!=sha(stage/p.relative_to(source)):raise ValueError('Save verification failed')
    backups.mkdir(parents=True,exist_ok=True);backup=backups/('prior-'+uuid.uuid4().hex)
    had=target.exists()
    if had:target.rename(backup)
    try:stage.rename(target)
    except BaseException:
        if had:backup.rename(target)
        raise
