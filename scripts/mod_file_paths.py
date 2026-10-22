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

def resolve(path):
    path=Path(path)
    game,files=records()
    if game is None:return path
    try:relative=path.resolve().relative_to(game).as_posix().casefold()
    except ValueError:return path
    return files.get(relative,path)

def archives(directory):
    directory=Path(directory)
    game,files=records()
    result={p.name.casefold():resolve(p) for p in directory.glob('*.pak')}
    if game is not None and directory.resolve()==game/'archives':
        for relative,source in files.items():
            if relative.startswith('archives/') and relative.count('/')==1 and relative.endswith('.pak'):
                result[relative.split('/')[1]]=source
    return [result[key] for key in sorted(result)]
