"""Print compatibility hashes for an author's own test installation."""
from pathlib import Path
import argparse,json,sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from nitro_components import sha

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--runtime',type=Path,required=True)
    p.add_argument('--game',type=Path,required=True);a=p.parse_args()
    print(json.dumps({'runtime_versions':[(a.runtime/'bin/VERSION').read_text().strip()],
        'runtime_sha256':[sha(a.runtime/'bin/sacredfire.exe')],
        'eboot_sha256':[sha(a.game/'eboot.bin')],'online':'offline-only'},indent=2))
