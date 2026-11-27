"""Host-side reads of the same mod file map used by the guest runtime.
SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import os, struct
from functools import lru_cache

_settings=None

def configure(environment):
    global _settings
    _settings=environment

def records():
    env=_settings if _settings is not None else os.environ
    name=env.get('BB_MOD_FILE_MAP')
    game=env.get('BB_MOD_SOURCE_GAME')
    if not name or not game:return None,{}
    stamp=Path(name).stat()
    return read_map(name,game,stamp.st_mtime_ns,stamp.st_size)

@lru_cache(maxsize=8)
def read_map(name,game,mtime,size):
    data=Path(name).read_bytes()
    if data[:8]!=b'BBFILE01':raise ValueError('Invalid mod file map')
    count,=struct.unpack_from('<I',data,8);at=12;files={}
    if count>65536:raise ValueError('Mod file map exceeds engine limit')
    for _ in range(count):
        a,b=struct.unpack_from('<II',data,at);at+=8
        if not 0<a<1024 or not 0<b<4096:raise ValueError('Invalid mod file map path')
        relative=data[at:at+a].decode('utf-8');at+=a
        host=data[at:at+b].decode('utf-8');at+=b
        if '\0' in relative+host or relative.casefold() in files:raise ValueError('Invalid duplicate mod file map entry')
        from mod_components import safe_path
        safe_path(relative)
        files[relative.casefold()]=Path(host)
    if at!=len(data):raise ValueError('Truncated or trailing mod file map')
    return Path(game).resolve(),files

@lru_cache(maxsize=64)
def _view_root(directory,game):
    """The game view (the game folder, or mods.py's view of it: links to the game's files) that holds
    directory: the nearest folder whose eboot.bin is the game's."""
    eboot=Path(game)/'eboot.bin'
    for folder in (Path(directory),*Path(directory).parents):
        candidate=folder/'eboot.bin'
        try:
            if candidate.exists() and os.path.samefile(candidate,eboot):return folder
        except OSError:pass
    return None

def relative_in_view(path,game):
    """path's place in the game (archives/update3.pak...), also for a path in mods.py's view, whose
    entries are links (26/11: the new tracks' view hid a mod session's update3.pak, the port's archive
    took the same number and the game never saw it: no Neo Bowser City, no Graphics tab)."""
    path=Path(os.path.abspath(path))
    root=_view_root(str(path.parent),str(game))
    if root is not None:return path.relative_to(root).as_posix().casefold()
    try:return path.resolve().relative_to(game).as_posix().casefold()
    except ValueError:return None

def resolve(path):
    path=Path(path)
    game,files=records()
    if game is None:return path
    relative=relative_in_view(path,game)
    if relative is None:return path
    return files.get(relative,path)

def extra_updates(game):
    """update3.pak, update4.pak, ... in the game view (a mod's, loaded after update2.pak and in that
    order, until one is missing): their names, oldest first. The port's own update archive
    (scripts/graphics_tab.py) takes the first free number after them."""
    out = []
    for n in range(3, 20):
        name = f'update{n}.pak'
        if not resolve(Path(game) / 'archives' / name).is_file():
            break
        out.append(name)
    return out


def archives(directory):
    directory=Path(directory)
    game,files=records()
    result={p.name.casefold():resolve(p) for p in directory.glob('*.pak')}
    if game is not None and relative_in_view(directory/'x',game)=='archives/x':
        for relative,source in files.items():
            if relative.startswith('archives/') and relative.count('/')==1 and relative.endswith('.pak'):
                result[relative.split('/')[1]]=source
    return [result[key] for key in sorted(result)]
