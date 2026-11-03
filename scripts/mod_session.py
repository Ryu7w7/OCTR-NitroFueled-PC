"""Session lock and the one-time merge of the old separate mod save profile. GPL-2.0-or-later."""
from pathlib import Path
from contextlib import contextmanager
import os,time,uuid
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

def newest(folder):
    """The latest modification time of a file in folder (0 without any)."""
    times=[p.stat().st_mtime for p in Path(folder).rglob('*') if p.is_file()] if Path(folder).is_dir() else []
    return max(times,default=0)

def adopt_mod_profile(state,user):
    """Up to v0.7.5 a mod session played on its own copy of the saves (mod-state/user), so
    progress made with mods was gone with them off. Once: the newer of the two saves becomes the
    player's (user/savedata); the other is kept in mod-state/save-backups. Returns messages."""
    state=Path(state);user=Path(user);old=state/'user'
    if not old.exists():return []
    backups=state/'save-backups';backups.mkdir(parents=True,exist_ok=True)
    stamp=time.strftime('%Y%m%d-%H%M%S')
    messages=[]
    if newest(old/'savedata')>newest(user/'savedata'):
        previous=sync_saves(old/'savedata',user/'savedata',backups)
        messages.append('the save made with mods was newer: it is now your save'+
                        (' (the previous one is in '+str(previous)+')' if previous else ''))
    else:
        messages.append('your save is the newer one and is kept')
    old.rename(backups/('mod-profile-'+stamp))
    messages.append('mods now play on your own save; the old mod save is in '+str(backups/('mod-profile-'+stamp)))
    return messages

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
    return backup if had else None
