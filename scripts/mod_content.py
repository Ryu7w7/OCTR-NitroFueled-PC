"""Shared package discovery and composition for OCTR. Author: tcosi.
SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import json,os,shutil,tempfile,time
import xml.etree.ElementTree as ET
import mods
import mod_format as fmt
from mod_library import folder as package_folder,entries as zip_entries
from mod_components import sha,sha_bytes,under,regular_tree,snapshot,read_ini,save_json

def engine(root):
    """The game executable: bin/sacredfire.exe on Windows; on Linux bin/sacredfire in a packaged
    build (BB_PREBUILT, the AppImage), else build.sh's out/sacredfire; the first that exists."""
    root=Path(root)
    if os.name=='nt':return root/'bin/sacredfire.exe'
    names=['bin/sacredfire','out/sacredfire'] if os.environ.get('BB_PREBUILT') else ['out/sacredfire','bin/sacredfire']
    return next((root/n for n in names+['bin/sacredfire.exe'] if (root/n).is_file()),root/names[0])

def metadata(folder):
    path=Path(folder)/'mod.json'
    return json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {}
def extended(folder):return 'schemaVersion' in metadata(folder)

def overlay_files(cache):
    files=[cache/'archive-plan.json']
    for name in ('files','archives','patches'):
        files.extend(regular_tree(cache/name))
    return {p.relative_to(cache).as_posix():sha(p) for p in files}

