"""Declarative mod choices. No mod-supplied scripts are evaluated.
SPDX-License-Identifier: GPL-2.0-or-later
"""
import itertools,json,re,sys
from pathlib import Path
from mod_components import save_json

def token(value):
    return isinstance(value,str) and re.fullmatch(r'[a-z0-9][a-z0-9-]{0,79}',value) is not None
def text(value,limit):
    return isinstance(value,str) and 0<len(value)<=limit and not any(ord(c)<32 for c in value)
def validate(declarations):
    if not isinstance(declarations,dict) or not 1<=len(declarations)<=16:raise ValueError('configuration needs 1–16 options')
    combinations=1;cells=set()
    for key,r in declarations.items():
        if not token(key) or not isinstance(r,dict) or set(r)-{'type','label','description','default','choices','advanced','row','column','patches','useOperations'}:
            raise ValueError('Invalid mod option declaration')
        if 'advanced' in r and type(r['advanced']) is not bool:raise ValueError('advanced must be a boolean')
        if 'row' in r or 'column' in r:
            if r.get('type')!='boolean' or not text(r.get('row'),80) or not text(r.get('column'),80):raise ValueError('Grid cells need boolean row and column labels')
            if r.get('advanced',False):raise ValueError('Grid cells cannot also be advanced controls')
            cell=(r['row'],r['column'])
            if cell in cells:raise ValueError('Duplicate grid cell')
            cells.add(cell)
        if 'patches' in r:
            from mod_components import safe_path
            choices=['false','true'] if r.get('type')=='boolean' else list(r.get('choices',{}))
            if not isinstance(r['patches'],dict) or set(r['patches'])!=set(choices):raise ValueError('Option patches must cover every value')
            for paths in r['patches'].values():
                if not isinstance(paths,list) or len(paths)>16 or any(not isinstance(p,str) for p in paths):raise ValueError('Invalid option patch list')
                for path in paths:
                    safe_path(path)
                    if not path.endswith('.xml'):raise ValueError('Option patches must be XML')
        if 'useOperations' in r:
            choices=['false','true'] if r.get('type')=='boolean' else list(r.get('choices',{}))
            selections=r['useOperations']
            if not isinstance(selections,dict) or set(selections)!=set(choices) or any(not isinstance(names,list) or len(names)>16 or any(not token(name) for name in names) or len(set(names))!=len(names) for names in selections.values()):raise ValueError('Invalid option operation sets')
        if not text(r.get('label'),160) or ('description' in r and not text(r['description'],1000)):
            raise ValueError('Invalid mod option label/description')
        if r.get('type')=='boolean':
            if type(r.get('default')) is not bool or 'choices' in r:raise ValueError('Invalid boolean option')
        elif r.get('type')=='choice':
            choices=r.get('choices')
            if not isinstance(choices,dict) or not 2<=len(choices)<=32 or any(not token(v) or not text(label,160) for v,label in choices.items()):
                raise ValueError('Choice options need 2–32 named choices')
            if not isinstance(r.get('default'),str) or r['default'] not in choices:raise ValueError('Option default must be a choice')
        else:raise ValueError('Option type must be choice or boolean')
        combinations*=len(domain(r))
    if combinations>2048:raise ValueError('At most 2048 mod option combinations are supported')

def patch_paths(manifest,values=None):
    paths=[]
    for key,rule in manifest.get('configuration',{}).items():
        selections=rule.get('patches',{})
        if values is None:paths.extend(p for entries in selections.values() for p in entries)
        else:
            value=values[key]
            token=('true' if value else 'false') if type(value) is bool else value
            paths.extend(selections.get(token,[]))
    return paths

def domain(rule):return [False,True] if rule['type']=='boolean' else list(rule['choices'])
def valid(rule,value):return type(value) is bool if rule['type']=='boolean' else isinstance(value,str) and value in rule['choices']
def effective(manifest,saved=None):
    declarations=manifest.get('configuration',{})
    if declarations:validate(declarations)
    saved=saved if isinstance(saved,dict) else {}
    return {k:saved[k] if k in saved and valid(r,saved[k]) else r['default'] for k,r in declarations.items()}
def combinations(manifest):
    rules=manifest.get('configuration',{})
    return [dict(zip(rules,values)) for values in itertools.product(*(domain(r) for r in rules.values()))]
def load(path):
    path=Path(path)
    if not path.exists():return {}
    try:
        data=json.loads(path.read_text(encoding='utf-8'))
        if not isinstance(data,dict) or data.get('version')!=1 or not isinstance(data.get('mods'),dict):
            raise ValueError('unexpected contents')
    except (OSError,ValueError) as error:
        # Saved choices must not stop the mods page or the game: the defaults apply until the next Save.
        print('Mod options not read, using defaults ('+path.name+': '+str(error)+')',file=sys.stderr)
        return {}
    return data['mods']
def save(path,ident,manifest,values):
    rules=manifest.get('configuration',{})
    if not isinstance(values,dict) or set(values)!=set(rules) or any(not valid(rules[k],v) for k,v in values.items()):
        raise ValueError('Invalid mod option values')
    mods=load(path);mods[ident]=values
    save_json(Path(path),{'version':1,'mods':mods})
