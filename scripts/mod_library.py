"""One OCTR mod library: native PAKs/folders and declarative ZIP packages.
Author: tcosi. SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import hashlib,json,os,re,shutil,stat,struct,tempfile,uuid,zipfile
import xml.etree.ElementTree as ET
import mod_format as fmt
from mod_components import save_json,sha,under,regular_tree,copy_tree,long_path,plain_path

FILENAME='OCTR-Mod-Library.xml'
PREFIX=FILENAME+'/Mod: '
MAX_EXPANDED=12*1024**3

def storage(root):return long_path(Path(root).parent/'mod-state')  # extended form: deep package paths
def read(root):
    p=storage(root)/'library.json'
    return json.loads(p.read_text(encoding='utf-8')) if p.is_file() else {'entries':{},'messages':[]}
def entries(root):return read(root)['entries']
def folder(root,name):
    item=entries(root).get(name)
    if item is None:raise ValueError('Mod is no longer available: '+name)
    return under(storage(root)/'packages',item['sha256']+'/'+name)
def ident_for(path):
    slug=re.sub('[^a-z0-9]+','-',path.stem.lower()).strip('-')[:60] or 'mod'
    return 'asset-'+slug+'-'+hashlib.sha256(path.name.casefold().encode()).hexdigest()[:8]

def check_pak(path):
    """Check native IGA archive index, without modifying or extracting assets."""
    size=path.stat().st_size
    with path.open('rb') as f:header=f.read(56)
    if len(header)!=56 or struct.unpack_from('<I',header)[0]!=0x1a414749:raise ValueError('Not a native IGA PAK: '+path.name)
    count=struct.unpack_from('<I',header,12)[0];names=struct.unpack_from('<Q',header,40)[0]
    if count>1000000 or 56+count*20>size or names>=size:raise ValueError('Invalid/truncated PAK index: '+path.name)
    from archive_reader import Archive
    a=Archive(path)
    try:
        seen=set()
        for item in a.files:
            name=item['path'].casefold()
            if not name or name in seen:raise ValueError('Duplicate/empty PAK resource path')
            seen.add(name)
            if item['offset']>=size and item['size']:raise ValueError('PAK resource offset outside file')
            if item['compression']==0xffffffff and item['offset']+item['size']>size:raise ValueError('Truncated PAK resource')
    finally:a.data.close();a._file.close()

def legacy_files(source):
    """Normalize simple native mods; archives retain the creator's filenames."""
    if source.is_file():
        check_pak(source);return [('archives/'+source.name,source)]
    import mods
    layout=mods.content_root(source)
    if layout is None:
        paks=[p for p in source.iterdir() if p.is_file() and p.suffix.lower()=='.pak']
        if not paks:raise ValueError('Expected archives/ or native .pak files')
        records=[('archives/'+p.name,p) for p in paks]
    else:records=[(p.as_posix(),s) for p,s in mods.mod_files(source)]
    if len({n.casefold() for n,_ in records})!=len(records):raise ValueError('Duplicate asset target')
    for target,p in records:
        if target.casefold().startswith('archives/'):
            if p.suffix.lower()!='.pak':raise ValueError('Only native PAKs may be placed in archives/')
            check_pak(p)
    if not records:raise ValueError('No supported mod assets')
    return records

def unpack_legacy(package,dest):
    """Asset-only ZIP; executable/XML files require an explicit manifest."""
    dest.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(package) as z:
        seen=set();total=0
        for info in z.infolist():
            name=info.filename.rstrip('/')
            if not name:continue
            fmt.safe_path(name)
            key=name.casefold()
            if key in seen:raise ValueError('Duplicate ZIP path')
            seen.add(key);mode=info.external_attr>>16
            if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) and not (stat.S_ISREG(mode) or stat.S_ISDIR(mode))):raise ValueError('ZIP links/special files unsupported')
            if info.flag_bits&1:raise ValueError('Encrypted ZIP unsupported')
            total+=info.file_size
            if total>MAX_EXPANDED:raise ValueError('ZIP exceeds expanded size limit')
            if info.is_dir():continue
            if Path(name).suffix.lower() not in ('.pak','.txt','.md','.png','.jpg','.jpeg'):
                raise ValueError('Non-asset ZIP needs mod.json: '+name)
            target=under(dest,name);target.parent.mkdir(parents=True,exist_ok=True)
            with z.open(info) as src,target.open('wb') as output:shutil.copyfileobj(src,output)
    # One ordinary wrapper directory is accepted, as in the base loose loader.
    root=dest
    while True:
        try:legacy_files(root);return root
        except ValueError as error:
            dirs=[p for p in root.iterdir() if p.is_dir()]
            files=[p for p in root.iterdir() if p.is_file() and p.suffix.lower() not in ('.txt','.md','.png','.jpg','.jpeg')]
            if len(dirs)!=1 or files:raise error
            root=dirs[0]

