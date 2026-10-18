"""Manifest-declared session defaults, selected before static variants.
Author: tcosi. SPDX-License-Identifier: GPL-2.0-or-later.
"""
import re

KEYS={'output_res','render_res'}
def validate(declarations):
    if not isinstance(declarations,dict) or not declarations:raise ValueError('variantSettings must be a nonempty object')
    for key,rule in declarations.items():
        if key not in KEYS or not isinstance(rule,dict) or set(rule)!={'default','allowed'}:
            raise ValueError('variantSettings permits output_res/render_res with default and allowed')
        allowed=rule['allowed']
        if not isinstance(allowed,list) or not allowed or not all(isinstance(s,str) for s in allowed) or len(set(allowed))!=len(allowed):
            raise ValueError('variantSettings allowed must be distinct strings')
        for value in allowed:
            if key=='output_res':
                if not re.fullmatch(r'[1-9]\d{1,4}x[1-9]\d{1,4}',value):raise ValueError('Invalid variant output resolution')
            elif value not in {'480','720','1080','1440','1620','2160'}:raise ValueError('Unsupported internal resolution default')
        if rule['default'] not in allowed:raise ValueError('variantSettings default must be allowed')

def choose(manifests,base):
    """Preserve a compatible preference; otherwise require one agreed default."""
    grouped={};result=dict(base);changes={}
    for manifest in manifests:
        if 'variantSettings' not in manifest:continue
        declarations=manifest['variantSettings'];validate(declarations)
        for key,rule in declarations.items():grouped.setdefault(key,[]).append(rule)
    for key,rules in grouped.items():
        allowed=set.intersection(*(set(rule['allowed']) for rule in rules))
        if not allowed:raise ValueError('Enabled mods require incompatible '+key+' values')
        if base.get(key) in allowed:continue
        defaults={rule['default'] for rule in rules if rule['default'] in allowed}
        if len(defaults)!=1:raise ValueError('Enabled mods disagree on a default '+key)
        value=defaults.pop();result[key]=value;changes[key]={'saved':base.get(key),'session':value}
    return result,changes
