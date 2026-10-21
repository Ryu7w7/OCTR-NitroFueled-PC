"""Prepare explicit offline AppContent metadata; SKU is a probe setting, not a license check.

Add-ons (DLC): the player's own add-on packages for this game in --dlc-dir, as .pkg files (only
their header and param.sfo are read; CTR Nitro-Fueled's DLC carry no extra data, the content is in
the game) or as extracted folders (sce_sys/param.sfo). Each one found is reported installed to the
game, by its entitlement label (the end of its CONTENT_ID). Optional: without the folder the game
runs as before. BB_DLC=0 turns them all off; BB_DLC_DISABLED=label,label turns some off.
"""
import argparse
import json
import os
import struct
from pathlib import Path
from prepare import sfo

PKG_MAGIC = b'\x7fCNT'
MAX_ADDONS = 64


def pkg_param(path):
    """param.sfo of a PS4 package (entry 0x1000), when it is stored in the clear."""
    with open(path, 'rb') as f:
        header = f.read(0x1000)
        if header[:4] != PKG_MAGIC:
            raise ValueError('not a PS4 package')
        count, = struct.unpack_from('>I', header, 0x10)
        table, = struct.unpack_from('>I', header, 0x18)
        f.seek(table)
        entries = f.read(count * 32)
        for i in range(count):
            eid, _name, flags1, _flags2, offset, size = struct.unpack_from('>IIIIII', entries, i * 32)
            if eid == 0x1000:
                if flags1 & 0x80000000:
                    raise ValueError('param.sfo is encrypted')
                f.seek(offset)
                return sfo(f.read(size))
    raise ValueError('no param.sfo')


def is_candidate(path):
    """A .pkg file or an extracted folder: what the dlc folder is scanned for."""
    path = Path(path)
    return (path.is_file() and path.suffix.lower() == '.pkg') or path.is_dir()


def describe_addon(path, title_id):
    """(label, title, problem) of one package or extracted folder; problem is '' for an add-on of
    title_id. The launchers list the dlc folder with it."""
    path = Path(path)
    try:
        if path.is_file():
            info = pkg_param(path)
        elif (path / 'sce_sys' / 'param.sfo').is_file():
            info = sfo((path / 'sce_sys' / 'param.sfo').read_bytes())
        else:
            return '', path.name, 'the folder has no sce_sys/param.sfo'
    except (OSError, ValueError, struct.error, IndexError) as error:
        return '', path.name, str(error)
    content = info.get('CONTENT_ID', '')
    label = content.rsplit('-', 1)[-1] if '-' in content else ''
    if info.get('TITLE_ID') != title_id or info.get('CATEGORY') != 'ac' or len(label) != 16:
        return label, info.get('TITLE', path.name), f'not an add-on of {title_id} ({content or "no CONTENT_ID"})'
    return label, info.get('TITLE', label) or label, ''


def find_addons(dlc_dir, title_id):
    """[(label, title, source)] of this game's add-ons in dlc_dir, by label (first one wins)."""
    found = {}
    if not dlc_dir or not Path(dlc_dir).is_dir():
        return []
    for path in sorted(Path(dlc_dir).iterdir()):
        if not is_candidate(path):
            continue
        label, title, problem = describe_addon(path, title_id)
        if problem:
            if path.is_dir() and problem == 'the folder has no sce_sys/param.sfo':
                continue  # any other folder, as before
            print(f'DLC: skipped {path.name}: {problem}')
            continue
        found.setdefault(label, (label, title, path.name))
    return list(found.values())[:MAX_ADDONS]


def prepare(game, out, sku='full', dlc_dir=None):
    values=sfo((game/'sce_sys/param.sfo').read_bytes())
    params=[values.get(f'USER_DEFINED_PARAM_{i}',0) for i in range(1,5)]
    if any(type(v) is not int or not 0<=v<=0xffffffff for v in params):
        raise ValueError('invalid user-defined parameter')
    addons = [] if os.environ.get('BB_DLC') == '0' else find_addons(dlc_dir, values.get('TITLE_ID'))
    disabled = {x.strip() for x in os.environ.get('BB_DLC_DISABLED', '').split(',') if x.strip()}
    addons = [a for a in addons if a[0] not in disabled]
    profile=dict(title_id=values.get('TITLE_ID'),sku=sku,sku_source='explicit probe setting',
                 user_params=params, mounted_addons=[dict(label=l, title=t, source=s) for l, t, s in addons],
                 boot_attr=0)
    out.mkdir(parents=True,exist_ok=True)
    blob = struct.pack('<8s6I', b'BBCONT02', {'full':3,'trial':1}[sku], *params, len(addons))
    for label, _title, _source in addons:
        blob += label.encode('ascii').ljust(20, b'\0')
    (out/'content.bin').write_bytes(blob)
    (out/'content-profile.json').write_text(json.dumps(profile,indent=2,ensure_ascii=False)+'\n', encoding='utf-8')
    for label, title, source in addons:
        print(f'DLC: {title} ({label}, {source})')
    print(f'AppContent profile: SKU={sku} (probe setting), user params={params}, add-ons={len(addons)}')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game',type=Path)
    p.add_argument('--out',type=Path,default=Path(__file__).resolve().parent.parent/'out')
    p.add_argument('--sku',choices=['full','trial'],default='full')
    p.add_argument('--dlc-dir',type=Path,help="folder with the game's DLC packages (.pkg) or extracted DLC folders")
    a=p.parse_args();prepare(a.game,a.out,a.sku,a.dlc_dir)