def sync_xml(root,patchdir):
    """Bridge loose mods/*.xml into the existing compiler, preserving patch keys.

    Only our recorded, unchanged copies can be replaced or removed. Legacy files
    remain authoritative; identical copies are listed once, collisions are errors.
    """
    patchdir.mkdir(parents=True,exist_ok=True)
    state=storage(root)/'xml-imports.json'
    previous=json.loads(state.read_text(encoding='utf-8')) if state.exists() else {}
    for name,digest in previous.items():
        target=under(patchdir,name)
        if target.is_file() and sha(target)==digest:target.unlink()
    imported={};messages=[]
    for source in sorted(root.iterdir()):
        if source.suffix.lower()!='.xml' or not source.is_file():continue
        try:
            if source.is_symlink():raise ValueError('Linked XML unsupported')
            if source.stat().st_size>16*1024**2:raise ValueError('XML exceeds 16 MB')
            raw=source.read_bytes();tree=ET.fromstring(raw)
            if tree.tag!='Patch' or not list(tree.iter('Metadata')):raise ValueError('Expected shadPS4 Patch XML with Metadata')
            if source.name.casefold().startswith('octr-mod-'):raise ValueError('Reserved generated patch filename')
            # Case-insensitive destinations also work consistently on Linux.
            existing=next((p for p in patchdir.iterdir() if p.name.casefold()==source.name.casefold()),None)
            if existing:
                if existing.read_bytes()!=raw:raise ValueError('Different XML with this filename already exists in patches; rename this add-on')
                continue
            # Importing a new XML must never activate its patches automatically.
            for meta in tree.iter('Metadata'):meta.set('isEnabled','false')
            target=patchdir/source.name
            target.write_bytes(ET.tostring(tree,encoding='utf-8',xml_declaration=True))
            imported[source.name]=sha(target)
        except (ValueError,OSError,ET.ParseError) as error:messages.append(source.name+': '+str(error))
    save_json(state,imported)
    return messages

