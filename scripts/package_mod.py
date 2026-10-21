"""Build/validate OCTR mod format 2 packages. Author: tcosi. GPL-2.0-or-later."""
from pathlib import Path
import argparse,json,zipfile,sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
import mod_format as fmt
from mod_components import regular_tree,sha,save_json

def build(folder,output):
    folder=Path(folder).resolve();output=Path(output).resolve()
    if output.is_relative_to(folder):raise ValueError('Output ZIP must be outside the package folder')
    if output.exists():raise ValueError('Output already exists; use a new release filename')
    manifest=json.loads((folder/'mod.json').read_text(encoding='utf-8'))
    (folder/fmt.MARKER).write_bytes(fmt.MARKER_CONTENT)
    manifest['files']=[{'file':p.relative_to(folder).as_posix(),'sha256':sha(p),'bytes':p.stat().st_size}
        for p in sorted(regular_tree(folder)) if p!=folder/'mod.json']
    save_json(folder/'mod.json',manifest);fmt.validate_folder(folder)
    output.parent.mkdir(parents=True,exist_ok=True)
    try:
        with zipfile.ZipFile(output,'x',zipfile.ZIP_DEFLATED,allowZip64=True) as z:
            for p in regular_tree(folder):z.write(p,p.relative_to(folder).as_posix())
    except BaseException:
        # Keep an incomplete output for diagnosis, never overwrite a prior release.
        raise
    return {'file':output.name,'sha256':sha(output),'bytes':output.stat().st_size,'id':manifest['id'],'version':manifest['version']}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('folder',type=Path)
    p.add_argument('--output',type=Path);p.add_argument('--validate',action='store_true');a=p.parse_args()
    if a.validate:fmt.validate_folder(a.folder);print('Valid OCTR mod format 2 package')
    elif a.output:print(json.dumps(build(a.folder,a.output),indent=2))
    else:p.error('Supply --output ZIP or --validate')
