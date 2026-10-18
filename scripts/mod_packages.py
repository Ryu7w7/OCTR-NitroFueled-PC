"""Native launcher adapter for declarative ZIP packages. Author: tcosi. GPL-2.0-or-later."""
from pathlib import Path
import atexit,json,os,shutil,sys,uuid
sys.path.insert(0,str(Path(__file__).resolve().parent))
from nitro_components import copy_tree,save_json,apply_shader,under
from nitro_library import scan,PREFIX
from mod_session import launch_lock,sync_saves
ACTIVE=None

def preflight(profile,config,selection):
    from prepare import parse_self
    from patches import external_patches,external_selection,read_settings,render_patches,compile_external,eboot_segments
    elf,*_=parse_self((profile/'game/eboot.bin').read_bytes())
    settings=read_settings(config)
    selected=external_selection(external_patches(profile/'patches'),selection,settings)+render_patches(settings)
    claims=[]
    for key,path,meta in selected:
        writes=compile_external([(key,path,meta)],eboot_segments(elf),elf)
        if len(writes)!=len(list(meta.iter('Line'))):raise ValueError('Incomplete executable patch: '+key)
        for offset,data in writes:
            for old,raw,owner in claims:
                if offset<old+len(raw) and old<offset+len(data):raise ValueError('Overlapping executable patches: '+owner+' / '+key)
            claims.append((offset,data,key))
    print('ZIP mods:',len(claims),'verified non-overlapping executable writes',flush=True)
    return len(claims)

def finish(code):
    if ACTIVE is None:return
    session,state=ACTIVE
    save_json(session/'result.json',{'exit_code':code})
    if code==0:sync_saves(session/'user/savedata',state/'user/savedata',state/'save-backups')

def prepare(port,data,environment,game,config,prepare_only=False):
    global ACTIVE
    from patches import external_patches,external_selection,read_settings
    import mods
    import nitro_extensions as nx
    root=Path(environment.get('BB_MODS_DIR',Path(data)/'mods'))
    # Preserve the zero-package path, including custom patches/settings/data locations.
    if environment.get('BB_MODS_ENABLED','1')=='0' or not root.is_dir() or not any(root.glob('*.[zZ][iI][pP]')):return Path(game),str(config)
    state=root.parent/'mod-packages'
    lock=launch_lock(state);lock.__enter__()
    try:
        catalog=scan(root,Path(port)/'patches')
        for message in catalog['messages']:print('ZIP mods:',message,flush=True)
        selection=Path(environment.get('BB_PATCHES_CONFIG',Path(data)/'patches.json'))
        patchdir=Path(environment.get('BB_PATCHES_DIR',Path(port)/'patches'))
        selected=external_selection(external_patches(patchdir),selection,read_settings(Path(config)))
        names=[key.removeprefix(PREFIX) for key,_,_ in selected if key.startswith(PREFIX)]
        original=json.loads(selection.read_text()) if selection.exists() else {'enabled':[],'disabled':[]}
        if catalog['messages'] and any(key.startswith(PREFIX) and key.removeprefix(PREFIX) not in catalog['entries'] for key in original.get('enabled',[])):
            raise ValueError('An enabled ZIP could not be validated; check the ZIP mod messages')
        if not names:lock.__exit__(None,None,None);return Path(game),str(config)
        if mods.selected(root,Path(environment.get('BB_MODS_CONFIG',Path(data)/'mods.json'))):
            raise ValueError('ZIP packages cannot yet be combined with enabled loose-file mods; disable those first')
        shutil.copy2(config,state/'bbport.ini')
        prefs=state/'selection.json';save_json(prefs,{'disabled':[n for n in catalog['entries'] if n not in names]})
        plan=nx.prepare(port,state,game,root,prefs,state/'bbport.ini',environment)
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
        writes=preflight(plan['profile'],ini,snapshot)
        if plan['has_shaders'] and not prepare_only:
            from nitro_shader_setup import ensure_shaders
            ensure_shaders(port,state,game,user,plan,environment,lambda stage,message:print(message,flush=True))
            for op,folder in plan['operations']:
                if op['kind'].startswith('shader_'):apply_shader(under(user,op['target']),op,folder)
        save_json(session/'launch.json',{'packages':names,'variants':plan['variants'],'writes':writes,
            'resolution':plan['selection_settings'].get('output_res'),'online':plan['settings'].get('online','0')})
        environment.update(plan['environment'])
        environment.update(BB_CONFIG=str(ini),BB_USER_DIR=str(user),BB_PATCHES_DIR=str(plan['profile']/'patches'),BB_PATCHES_CONFIG=str(snapshot),BB_MODS_ENABLED='0',BB_CLEAR_SHADER_CACHE='0')
        print('ZIP mod session:',', '.join(names),'at',plan['selection_settings'].get('output_res'),flush=True)
        print('Separate mod saves and logs:',session,flush=True)
        ACTIVE=(session,state);atexit.register(lock.__exit__,None,None,None)
        return plan['profile']/'game',str(ini)
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
        from nitro_library import remove
        print('Mod ZIP moved to:',remove(args.scan/'mods',args.scan/'patches',args.remove,args.expected_sha))
    else:
        with launch_lock(args.scan/'mod-packages'):
            result=scan(args.scan/'mods',args.scan/'patches')
        for message in result['messages']:print(message)
        print('ZIP packages:',len(result['entries']))