def publish_profile(staging,cache,log):
    """Publish generated mod metadata, allowing transient Windows locks to clear."""
    for attempt in range(8):
        try:
            os.replace(staging,cache)
            return
        except OSError as error:
            # Never overwrite another prepared directory or retry unrelated errors.
            if getattr(error,'winerror',None) not in (5,32,33) or cache.exists() or attempt==7:
                raise
            if attempt==0:log('Waiting briefly for Windows to release the prepared mod folder...')
            time.sleep(.1*(attempt+1))
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
        # Texts claim object::field under their entry: packages changing different texts of one
        # entry compose, and a whole-entry edit of it still conflicts with them.
        if op['kind']=='localized_strings':
            keys=['archives/'+op['archive']+'::'+op['asset']+'::'+name+'::'+field for name,fields in op['strings'].items() for field in fields]
        elif op['kind']=='archive_asset':keys=['archives/'+op['archive']+'::'+op['asset']]
        elif op['kind'].startswith('shader_'):keys=['@shader/'+op['target']]
        else:keys=[op['target']]
        items.extend((key.casefold(),folder/'mod.json') for key in keys)
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
    from prepare import sfo
    game_info=sfo((game/'sce_sys/param.sfo').read_bytes())
    title_id,app_version=game_info.get('TITLE_ID'),game_info.get('APP_VER')
    if title_id not in ('CUSA13795','CUSA14876') or app_version!='01.21':
        raise ValueError('Mod packages require a supported CTR edition at 01.21')
    base=read_ini(base_config)
    base.setdefault('widescreen_layout','off')
    problems=conflicts(mod_root,selection)
    if problems:raise ValueError('Mod conflicts: '+'; '.join(problems))
    names=selected(mod_root,selection)
    from mod_variants import choose
    base,automatic_settings=choose([metadata(package_folder(mod_root,n)) for n in names],base)
    problems=conflicts(mod_root,selection,base)
    if problems:raise ValueError('Mod conflicts: '+'; '.join(problems))
    packages=[];operations=[];settings={};overrides={};patches=[];whole=[]
    runtime=sha(engine(root))
    effective=base.copy();source_state=snapshot(game);chosen_variants={}
    for name in names:
        folder=package_folder(mod_root,name)
        if extended(folder):
            raw=fmt.validate_folder(folder)
            variant,m=fmt.resolve(raw,base)
            if variant:chosen_variants[name]=variant
            if m['id']!=name:raise ValueError('Package ID/folder mismatch: '+name)
            compat=m['compatibility']
            # Compatibility is established from affected assets and executable sites,
            # not a whole-file executable or engine allowlist.
            if compat['online']=='offline-only' and environment.get('BB_ONLINE',effective.get('online','0'))=='1':
                raise ValueError(name+': offline-only until online compatibility is tested. Set online=0 in this profile bbport.ini.')
            for field,target in (('settings',settings),('environment',overrides)):
                for key,value in m.get(field,{}).items():
                    if key in target and target[key]!=value:raise ValueError('Conflicting '+field+': '+key)
                    target[key]=value
            operations.extend((op,folder) for op in m.get('operations',[]))
            for patch_name in m.get('patches',[]):
                patch_path=under(folder,patch_name)
                patch_xml=ET.parse(patch_path).getroot()
                patch_ids=[node.text for node in patch_xml.findall('./TitleID/ID')]
                if patch_ids!=[title_id] or any(meta.get('AppVer')!=app_version for meta in patch_xml.iter('Metadata')):
                    raise ValueError(name+': executable patch targets a different game edition/version')
                patches.append((patch_path,name))
            whole.extend((under(folder,r['file']),r['file']) for r in m['files'] if r['file'].startswith('archives/'))
            packages.append({'id':name,'manifest':sha(folder/'mod.json'),'variant':variant})
        else:
            files=list(mods.mod_files(folder));whole.extend((source,path.as_posix()) for path,source in files)
            packages.append({'id':name,'files':[[p.as_posix(),sha(s)] for p,s in files]})
    effective.update(settings)
    for name in names:
        raw=metadata(package_folder(mod_root,name))
        m=fmt.resolve(raw,base)[1] if 'schemaVersion' in raw else raw
        for key,values in m.get('constraints',{}).items():
            if effective.get(key) not in values:raise ValueError(name+': '+key+' must be one of '+', '.join(values))
    identity={'game':str(game),'target':[title_id,app_version],'target_policy':1,'source_files':source_state,'packages':packages,'settings':effective,
        'environment':overrides,'engine':runtime,'run':sha(root/'run.py'),
        'patch_compiler':sha(root/'scripts/patches.py'),
        'base_patches':{p.name:sha(p) for p in (root/'patches').glob('*.xml')}}
    key=sha_bytes(json.dumps(identity,sort_keys=True).encode())[:24]
    # New namespace: old full-game profiles must never be reused by this loader.
    cache=state/'extensions/overlays'/key;ready=cache/'ready.json'
    if ready.exists():
        r=json.loads(ready.read_text())
        if r['identity']!=identity or r['files']!=overlay_files(cache):
            raise ValueError('Prepared files changed; preserved for inspection: '+str(cache))
    else:
        cache.parent.mkdir(parents=True,exist_ok=True)
        staging=Path(tempfile.mkdtemp(prefix='building-',dir=cache.parent))
        try:
            log('Preparing mod overlays (no game copies): '+', '.join(names))
            (staging/'files').mkdir()
            for op,folder in operations:
                if op['kind']=='game_file':
                    original=under(game,op['target'])
                    if op.get('original_sha256') is None:
                        if original.exists():raise ValueError('Expected a new file: '+op['target'])
                        data=under(folder,op['payload']).read_bytes()
                        if sha_bytes(data)!=op['patched_sha256']:raise ValueError('Replacement checksum mismatch')
                    else:
                        if sha(original)!=op['original_sha256']:raise ValueError('Original content checksum mismatch')
                        data=under(folder,op['payload']).read_bytes()
                        if sha_bytes(data)!=op['patched_sha256']:raise ValueError('Replacement checksum mismatch')
                    # Only the mod's replacement content, never an unchanged game file.
                    dest=under(staging/'files',op['target']);dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
            records=[{'operation':op,'folder':str(folder)} for op,folder in operations if op['kind'] in ('archive_asset','localized_strings')]
            save_json(staging/'archive-plan.json',records)
            from mod_overlays import compose
            compose(game,staging/'archives',[(r['operation'],r['folder']) for r in records],{})
            (staging/'patches').mkdir()
            for p in (root/'patches').glob('*.xml'):shutil.copy2(p,staging/'patches'/p.name)
            if (staging/'patches/OCTR-Mod-Content.xml').exists():raise ValueError('Reserved generated patch filename is already used')
            xml=ET.Element('Patch');ids=ET.SubElement(xml,'TitleID');ET.SubElement(ids,'ID').text=title_id
            for path,name in patches:
                for meta in ET.parse(path).getroot().iter('Metadata'):
                    meta.set('Name',name+': '+meta.get('Name',''));meta.set('isEnabled','true');xml.append(meta)
            ET.ElementTree(xml).write(staging/'patches/OCTR-Mod-Content.xml',encoding='utf-8',xml_declaration=True)
            if source_state!=snapshot(game):raise ValueError('Original game changed while preparing the view')
            save_json(staging/'ready.json',{'identity':identity,
                'files':overlay_files(staging)})
            publish_profile(staging,cache,log)
        except BaseException:
            log('Incomplete preparation preserved at '+str(staging));raise
    # Full PAKs/loose files are read directly from mod storage by the engine.
    replacements=[(Path(target),source) for source,target in whole]
    replacements.extend((p.relative_to(cache/'files'),p) for p in regular_tree(cache/'files'))
    from mod_overlays import file_map
    file_map(game,cache/'file-map.bin',cache/'directories',replacements)
    return {'profile':cache,'game':game,'archive_plan':cache/'archive-plan.json','file_map':cache/'file-map.bin','file_dirs':cache/'directories','settings':effective,'environment':overrides,'operations':operations,
            'engine':runtime,'packages':names,'variants':chosen_variants,
            'selection_settings':base,'automatic_settings':automatic_settings,
            'has_shaders':any(op['kind'].startswith('shader_') for op,_ in operations)}
