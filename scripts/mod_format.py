"""OCTR mod package format 2 proposal. Declarative packages; no downloaded code runs.
Author: tcosi. SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import json,math,re,stat,zipfile
import xml.etree.ElementTree as ET
from mod_components import safe_path,under,sha,regular_tree

MARKER='octr-mod.required'
MARKER_CONTENT=b'OCTR Mod Package 2\n'
CAPABILITIES={'archive-assets','game-files','xml-patches','shaders','settings','environment','dependencies','constraints','variants','variant-settings','code-signatures','localized-strings'}
SETTINGS={'output_res','render_res','widescreen_layout','upscaler','temporal','dlss_quality','dlss_output','fps_limit',
 'patch_60fps','patch_no_motion_blur','patch_pro_1080p','async_pipelines','fullscreen','show_fps','preset','menu_language'}
ENVIRONMENT={'BB_NEO','BB_FPS','BB_VBLANK_HZ','BB_DMEM_MB','BB_PREUPLOAD'}
KINDS={'archive_asset':'archive-assets','game_file':'game-files','shader_file':'shaders',
 'shader_bytes':'shaders','shader_position_scale':'shaders','localized_strings':'localized-strings'}
HEX=re.compile('[0-9a-f]{64}')

def error(message):raise ValueError('OCTR mod format 2: '+message)
def hash_string(value):
    if not isinstance(value,str) or not HEX.fullmatch(value):error('expected a SHA-256 digest')
def slug(value):
    if not isinstance(value,str) or not re.fullmatch('[a-z0-9][a-z0-9-]{0,79}',value):error('invalid package ID')
def version(value):
    if not isinstance(value,str) or not re.fullmatch(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)',value):
        error('versions must be stable major.minor.patch numbers')
    return tuple(map(int,value.split('.')))
def matches(actual,requirement):
    actual=version(actual)
    if not isinstance(requirement,str) or not requirement:error('dependency version range must be a string')
    results=[]
    for term in requirement.split(','):
        term=term.strip();m=re.fullmatch(r'(>=|<=|>|<|=)?(\d+\.\d+\.\d+)',term)
        if not m:error('invalid dependency version range '+requirement)
        op,wanted=m.groups();wanted=version(wanted)
        results.append({None:actual==wanted,'=':actual==wanted,'>=':actual>=wanted,'<=':actual<=wanted,
                '>':actual>wanted,'<':actual<wanted}[op])
    return all(results)
def string_values(values,allowed,label):
    if not isinstance(values,dict):error(label+' must be an object')
    for key,value in values.items():
        if key not in allowed:error('unsupported '+label+' '+key)
        if not isinstance(value,str) or not value or any(c in value for c in '\r\n\0'):
            error(label+' values must be nonempty single-line strings')

def expanded_variants(m):
    """Expand reusable operations and static alternatives; never evaluate package code."""
    if 'variants' not in m:
        if 'operationSets' in m:error('operationSets requires variants')
        return [(None,m)]
    if 'variants' not in m.get('requiresCapabilities',[]):error('missing variants capability')
    variants=m['variants'];sets=m.get('operationSets',{})
    if not isinstance(variants,list) or not variants or len(variants)>64:error('variants must contain 1–64 alternatives')
    if not isinstance(sets,dict) or len(sets)>64:error('invalid operationSets')
    for name,ops in sets.items():
        slug(name)
        if not isinstance(ops,list):error('operationSet must be an operations array')
    ids=set();result=[]
    for v in variants:
        if not isinstance(v,dict) or set(v)-{'id','when','useOperations','operations','patches','settings','environment','constraints'}:
            error('invalid variant fields')
        slug(v.get('id'))
        if v['id'] in ids:error('duplicate variant ID')
        ids.add(v['id']);conditions=v.get('when')
        if not isinstance(conditions,dict) or not conditions:error('variant needs nonempty when conditions')
        for key,values in conditions.items():
            if key not in SETTINGS or not isinstance(values,list) or not values or not all(isinstance(s,str) and s for s in values):
                error('invalid variant condition '+key)
        used=v.get('useOperations',[])
        if not isinstance(used,list) or not all(isinstance(s,str) and s in sets for s in used):error('unknown operationSet')
        if len(used)!=len(set(used)):error('duplicate operationSet reference')
        flat={k:value for k,value in m.items() if k not in ('variants','operationSets')}
        flat['operations']=list(m.get('operations',[]))+[op for name in used for op in sets[name]]+list(v.get('operations',[]))
        flat['patches']=list(m.get('patches',[]))+list(v.get('patches',[]))
        for field in ('settings','environment','constraints'):
            if not isinstance(v.get(field,{}),dict):error('variant '+field+' must be an object')
            flat[field]=dict(m.get(field,{}))|v.get(field,{})
        result.append((v,flat))
    return result

def resolve(m,settings):
    if 'variantSettings' in m:
        from mod_variants import choose
        settings=choose([m],settings)[0]
    alternatives=expanded_variants(m)
    matches=[(v,flat) for v,flat in alternatives if v is None or all(settings.get(k) in values for k,values in v['when'].items())]
    if len(matches)!=1:
        error('exactly one variant must match the base settings; found '+str(len(matches))+'. Check output_res and render_res, then fully relaunch.')
    v,flat=matches[0]
    return (v['id'] if v else None),flat

def validate_manifest(m):
    if not isinstance(m,dict) or m.get('schemaVersion')!=2:error('unsupported schemaVersion')
    if 'variants' in m or 'operationSets' in m:
        alternatives=expanded_variants(m)
        for _,flat in alternatives:validate_manifest(flat)
        for key,rule in m.get('variantSettings',{}).items():
            domains=[set(v['when'][key]) for v,_ in alternatives if key in v['when']]
            if domains and not set(rule['allowed'])<=set.union(*domains):error('variantSettings allowed values are not covered by variants: '+key)
        defaults={k:r['default'] for k,r in m.get('variantSettings',{}).items()}
        if defaults and all(set(v['when'])<=set(defaults) for v,_ in alternatives):
            if sum(all(defaults[k] in values for k,values in v['when'].items()) for v,_ in alternatives)!=1:error('variantSettings defaults must select exactly one variant')
        return m
    allowed={'schemaVersion','id','name','version','author','description','files','requiresCapabilities',
             'compatibility','requires','conflicts','operations','patches','settings','environment','constraints','source','variantSettings'}
    if set(m)-allowed:error('unknown manifest fields: '+', '.join(sorted(set(m)-allowed)))
    slug(m.get('id'));version(m.get('version'))
    for key in ('name','author','description'):
        if not isinstance(m.get(key),str) or not m[key] or len(m[key])>4000:error('missing/invalid '+key)
    caps=m.get('requiresCapabilities')
    if not isinstance(caps,list) or not all(isinstance(c,str) for c in caps) or set(caps)-CAPABILITIES:
        error('unknown or missing required capabilities')
    needed=set()
    if 'variantSettings' in m:
        from mod_variants import validate
        validate(m['variantSettings']);needed.add('variant-settings')
    records=m.get('files')
    if not isinstance(records,list):error('files must be a list')
    files={};folded=set()
    for r in records:
        if not isinstance(r,dict) or set(r)!={'file','sha256','bytes'}:error('invalid file record')
        safe_path(r['file']);hash_string(r['sha256'])
        if r['file']=='mod.json' or r['file'].casefold() in folded:error('duplicate/reserved file record')
        if type(r['bytes']) is not int or r['bytes']<0:error('invalid file size')
        files[r['file']]=r;folded.add(r['file'].casefold())
    if MARKER not in files:error('missing old-loader rejection marker')
    compatibility=m.get('compatibility')
    if not isinstance(compatibility,dict) or not {'online'}<=set(compatibility) or set(compatibility)-{'runtime_versions','runtime_sha256','eboot_sha256','online'}:
        error('compatibility requires online; executable and runtime allowlists are optional legacy metadata')
    for key in ('runtime_versions','runtime_sha256','eboot_sha256'):
        if key not in compatibility:continue
        values=compatibility[key]
        if not isinstance(values,list) or not values or not all(isinstance(v,str) and v for v in values):error('invalid '+key)
        if key.endswith('sha256'):
            for value in values:hash_string(value)
    if compatibility['online'] not in ('offline-only','allowed'):error('online must be offline-only or allowed')
    requirements=m.get('requires',[])
    if not isinstance(requirements,list):error('requires must be a list')
    for r in requirements:
        if not isinstance(r,dict) or set(r)!={'id','version'}:error('dependencies need id and version')
        slug(r['id']);matches('0.0.0',r['version'])
        if r['id']==m['id']:error('a package cannot require itself')
    if requirements:needed.add('dependencies')
    if not isinstance(m.get('conflicts',[]),list):error('conflicts must be a list')
    for ident in m.get('conflicts',[]):slug(ident)
    string_values(m.get('settings',{}),SETTINGS,'settings')
    string_values(m.get('environment',{}),ENVIRONMENT,'environment')
    if m.get('settings'):needed.add('settings')
    if m.get('environment'):needed.add('environment')
    constraints=m.get('constraints',{})
    if not isinstance(constraints,dict):error('constraints must be an object')
    for key,values in constraints.items():
        if key not in SETTINGS or not isinstance(values,list) or not values or not all(isinstance(v,str) for v in values):error('invalid constraint '+key)
    if constraints:needed.add('constraints')
    operations=m.get('operations',[])
    if not isinstance(operations,list):error('operations must be a list')
    def payload(name):
        safe_path(name)
        if name not in files:error('undeclared payload '+name)
    def archive_entry(op):
        safe_path(op['archive']);safe_path(op['asset'])
        if '/' in op['archive'] or not op['archive'].lower().endswith('.pak'):error('archive must be a flat PAK name')
        if type(op.get('index')) is not int or op['index']<0:error('invalid archive index')
    for op in operations:
        if not isinstance(op,dict) or op.get('kind') not in KINDS:error('unsupported operation')
        kind=op['kind'];needed.add(KINDS[kind])
        if kind=='localized_strings':
            # Texts of one entry's igLocalizedStringData objects; packages changing different
            # texts of the same entry compose, so there is no patched checksum.
            if set(op)!={'kind','archive','asset','index','original_sha256','strings'}:
                error('localized_strings needs exactly archive, asset, index, original_sha256 and strings')
            archive_entry(op);hash_string(op['original_sha256'])
            strings=op['strings']
            if not isinstance(strings,dict) or not strings:error('strings must be a nonempty object')
            for name,fields in strings.items():
                if not isinstance(name,str) or not name or not isinstance(fields,dict) or not fields:error('invalid localized string object')
                for field,text in fields.items():
                    if not isinstance(field,str) or not field or not isinstance(text,str) or '\0' in text or len(text)>4000:
                        error('invalid localized string '+name)
            continue
        required={'kind','original_sha256','patched_sha256'}|({'archive','asset','index'} if kind=='archive_asset' else {'target'})
        if kind=='shader_position_scale':required.add('factor')
        if not required<=set(op):error('missing operation fields: '+', '.join(sorted(required-set(op))))
        common={'kind','original_sha256','patched_sha256','payload','runs'}
        permitted=common|({'archive','asset','index'} if kind=='archive_asset' else {'target'})
        if kind=='shader_position_scale':permitted={'kind','target','original_sha256','patched_sha256','factor'}
        if set(op)-permitted:error('unknown operation fields')
        hash_string(op.get('patched_sha256'))
        if kind=='archive_asset':archive_entry(op)
        else:
            safe_path(op['target'])
            if kind=='game_file' and (op['target'].casefold()=='eboot.bin' or op['target'].casefold().startswith(('sce_sys/','sce_module/'))):
                error('executable/system replacement is prohibited; use XML patches')
            if kind.startswith('shader_') and not op['target'].startswith('cache/CUSA13795/'):error('shader target is outside the game cache')
        if op.get('original_sha256') is None:
            if kind not in ('game_file','shader_file'):error('original checksum is required')
            if 'original_sha256' not in op:error('new files require explicit original_sha256: null')
        else:hash_string(op['original_sha256'])
        if kind=='shader_position_scale':
            factor=op.get('factor')
            if type(factor) not in (int,float) or not math.isfinite(factor) or not 0<factor<=2:error('invalid shader factor')
        elif ('payload' in op)==('runs' in op):error('provide exactly one payload or runs')
        elif 'payload' in op:payload(op['payload'])
        else:
            if kind not in ('archive_asset','shader_bytes'):error('deltas unsupported for this operation')
            spans=[]
            for run in op['runs']:
                if not isinstance(run,list) or len(run)!=3:error('invalid delta run')
                offset,old,new=run
                try:a=bytes.fromhex(old);b=bytes.fromhex(new)
                except (ValueError,TypeError):error('invalid hex delta')
                if type(offset) is not int or offset<0 or not a or len(a)!=len(b):error('invalid delta range')
                if any(offset<end and start<offset+len(a) for start,end in spans):error('overlapping delta runs')
                spans.append((offset,offset+len(a)))
    patches=m.get('patches',[])
    if not isinstance(patches,list):error('patches must be a list')
    for path in patches:payload(path)
    if patches:needed.add('xml-patches')
    if 'code-signatures' in m.get('requiresCapabilities',[]):needed.add('code-signatures')
    # Whole archives remain native IGA PAKs; the container format is unchanged.
    for path in files:
        if path.startswith('archives/') and (len(Path(path).parts)!=2 or not path.lower().endswith('.pak')):
            error('use flat archives/*.pak for complete archive payloads')
    if not needed<=set(caps):error('missing capabilities: '+', '.join(sorted(needed-set(caps))))
    return m

def validate_folder(root):
    root=Path(root);manifest=root/'mod.json'
    if manifest.stat().st_size>1024**2:error('manifest exceeds 1 MB')
    m=validate_manifest(json.loads(manifest.read_text(encoding='utf-8')))
    actual={p.relative_to(root).as_posix():p for p in regular_tree(root) if p!=manifest}
    if set(actual)!={r['file'] for r in m['files']}:error('every file must be listed, with no undeclared extras')
    for record in m['files']:
        p=actual[record['file']]
        if p.stat().st_size!=record['bytes'] or sha(p)!=record['sha256']:error('file checksum/size mismatch: '+record['file'])
    if (root/MARKER).read_bytes()!=MARKER_CONTENT:error('invalid old-loader marker')
    patch_paths=set(p for _,flat in expanded_variants(m) for p in flat.get('patches',[]))
    for name in patch_paths:
        data=under(root,name).read_bytes()
        if len(data)>8*1024**2 or b'<!DOCTYPE' in data.upper() or b'<!ENTITY' in data.upper():error('unsupported XML declarations/size')
        xml=ET.fromstring(data);ids=[n.text for n in xml.findall('./TitleID/ID')]
        if ids not in (['CUSA13795'],['CUSA14876']) or not list(xml.iter('Metadata')):
            error('XML must target exactly one supported CTR edition (CUSA13795 or CUSA14876) and contain Metadata')
        for meta in xml.iter('Metadata'):
            if meta.get('AppVer')!='01.21' or meta.get('AppElf')!='eboot.bin' or not list(meta.iter('Line')):
                error('XML metadata must target eboot.bin 01.21 and contain writes')
            for line in meta.iter('Line'):
                if line.get('ExpectedSHA256') is not None:hash_string(line.get('ExpectedSHA256'))
                if 'code-signatures' in m.get('requiresCapabilities',[]) and not (line.get('Signature') or line.get('ExpectedSHA256')):
                    error('code-signatures requires a Signature or ExpectedSHA256 on every write')
    return m

def inspect_zip(package):
    """Read a bounded manifest before deciding legacy vs format 2. No extraction."""
    with zipfile.ZipFile(package) as z:
        candidates=[i for i in z.infolist() if Path(i.filename).name=='mod.json']
        if not candidates:return None
        if len(candidates)!=1:error('ZIP must have one mod.json')
        item=candidates[0];safe_path(item.filename)
        if item.file_size>1024**2:error('manifest exceeds 1 MB')
        m=json.loads(z.read(item))
        if 'schemaVersion' not in m:return None
        validate_manifest(m)
        return m,item.filename

def unpack_v2(package,destination,manifest_path):
    prefix=Path(manifest_path).parent.as_posix();prefix='' if prefix=='.' else prefix+'/'
    root=under(destination,prefix[:-1]) if prefix else Path(destination)
    with zipfile.ZipFile(package) as z:
        seen=set();total=0
        if len(z.infolist())>20000:error('too many ZIP members')
        for item in z.infolist():
            safe_path(item.filename.rstrip('/'))
            if prefix and not item.filename.startswith(prefix):error('files outside package wrapper')
            key=item.filename.rstrip('/').casefold()
            if key in seen:error('duplicate ZIP path')
            seen.add(key);total+=item.file_size
            mode=item.external_attr>>16
            if stat.S_IFMT(mode) not in (0,stat.S_IFREG,stat.S_IFDIR) or item.flag_bits&1:error('linked/encrypted/special ZIP member')
        if total>12*1024**3:error('expanded ZIP exceeds 12 GB')
        import shutil
        if shutil.disk_usage(destination).free<total+128*1024**2:error('insufficient extraction space')
        for item in z.infolist():
            if item.is_dir():continue
            target=under(destination,item.filename);target.parent.mkdir(parents=True,exist_ok=True)
            with z.open(item) as src,target.open('xb') as dst:shutil.copyfileobj(src,dst)
    validate_folder(root);return root
