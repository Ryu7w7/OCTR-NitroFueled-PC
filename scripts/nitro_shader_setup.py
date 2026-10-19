"""Original-shader validation and normal closure for owned setup children.
Author: tcosi. SPDX-License-Identifier: GPL-2.0-or-later.
"""
from pathlib import Path
import os,shutil,subprocess,time,uuid
from nitro_components import copy_tree,under,sha,save_json

def requirements(user,operations):
    """Missing inputs may be generated; incompatible existing inputs must not be used."""
    cache=Path(user)/'cache/CUSA13795';missing=[];wrong=[]
    if not (cache/'profile.bin').is_file():missing.append('shader profile')
    if not any(cache.glob('*.meta')):missing.append('shader metadata')
    if not any(cache.glob('*.key')):missing.append('pipeline keys')
    for op,folder in operations:
        if not op['kind'].startswith('shader_'):continue
        p=under(user,op['target']);expected=op['original_sha256']
        if expected is None:
            if p.exists():wrong.append(p.name+' must be a new file')
        elif not p.is_file():missing.append(p.name)
        elif sha(p)!=expected:wrong.append(p.name+' has an unsupported checksum')
    return missing,wrong

def close_owned_game(parent_pid,executable):
    """Request normal window closure only for this wrapper's verified child engine."""
    import ctypes
    from ctypes import wintypes as w
    kernel=ctypes.WinDLL('kernel32',use_last_error=True);user=ctypes.WinDLL('user32',use_last_error=True)
    class Entry(ctypes.Structure):
        _fields_=[('size',w.DWORD),('usage',w.DWORD),('pid',w.DWORD),('heap',ctypes.c_size_t),('module',w.DWORD),('threads',w.DWORD),('parent',w.DWORD),('priority',w.LONG),('flags',w.DWORD),('exe',w.WCHAR*260)]
    kernel.CreateToolhelp32Snapshot.restype=w.HANDLE
    kernel.Process32FirstW.argtypes=[w.HANDLE,ctypes.POINTER(Entry)]
    kernel.Process32NextW.argtypes=[w.HANDLE,ctypes.POINTER(Entry)]
    kernel.CloseHandle.argtypes=[w.HANDLE]
    kernel.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];kernel.OpenProcess.restype=w.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes=[w.HANDLE,w.DWORD,w.LPWSTR,ctypes.POINTER(w.DWORD)]
    handle=kernel.CreateToolhelp32Snapshot(2,0);parents={}
    if handle==w.HANDLE(-1).value:return False
    try:
        entry=Entry();entry.size=ctypes.sizeof(entry);valid=kernel.Process32FirstW(handle,ctypes.byref(entry))
        while valid:
            parents[entry.pid]=(entry.parent,entry.exe)
            valid=kernel.Process32NextW(handle,ctypes.byref(entry))
    finally:kernel.CloseHandle(handle)
    owned=set()
    for pid,(ppid,name) in parents.items():
        if name.casefold()!=Path(executable).name.casefold():continue
        seen=set();ancestor=ppid
        while ancestor and ancestor not in seen and ancestor!=parent_pid:
            seen.add(ancestor);ancestor=parents.get(ancestor,(0,''))[0]
        if ancestor!=parent_pid:continue
        process=kernel.OpenProcess(0x1000,False,pid)
        if not process:continue
        try:
            buf=ctypes.create_unicode_buffer(32768);length=w.DWORD(len(buf))
            if kernel.QueryFullProcessImageNameW(process,0,buf,ctypes.byref(length)) and Path(buf.value).resolve()==Path(executable).resolve():owned.add(pid)
        finally:kernel.CloseHandle(process)
    callback=ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
    user.GetWindowThreadProcessId.argtypes=[w.HWND,ctypes.POINTER(w.DWORD)]
    user.IsWindowVisible.argtypes=[w.HWND];user.PostMessageW.argtypes=[w.HWND,w.UINT,w.WPARAM,w.LPARAM]
    requested=[]
    @callback
    def visit(hwnd,param):
        pid=w.DWORD();user.GetWindowThreadProcessId(hwnd,ctypes.byref(pid))
        if pid.value in owned and user.IsWindowVisible(hwnd):
            if user.PostMessageW(hwnd,0x0010,0,0):requested.append(pid.value)
        return True
    user.EnumWindows.argtypes=[callback,w.LPARAM];user.EnumWindows(visit,0)
    return bool(requested)

def wait_for_setup(process,user,operations,executable,status,clock=time.monotonic,sleep=time.sleep,closer=close_owned_game):
    """Close normally after inputs remain complete for three seconds; never force kill."""
    stable=None;close_sent=False
    while process.poll() is None:
        try:missing,wrong=requirements(user,operations)
        except OSError:missing,wrong=['shader cache is being written'],[]
        ready=not missing and not wrong
        if ready:
            if stable is None:stable=clock()
            if not close_sent and clock()-stable>=3:
                close_sent=closer(process.pid,executable)
                if close_sent:status('shader_setup','Shaders ready. Finishing setup; your mods will start next.')
        else:stable=None
        sleep(.25)
    code=process.wait()
    if code!=0:raise ValueError('Shader setup game exited unsuccessfully. Your mods were not started; see the setup log.')
    missing,wrong=requirements(user,operations)
    if wrong:raise ValueError('Shader setup produced incompatible inputs: '+', '.join(wrong))
    if missing:raise ValueError('Shader setup ended before the required shaders were generated: '+', '.join(missing)+'. Press Play to try again and reach the main menu or content that uses those shaders.')
