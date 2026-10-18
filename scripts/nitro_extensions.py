"""Shared package discovery and composition for Nitro. Author: tcosi.
SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import hashlib,json,os,shutil,struct,subprocess,sys,tempfile,uuid
import xml.etree.ElementTree as ET
import mods
import nitro_format as fmt
from nitro_library import folder as package_folder,entries as zip_entries
from nitro_components import sha,sha_bytes,under,regular_tree,snapshot,copy_tree,read_ini,save_json,apply_file,patch_archive,apply_shader

def metadata(folder):
    path=Path(folder)/'nitro-mod.json'
    return json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {}
def extended(folder):return 'schemaVersion' in metadata(folder)
def discover(root):
    root=Path(root)
    if not root.exists():return []
    found=set()
    for name in zip_entries(root):
        folder=package_folder(root,name)
        if folder.is_dir():
            m=metadata(folder)
            if 'schemaVersion' in m:fmt.validate_manifest(m)
            found.add(name)
    return sorted(found,key=lambda s:(s.casefold(),s))
def selected(root,config):
    available=discover(root)
    prefs=json.loads(Path(config).read_text()) if Path(config).is_file() else {}
    for key in ('order','disabled'):
        if not isinstance(prefs.get(key,[]),list) or not all(isinstance(n,str) for n in prefs.get(key,[])):raise ValueError('Invalid mod selection '+key)
    return list(dict.fromkeys(n for n in [*prefs.get('order',[]),*available] if n in available and n not in prefs.get('disabled',[])))
def claims(folder,settings=None):
    folder=Path(folder)
    if not extended(folder):return [(p.as_posix().casefold(),source) for p,source in mods.mod_files(folder)]
    m=fmt.validate_manifest(metadata(folder));items=[]
    for r in m['files']:
        if r['file'].startswith('archives/'):items.append((r['file'].casefold(),under(folder,r['file'])))
    alternatives=[fmt.resolve(m,settings)[1]] if settings is not None else [flat for _,flat in fmt.expanded_variants(m)]
    operations=[op for flat in alternatives for op in flat.get('operations',[])]
    for op in operations:
        if op['kind']=='archive_asset':key='archives/'+op['archive']+'::'+op['asset']
        elif op['kind'].startswith('shader_'):key='@shader/'+op['target']
        else:key=op['target']
        items.append((key.casefold(),folder/'nitro-mod.json'))
    return list(dict(items).items()) if settings is None else items
def mod_files(folder):return [(Path(key),source) for key,source in claims(folder)]
def conflicts(root,config,settings=None):
    names=selected(root,config);owners=[];problems=[];installed={n:metadata(package_folder(root,n)) for n in names}
    for name in names:
        for key,source in claims(package_folder(root,name),settings):
            for prior,owner in owners:
                if key==prior or key.startswith(prior+'::') or prior.startswith(key+'::'):
                    problems.append(key+': '+owner+' / '+name)
            owners.append((key,name))
        m=installed[name]
        if 'schemaVersion' in m:
            for r in m.get('requires',[]):
                dep=installed.get(r['id'])
                if dep is None:problems.append(name+': enable dependency '+r['id'])
                elif not fmt.matches(dep.get('version',''),r['version']):problems.append(name+': dependency '+r['id']+' needs '+r['version'])
            for other in m.get('conflicts',[]):
                if other in installed:problems.append(name+': incompatible with '+other)
    visiting=set();visited=set()
    def visit(name):
        if name in visiting:
            problems.append('Dependency cycle involving '+name);return
        if name in visited:return
        visiting.add(name)
        for dep in installed[name].get('requires',[]):
            if dep['id'] in installed:visit(dep['id'])
        visiting.remove(name);visited.add(name)
    for name in names:visit(name)
    return list(dict.fromkeys(problems))

def has_extended(root,config):return any((n in zip_entries(root) or extended(package_folder(root,n))) for n in selected(root,config))

def prepare(root,state,game,mod_root,selection,base_config,environment,log=print):
    root=Path(root).resolve();state=Path(state).resolve();game=Path(game).resolve();mod_root=Path(mod_root)
    base=read_ini(base_config)
    problems=conflicts(mod_root,selection)
    if problems:raise ValueError('Mod conflicts: '+'; '.join(problems))
    names=selected(mod_root,selection)
    from nitro_variant_settings import choose
    base,automatic_settings=choose([metadata(package_folder(mod_root,n)) for n in names],base)
    problems=conflicts(mod_root,selection,base)
    if problems:raise ValueError('Mod conflicts: '+'; '.join(problems))
    packages=[];operations=[];settings={};overrides={};patches=[];whole=[]
    runtime=sha(root/'bin/sacredfire.exe');runtime_version=(root/'bin/VERSION').read_text().strip();eboot=sha(game/'eboot.bin')
    effective=base.copy();source_state=snapshot(game);chosen_variants={}
    for name in names:
        folder=package_folder(mod_root,name)
        if extended(folder):
            raw=fmt.validate_folder(folder)
            variant,m=fmt.resolve(raw,base)
            if variant:chosen_variants[name]=variant
            if m['id']!=name:raise ValueError('Package ID/folder mismatch: '+name)
            compat=m['compatibility']
            if runtime_version not in compat['runtime_versions'] or runtime not in compat['runtime_sha256']:
                raise ValueError(name+': unsupported runtime build; package must declare this exact engine')
            if eboot not in compat['eboot_sha256']:raise ValueError(name+': unsupported game executable')
            if compat['online']=='offline-only' and environment.get('BB_ONLINE',effective.get('online','0'))=='1':
                raise ValueError(name+': offline-only until online compatibility is tested. Set online=0 in this profile bbport.ini.')
            for field,target in (('settings',settings),('environment',overrides)):
                for key,value in m.get(field,{}).items():
                    if key in target and target[key]!=value:raise ValueError('Conflicting '+field+': '+key)
                    target[key]=value
            operations.extend((op,folder) for op in m.get('operations',[]))
            patches.extend((under(folder,p),name) for p in m.get('patches',[]))
            whole.extend((under(folder,r['file']),r['file']) for r in m['files'] if r['file'].startswith('archives/'))
            packages.append({'id':name,'manifest':sha(folder/'nitro-mod.json'),'variant':variant})
        else:
            files=list(mods.mod_files(folder));whole.extend((source,path.as_posix()) for path,source in files)
            packages.append({'id':name,'files':[[p.as_posix(),sha(s)] for p,s in files]})
    effective.update(settings)
    for name in names:
        raw=metadata(package_folder(mod_root,name))
        m=fmt.resolve(raw,base)[1] if 'schemaVersion' in raw else raw
        for key,values in m.get('constraints',{}).items():
            if effective.get(key) not in values:raise ValueError(name+': '+key+' must be one of '+', '.join(values))
    identity={'game':str(game),'source_files':source_state,'packages':packages,'settings':effective,
        'environment':overrides,'engine':runtime,'run':sha(root/'run.py'),
        'patch_compiler':sha(root/'scripts/patches.py'),
        'base_patches':{p.name:sha(p) for p in (root/'patches').glob('*.xml')}}
    key=sha_bytes(json.dumps(identity,sort_keys=True).encode())[:24]
    cache=state/'extensions/profiles'/key;ready=cache/'ready.json'
    if ready.exists():
        r=json.loads(ready.read_text())
        if r['identity']!=identity or r['game_files']!=snapshot(cache/'game') or r['patches']!={p.name:sha(p) for p in (cache/'patches').glob('*.xml')}:
            raise ValueError('Prepared files changed; preserved for inspection: '+str(cache))
    else:
        cache.parent.mkdir(parents=True,exist_ok=True)
        staging=Path(tempfile.mkdtemp(prefix='building-',dir=cache.parent))
        try:
            required=sum(v[0] for v in source_state.values())+sum(p.stat().st_size for p,_ in whole)
            if shutil.disk_usage(staging).free<required+1024**3:raise ValueError('Insufficient space for private modded game')
            log('Preparing private game view: '+', '.join(names));copy_tree(game,staging/'game')
            for source,target in whole:
                dest=under(staging/'game',target);dest.parent.mkdir(parents=True,exist_ok=True)
                shutil.copy2(source,dest)
            groups={}
            for op,folder in operations:
                if op['kind']=='archive_asset':groups.setdefault(op['archive'],[]).append((op,folder))
                elif op['kind']=='game_file':apply_file(under(staging/'game',op['target']),op,folder)
            for archive,items in groups.items():
                log('Patching private archive: '+archive);patch_archive(under(staging/'game','archives/'+archive),items)
            (staging/'patches').mkdir()
            for p in (root/'patches').glob('*.xml'):shutil.copy2(p,staging/'patches'/p.name)
            if (staging/'patches/Nitro-Extensions.xml').exists():raise ValueError('Reserved generated patch filename is already used')
            xml=ET.Element('Patch');ids=ET.SubElement(xml,'TitleID');ET.SubElement(ids,'ID').text='CUSA13795'
            for path,name in patches:
                for meta in ET.parse(path).getroot().iter('Metadata'):
                    meta.set('Name',name+': '+meta.get('Name',''));meta.set('isEnabled','true');xml.append(meta)
            ET.ElementTree(xml).write(staging/'patches/Nitro-Extensions.xml',encoding='utf-8',xml_declaration=True)
            if source_state!=snapshot(game):raise ValueError('Original game changed while preparing the view')
            save_json(staging/'ready.json',{'identity':identity,'game_files':snapshot(staging/'game'),
                'patches':{p.name:sha(p) for p in (staging/'patches').glob('*.xml')}})
            os.replace(staging,cache)
        except BaseException:
            log('Incomplete preparation preserved at '+str(staging));raise
    return {'profile':cache,'settings':effective,'environment':overrides,'operations':operations,
            'engine':runtime,'packages':names,'variants':chosen_variants,
            'selection_settings':base,'automatic_settings':automatic_settings,
            'has_shaders':any(op['kind'].startswith('shader_') for op,_ in operations)}
