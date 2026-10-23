"""Automatic, private original-shader setup for declarative shader packages.
Author: tcosi. SPDX-License-Identifier: GPL-2.0-or-later.
"""
from pathlib import Path
import os,shutil,subprocess,time,uuid
from mod_components import copy_tree,under,sha,save_json

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
    if os.name!='nt':return close_owned_game_posix(parent_pid,executable)
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

def close_owned_game_posix(pid,executable):
    """Linux: the setup's run.sh becomes the engine (exec, same PID). Once /proc names our engine,
    SIGTERM, which SDL turns into a quit request: the game closes as from its window's close
    button and ends with code 0 (gpu/shim/window.cpp). False while run.sh is still preparing."""
    import signal
    try:
        if Path(f'/proc/{pid}/exe').resolve()!=Path(executable).resolve():return False
        os.kill(pid,signal.SIGTERM)
    except OSError:return False
    return True

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

def ensure_shaders(root,state,game,user,plan,environment,status,prepare_only=False):
    missing,wrong=requirements(user,plan['operations'])
    if wrong:raise ValueError('Unsupported original shader cache: '+', '.join(wrong))
    if not missing:return False
    if prepare_only:raise ValueError('Shader setup required: '+', '.join(missing)+'. Press Play to generate the original cache automatically.')
    import mod_content as nx
    setup=state/'extensions/shader-setup'/uuid.uuid4().hex;setup.mkdir(parents=True)
    empty=setup/'mods';empty.mkdir();save_json(setup/'selection.json',{'disabled':[]})
    # No package settings, XML, assets or shaders participate in the setup session.
    from mod_components import read_ini
    settings=read_ini(state/'bbport.ini')
    settings.update(output_res='1920x1080',render_res='1080',patch_pro_1080p='0',temporal='off',upscaler='off',online='0')
    ini=setup/'bbport.ini';ini.write_text(''.join(k+'='+v+'\n' for k,v in settings.items()),encoding='utf-8')
    baseline=nx.prepare(root,state,game,empty,setup/'selection.json',ini,environment)
    setup_user=setup/'user';setup_user.mkdir()
    # Copy only this independent launcher's profile, never another installation.
    if (state/'user').exists():copy_tree(state/'user',setup_user)
    data=baseline['profile']/'runtime';(data/'out').mkdir(parents=True,exist_ok=True)
    for name in ('aerolib.inl','import_names.txt'):
        # run.sh keeps them in the data folder's out/, next to mod-state/.
        source=next((d/name for d in (state/'out',state.parent/'out') if (d/name).exists()),None)
        if source and not (data/'out'/name).exists():shutil.copy2(source,data/'out'/name)
    env={k:v for k,v in environment.items() if not k.startswith('BB_')}
    # Linux: BB_PREBUILT keeps run.sh on the package's engine (else it would build one); the
    # screen and controller choices let it be played on a Steam Deck in game mode.
    kept=('BB_LANGUAGE','BB_ONLINE_NAME','BB_USER_NAME')+(() if os.name=='nt' else ('BB_PREBUILT','BB_FULLSCREEN','BB_DISPLAY','BB_GAMEPAD'))
    for key in kept:
        if key in environment:env[key]=environment[key]
    env.update(BB_MOD_PACKAGES_SKIP='1',BB_GAME_DIR=str(baseline['game']),BB_USER_DIR=str(setup_user),BB_DATA_DIR=str(data),
        BB_CONFIG=str(ini),BB_MODS_ENABLED='0',BB_PATCHES_DIR=str(baseline['profile']/'patches'),
        BB_PATCHES_CONFIG=str(setup/'patches.json'),BB_ONLINE='0',BB_NEO='0')
    save_json(setup/'patches.json',{'enabled':[],'disabled':[]})
    save_json(setup/'setup.json',{'requested_packages':plan['packages'],'game':str(baseline['game']),'mods':[]})
    status('shader_setup','First-time shader setup: reach the main menu and accept the game agreement if asked. The game will restart with your mods when the required shaders are ready. Some mods may need their game content visited first.')
    python=str(root/'python/python.exe') if (root/'python/python.exe').exists() else os.sys.executable
    command=[python,'-X','utf8',str(root/'run.py')] if os.name=='nt' else ['bash',str(root/'run.sh')]
    try:
        with (setup/'console.log').open('w',encoding='utf-8') as log:
            process=subprocess.Popen(command,cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            wait_for_setup(process,setup_user,plan['operations'],nx.engine(root),status)
        save_json(setup/'result.json',{'exit_code':0,'required_shaders_verified':True})
    except Exception as e:
        save_json(setup/'result.json',{'error':str(e)});raise
    # Promote only verified, unmodified shader cache; no modded cache is ever reused.
    cache=state/'extensions/vanilla-cache'/plan['engine']
    copy_tree(setup_user/'cache',cache);copy_tree(setup_user/'cache',user/'cache')
    from mod_session import sync_saves
    sync_saves(setup_user/'savedata',state/'user/savedata',state/'extensions/save-backups')
    if (setup_user/'savedata').exists():copy_tree(setup_user/'savedata',user/'savedata')
    status('preparing','Shader setup complete. Starting your selected mods…')
    return True