def scan(root,patchdir):
    root=Path(root);root.mkdir(parents=True,exist_ok=True);patchdir=Path(patchdir)
    cache=storage(root)/'packages';cache.mkdir(parents=True,exist_ok=True)
    found={};messages=sync_xml(root,patchdir);candidates={}
    for source in sorted(root.iterdir(),key=lambda p:p.name.casefold()):
        if not source.is_dir() and source.suffix.lower() not in ('.zip','.pak'):continue
        try:
            if source.is_symlink() or (hasattr(source,'is_junction') and source.is_junction()):raise ValueError('Linked mod inputs unsupported')
            if source.is_file() and source.stat().st_size>4*1024**3:raise ValueError('Mod exceeds4GB')
            if source.is_dir():
                raw=list(regular_tree(source));digest=hashlib.sha256(json.dumps([[p.relative_to(source).as_posix(),sha(p)] for p in sorted(raw)]).encode()).hexdigest()
                manifest=fmt.validate_folder(source) if (source/'mod.json').exists() else None
                logical=source
            else:
                digest=sha(source);manifest=None;logical=source
                if source.suffix.lower()=='.zip':
                    inspected=fmt.inspect_zip(source)
                    if inspected:manifest,_=inspected
            ident=manifest['id'] if manifest else ident_for(source)
            dest=cache/digest/ident
            if not dest.exists():
                dest.parent.mkdir(exist_ok=True)
                with tempfile.TemporaryDirectory(prefix='stage-',dir=cache) as temporary:
                    staging=Path(temporary)/'package';staging.mkdir()
                    if manifest:
                        if source.is_dir():copy_tree(source,staging)
                        else:
                            unpack=Path(temporary)/'unpack';unpack.mkdir()
                            extracted=fmt.unpack_v2(source,unpack,inspected[1]);copy_tree(extracted,staging)
                    else:
                        if source.is_file() and source.suffix.lower()=='.zip':logical=unpack_legacy(source,Path(temporary)/'unpack')
                        for name,p in legacy_files(logical):
                            target=under(staging,name);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,target)
                        save_json(dest.parent/(ident+'.sha256.json'),{p.relative_to(staging).as_posix():sha(p) for p in regular_tree(staging)})
                    os.replace(staging,dest)
            if manifest:fmt.validate_folder(dest)
            else:
                expected=json.loads((dest.parent/(ident+'.sha256.json')).read_text())
                actual={p.relative_to(dest).as_posix():sha(p) for p in regular_tree(dest)}
                if actual!=expected:raise ValueError('Cached mod assets changed')
                legacy_files(dest)
            if source.is_file() and sha(source)!=digest:raise ValueError('Mod changed while scanning')
            item=dict(source=source.name,sha256=digest,name=manifest['name'] if manifest else source.stem,
                version=manifest['version'] if manifest else '1.0.0',author=manifest['author'] if manifest else 'Local asset mod',
                description=manifest['description'] if manifest else '',kind='package' if manifest else 'assets')
            item['content_sha256']=hashlib.sha256(json.dumps(manifest,sort_keys=True).encode()).hexdigest() if manifest else digest
            candidates.setdefault(ident,[]).append(item)
        except (ValueError,OSError,KeyError,TypeError,zipfile.BadZipFile,RuntimeError,AssertionError,struct.error,IndexError) as error:
            messages.append(source.name+': '+str(error))
    for ident,items in candidates.items():
        usable=items
        latest=max(fmt.version(i['version']) for i in usable)
        top=[i for i in usable if fmt.version(i['version'])==latest]
        if len({i['content_sha256'] for i in top})>1:
            messages.append('Conflicting content for '+ident+' version '+top[0]['version']+'; remove one of: '+', '.join(i['source'] for i in top))
            continue
        item=top[0]
        item['version_notice']=''
        if len(items)>1:
            item['version_notice']='Using '+item['version']+'. Other copies retained: '+', '.join(i['source']+' ('+i['version']+')' for i in items if i is not item)
        found[ident]=item
    result={'entries':found,'messages':messages};save_json(storage(root)/'library.json',result)
    xml=ET.Element('Patch');ids=ET.SubElement(xml,'TitleID');ET.SubElement(ids,'ID').text='CUSA13795'
    for ident,item in sorted(found.items()):
        meta=ET.SubElement(xml,'Metadata',Title='Crash Team Racing Nitro-Fueled',Name='Mod: '+ident,
            Author=item['author'],PatchVer=item['version'],AppVer='01.21',AppElf='eboot.bin',isEnabled='false',
            ModName=item['name'],ModHash=item['sha256'],ModSource=item['source'],ModDescription=item['description'],Note='OCTR mod: assets and any logic patches switch together. Relaunch after changes.')
        meta.set('ModVersions',item['version_notice'])
        ET.SubElement(meta,'PatchList')
    patchdir.mkdir(parents=True,exist_ok=True);target=patchdir/FILENAME
    payload=ET.tostring(xml,encoding='utf-8',xml_declaration=True)
    if not target.exists() or target.read_bytes()!=payload:
        tmp=target.with_suffix('.tmp');tmp.write_bytes(payload);os.replace(tmp,target)
    return result

def remove(root,patchdir,ident,expected_sha256):
    """Move precisely the displayed file/folder to recoverable mod-state storage."""
    from mod_session import launch_lock
    fmt.slug(ident);fmt.hash_string(expected_sha256);root=Path(root)
    with launch_lock(storage(root)):
        catalog=scan(root,patchdir);item=catalog['entries'].get(ident)
        if item is None or item['sha256']!=expected_sha256:raise ValueError('Mod changed; Reload before removing')
        source=under(root,item['source']);config=root.parent/'patches.json'
        previous=json.loads(config.read_text()) if config.exists() else {};settings=dict(previous);key=PREFIX+ident
        if not isinstance(previous,dict) or any(not isinstance(previous.get(k,[]),list) or not all(isinstance(v,str) for v in previous.get(k,[])) for k in ('enabled','disabled')):
            raise ValueError('Invalid patch selection; mod not removed')
        settings['enabled']=[k for k in settings.get('enabled',[]) if k!=key]
        settings['disabled']=list(dict.fromkeys(settings.get('disabled',[])+[key]))
        target=storage(root)/'removed'/uuid.uuid4().hex/source.name;target.parent.mkdir(parents=True)
        save_json(config,settings)
        try:source.rename(target)
        except OSError:save_json(config,previous);raise
        scan(root,patchdir);return plain_path(target)
