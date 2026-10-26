"""Native launcher adapter for declarative Mod packages. Author: tcosi. GPL-2.0-or-later."""
from pathlib import Path
import atexit,json,os,shutil,sys,uuid,subprocess
from functools import lru_cache
sys.path.insert(0,str(Path(__file__).resolve().parent))
from mod_components import copy_tree,save_json,apply_shader,under
from mod_library import scan,PREFIX
from mod_session import launch_lock,sync_saves
ACTIVE=None

@lru_cache(maxsize=8)
def engine_features(executable,digest):
    """Negotiate features once per engine, without any per-mod build allowlist."""
    try:
        result=subprocess.run([str(executable),'--mod-capabilities'],capture_output=True,text=True,
            timeout=15,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        value=json.loads(result.stdout) if result.returncode==0 else {}
        return value if isinstance(value,dict) else {}
    except (OSError,ValueError,subprocess.TimeoutExpired):return {}

def preflight(profile,config,selection,extra_dirs=(),title_id='CUSA13795',app_version='01.21',game=None):
    from prepare import parse_self
    from patches import all_external_patches,external_selection,read_settings,render_patches,compile_external,eboot_segments
    elf,*_=parse_self(((Path(game) if game is not None else profile/'game')/'eboot.bin').read_bytes())
    settings=read_settings(Path(config))  # run.py passes bbport.ini's path as text without a widescreen layout
    selected=external_selection(all_external_patches([profile/'patches',*extra_dirs],app_version,title_id),selection,settings)+render_patches(settings)
    claims=[]
    for key,path,meta in selected:
        writes=compile_external([(key,path,meta)],eboot_segments(elf),elf,
            strict=path.name=='OCTR-Mod-Content.xml',require_verification=path.name=='OCTR-Mod-Content.xml')
        if len(writes)!=len(list(meta.iter('Line'))):raise ValueError('Incomplete executable patch: '+key)
        for offset,data in writes:
            for old,raw,owner in claims:
                if offset<old+len(raw) and old<offset+len(data):raise ValueError('Overlapping executable patches: '+owner+' / '+key)
            claims.append((offset,data,key))
    print('OCTR mods:',len(claims),'verified non-overlapping executable writes',flush=True)
    return len(claims)

KEEP_SESSIONS = 3


def prune_sessions(sessions, keep=KEEP_SESSIONS):
    """Deletes old mod sessions (each one a copy of user/: saves and shader cache), keeping the
    `keep` latest. Only sessions that ended normally go: their saves were synced to the mod
    profile (finish). One that crashed or is unfinished stays, as it may hold the only copy of
    its saves. Returns the deleted folders."""
    sessions = Path(sessions)
    if not sessions.is_dir():
        return []
    found = sorted((p for p in sessions.iterdir() if p.is_dir()), key=lambda p: p.stat().st_mtime, reverse=True)
    deleted = []
    for session in found[keep:]:
        try:
            ended = json.loads((session / 'result.json').read_text()).get('exit_code') == 0
        except (OSError, ValueError, AttributeError):
            ended = False
        if ended:
            shutil.rmtree(session, ignore_errors=True)
            deleted.append(session)
    return deleted


def finish(code):
    if ACTIVE is None:return
    session,state=ACTIVE
    save_json(session/'result.json',{'exit_code':code})
    if code==0:sync_saves(session/'user/savedata',state/'user/savedata',state/'save-backups')

def prepare(port,data,environment,game,config,prepare_only=False,log=print):
    global ACTIVE
    from patches import external_patches,external_selection,read_settings
    import mods
    import mod_content as nx
    for key in ('BB_MOD_FILE_MAP','BB_MOD_FILE_DIRS','BB_MOD_SOURCE_GAME','BB_MOD_ARCHIVE_PLAN'):
        environment.pop(key,None)
    import mod_file_paths
    mod_file_paths.configure({})
    root=Path(environment.get('BB_MODS_DIR',Path(data)/'mods'))
    # Preserve the zero-package path, including custom patches/settings/data locations.
    if environment.get('BB_MODS_ENABLED','1')=='0' or not root.is_dir():return Path(game),str(config)
    from game_check import sfo_info
    game_info=sfo_info(game)
    title_id,app_version=game_info.get('TITLE_ID'),game_info.get('APP_VER')
    supported=title_id in ('CUSA13795','CUSA14876') and app_version=='01.21'
    state=root.parent/'mod-state'
    lock=launch_lock(state);lock.__enter__()
    try:
        # BB_MOD_LIBRARY_DIR: where the library's patch file goes (run.sh: the data folder's
        # patches/, as the Linux package's own folder is read-only); the bundled patches/ otherwise.
        library=Path(environment.get('BB_MOD_LIBRARY_DIR') or Path(port)/'patches')
        catalog=scan(root,library)
        for message in catalog['messages']:log('OCTR mods: '+message)
        selection=Path(environment.get('BB_PATCHES_CONFIG',Path(data)/'patches.json'))
        if not supported:
            # Every package ships a mods folder: a game the packages cannot target (EU 1.00, no
            # readable param.sfo) starts as before, and is refused only with a package enabled.
            enabled=json.loads(selection.read_text()).get('enabled',[]) if selection.exists() else []
            if any(key.startswith(PREFIX) and key.removeprefix(PREFIX) in catalog['entries'] for key in enabled):
                raise ValueError('Mod packages require a supported CTR edition at 01.21 (this game: '+
                    (title_id or 'unknown')+' '+(app_version or 'unknown version')+')')
            if catalog['entries'] or catalog['messages']:environment['BB_MODS_ENABLED']='0'  # not loose mods
            lock.__exit__(None,None,None);return Path(game),str(config)
        patchdir=Path(environment.get('BB_PATCHES_DIR',Path(port)/'patches'))
        found=external_patches(patchdir,app_version,title_id)
        if library.resolve()!=patchdir.resolve():
            found+=[p for p in external_patches(library,app_version,title_id) if p[0].startswith(PREFIX)]
        selected=external_selection(found,selection,read_settings(Path(config)))
        names=[key.removeprefix(PREFIX) for key,_,_ in selected if key.startswith(PREFIX)]
        original=json.loads(selection.read_text()) if selection.exists() else {'enabled':[],'disabled':[]}
        if catalog['messages'] and any(key.startswith(PREFIX) and key.removeprefix(PREFIX) not in catalog['entries'] for key in original.get('enabled',[])):
            raise ValueError('An enabled mod could not be validated; check the OCTR mod messages')
        if catalog['entries'] or catalog['messages']:environment['BB_MODS_ENABLED']='0'  # own validated selection
        if not names:lock.__exit__(None,None,None);return Path(game),str(config)
        shutil.copy2(config,state/'bbport.ini')
        prefs=state/'selection.json';save_json(prefs,{'disabled':[n for n in catalog['entries'] if n not in names]})
        plan=nx.prepare(port,state,game,root,prefs,state/'bbport.ini',environment,log=log)
        import struct
        file_count,=struct.unpack_from('<I',plan['file_map'].read_bytes(),8)
        if file_count and engine_features(nx.engine(port),plan['engine']).get('file_overlays')!=1:
            raise ValueError('This engine lacks native mod file redirection. Update the engine and launcher together; no game-copy fallback is permitted.')
        prune_sessions(state/'sessions')
        session=state/'sessions'/uuid.uuid4().hex;session.mkdir(parents=True)
        user=session/'user';user.mkdir()
        canonical=state/'user';original_user=Path(environment.get('BB_USER_DIR',Path(data)/'user'))
        if not canonical.exists() and original_user.exists():copy_tree(original_user,canonical)
        if canonical.exists():copy_tree(canonical,user)
        cache=state/'extensions/vanilla-cache'/plan['engine']
        if cache.exists():copy_tree(cache,user/'cache')
        if environment.get('BB_CLEAR_SHADER_CACHE')=='1' and (user/'cache').exists():
            # Preserve a requested reset for inspection; never clear another profile.
            (user/'cache').rename(session/'cache-before-reset')
        ini=session/'bbport.ini';ini.write_text(''.join(k+'='+v+'\n' for k,v in plan['settings'].items()),encoding='utf-8')
        snapshot=session/'patches.json';save_json(snapshot,original)
        writes=preflight(plan['profile'],ini,snapshot,title_id=title_id,app_version=app_version,game=plan['game'])
        if plan['has_shaders'] and not prepare_only:
            from mod_shaders import ensure_shaders
            ensure_shaders(port,state,game,user,plan,environment,lambda stage,message:print(message,flush=True))
            for op,folder in plan['operations']:
                if op['kind'].startswith('shader_'):apply_shader(under(user,op['target']),op,folder)
        save_json(session/'launch.json',{'packages':names,'variants':plan['variants'],'writes':writes,
            'resolution':plan['selection_settings'].get('output_res'),'online':plan['settings'].get('online','0')})
        environment.update(plan['environment'])
        environment.update(BB_CONFIG=str(ini),BB_USER_DIR=str(user),BB_PATCHES_DIR=str(plan['profile']/'patches'),BB_PATCHES_CONFIG=str(snapshot),BB_MODS_ENABLED='0',BB_CLEAR_SHADER_CACHE='0',BB_MOD_ARCHIVE_PLAN=str(plan['archive_plan']),BB_MOD_FILE_MAP=str(plan['file_map']),BB_MOD_FILE_DIRS=str(plan['file_dirs']),BB_MOD_SOURCE_GAME=str(game))
        mod_file_paths.configure(environment)
        log('OCTR mod session: '+', '.join(names)+' at '+str(plan['selection_settings'].get('output_res')))
        log('Separate mod saves and logs: '+str(session))
        ACTIVE=(session,state);atexit.register(lock.__exit__,None,None,None)
        return plan['game'],str(ini)
    except BaseException:
        lock.__exit__(*sys.exc_info());raise

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scan',type=Path,required=True,metavar='PORTABLE_ROOT')
    parser.add_argument('--remove',metavar='MOD_ID')
    parser.add_argument('--expected-sha',metavar='ZIP_SHA256')
    args=parser.parse_args()
    if bool(args.remove)!=bool(args.expected_sha):parser.error('--remove requires --expected-sha')
    if args.remove:
        from mod_library import remove
        print('Mod moved to:',remove(args.scan/'mods',args.scan/'patches',args.remove,args.expected_sha))
    else:
        with launch_lock(args.scan/'mod-state'):
            result=scan(args.scan/'mods',args.scan/'patches')
        for message in result['messages']:print(message)
        print('Mod packages:',len(result['entries']))
