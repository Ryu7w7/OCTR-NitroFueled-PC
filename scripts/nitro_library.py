"""Checked ZIP library for the native launcher's patch screen. GPL-2.0-or-later."""
from pathlib import Path
import json,os,tempfile,zipfile,uuid
import xml.etree.ElementTree as ET
import nitro_format as fmt
from nitro_components import save_json,sha,under

FILENAME='ZIP-Mod-Packages.xml'
PREFIX=FILENAME+'/ZIP mod: '

def storage(root):return Path(root).parent/'mod-packages'
def read(root):
    p=storage(root)/'library.json'
    return json.loads(p.read_text(encoding='utf-8')) if p.is_file() else {'entries':{},'messages':[]}
def entries(root):return read(root)['entries']
def folder(root,name):
    item=entries(root).get(name)
    if item is None:return Path(root)/name
    return under(storage(root)/'packages',item['sha256']+'/'+name)

def scan(root,patchdir):
    root=Path(root);patchdir=Path(patchdir);root.mkdir(parents=True,exist_ok=True)
    cache=storage(root)/'packages';cache.mkdir(parents=True,exist_ok=True)
    found={};messages=[];duplicates=set()
    for package in sorted(root.iterdir()):
        if package.suffix.lower()!='.zip':continue
        try:
            if package.is_symlink() or (hasattr(package,'is_junction') and package.is_junction()):raise ValueError('linked ZIP')
            if not package.is_file() or package.stat().st_size>4*1024**3:raise ValueError('ZIP exceeds 4 GB')
            stamp=(package.stat().st_size,package.stat().st_mtime_ns)
            inspected=fmt.inspect_zip(package)
            if inspected is None:raise ValueError('requires nitro-mod.json schemaVersion 2')
            manifest,path=inspected;ident=manifest['id'];digest=sha(package)
            stage=cache/digest;dest=stage/ident
            if not dest.is_dir():
                stage.mkdir(exist_ok=True)
                with tempfile.TemporaryDirectory(prefix='extract-',dir=cache) as temp:
                    extracted=fmt.unpack_v2(package,Path(temp),path)
                    if dest.exists():raise ValueError('package cache changed during extraction')
                    os.replace(extracted,dest)
            # Revalidate cached bytes, not just source ZIP metadata.
            fmt.validate_folder(dest)
            if stamp!=(package.stat().st_size,package.stat().st_mtime_ns):raise ValueError('ZIP is still being copied')
            item={'zip':package.name,'sha256':digest,'name':manifest['name'],'version':manifest['version'],'author':manifest['author']}
            if ident in found:duplicates.add(ident)
            else:found[ident]=item
        except (ValueError,OSError,KeyError,TypeError,zipfile.BadZipFile,RuntimeError) as error:
            messages.append(package.name+': '+str(error))
    for ident in duplicates:
        found.pop(ident,None);messages.append('Duplicate mod ID: '+ident+'; keep one ZIP per mod')
    result={'entries':found,'messages':messages}
    if result!=read(root):save_json(storage(root)/'library.json',result)
    xml=ET.Element('Patch');ids=ET.SubElement(xml,'TitleID');ET.SubElement(ids,'ID').text='CUSA13795'
    for ident,item in sorted(found.items()):
        meta=ET.SubElement(xml,'Metadata',Title='Crash Team Racing Nitro-Fueled',Name='ZIP mod: '+ident,
            Author=item['author'],PatchVer=item['version'],AppVer='01.21',AppElf='eboot.bin',isEnabled='false',
            ModName=item['name'],ModHash=item['sha256'],
            Note=item['name']+'. ZIP package: XML, assets, shaders and settings switch together. Fully relaunch after changing mods.')
        ET.SubElement(meta,'PatchList')
    patchdir.mkdir(parents=True,exist_ok=True);target=patchdir/FILENAME
    payload=ET.tostring(xml,encoding='utf-8',xml_declaration=True)
    if not target.exists() or target.read_bytes()!=payload:
        temporary=target.with_suffix('.xml.tmp');temporary.write_bytes(payload);os.replace(temporary,target)
    return result

def remove(root,patchdir,ident,expected_sha256):
    """Recoverably remove exactly the ZIP displayed by the launcher, while idle."""
    from mod_session import launch_lock
    fmt.slug(ident);fmt.hash_string(expected_sha256)
    root=Path(root);patchdir=Path(patchdir)
    with launch_lock(storage(root)):
        catalog=scan(root,patchdir)
        item=catalog['entries'].get(ident)
        if item is None or item['sha256']!=expected_sha256:
            raise ValueError('Mod ZIP changed or is unavailable; Reload before removing it')
        source=under(root,item['zip'])
        if not source.is_file() or source.is_symlink() or sha(source)!=expected_sha256:
            raise ValueError('Mod ZIP changed; Reload before removing it')
        config=root.parent/'patches.json'
        previous=json.loads(config.read_text(encoding='utf-8')) if config.exists() else {'enabled':[],'disabled':[]}
        if not isinstance(previous,dict) or any(not isinstance(previous.get(k,[]),list) or
            not all(isinstance(v,str) for v in previous.get(k,[])) for k in ('enabled','disabled')):
            raise ValueError('Invalid patch selection; ZIP was not removed')
        prefs=dict(previous);key=PREFIX+ident
        prefs['enabled']=[v for v in previous.get('enabled',[]) if v!=key]
        prefs['disabled']=list(dict.fromkeys(previous.get('disabled',[])+[key]))
        target=storage(root)/'removed'/uuid.uuid4().hex/source.name
        target.parent.mkdir(parents=True,exist_ok=True)
        save_json(config,prefs)
        try:source.rename(target)
        except OSError:
            save_json(config,previous);raise
        scan(root,patchdir)
        return target
