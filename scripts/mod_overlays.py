"""Read-only mod archive overlays; never materialize copies of game archives.
SPDX-License-Identifier: GPL-2.0-or-later
"""
from pathlib import Path
import json, struct
import archive_patch
from archive_reader import Archive
from mod_components import payload_bytes, under, sha_bytes, plain_path


def file_map(game, target, directories, replacements):
    """Map relative guest files directly to validated mod files, with no game view."""
    records=[];seen=[]
    directories=Path(directories);directories.mkdir(parents=True,exist_ok=True)
    for relative,source in replacements:
        relative=relative.as_posix();under(game,relative)
        if under(game,relative).is_dir():raise ValueError('File/directory conflict: '+relative)
        for parent in Path(relative).parents:
            if parent==Path('.'):continue
            if under(game,parent.as_posix()).is_file():raise ValueError('File/directory conflict: '+relative)
        key=relative.casefold()
        if any(key==p or key.startswith(p+'/') or p.startswith(key+'/') for p in seen):
            raise ValueError('Conflicting file redirections: '+relative)
        seen.append(key)
        # The engine opens these with the usual Windows paths, not the library's long_path form.
        source=plain_path(Path(source).resolve(strict=True))
        if source.is_relative_to(plain_path(Path(game).resolve())):raise ValueError('Replacement must come from mod storage')
        if not source.is_file():raise ValueError('Mod replacement is not a file: '+relative)
        a=relative.encode('utf-8');b=str(source).encode('utf-8')
        if not 0<len(a)<1024 or not 0<len(b)<4096 or b'\0' in a+b:
            raise ValueError('Mod file path exceeds engine limits')
        under(directories,relative).parent.mkdir(parents=True,exist_ok=True)
        records.append(struct.pack('<II',len(a),len(b))+a+b)
    if len(records)>65536:raise ValueError('Too many mod file redirections')
    Path(target).write_bytes(b'BBFILE01'+struct.pack('<I',len(records))+b''.join(records))


def compose(game, out, records, base_patches):
    """Append validated mod entries to built-in patches, one patch per guest archive.

    A collision with a built-in entry is an error, never a silently dropped layer.
    Runtime bounds are checked here because its parser would otherwise ignore them.
    """
    made = dict(base_patches)
    groups = {}
    for op, folder in records:
        if op['kind'] == 'archive_asset':
            groups.setdefault(op['archive'], []).append((op, Path(folder)))
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    for name, items in groups.items():
        path = under(game, 'archives/' + name)
        archive = Archive(path)
        entries = {}
        try:
            for op, folder in items:
                entry = archive.files[op['index']]
                if entry['path'] != op['asset']:
                    raise ValueError('Archive asset/index mismatch: ' + op['asset'])
                if op['index'] in entries:
                    raise ValueError('Duplicate mod archive entry: ' + op['asset'])
                entries[op['index']] = payload_bytes(op, folder, archive.extract(entry))
        finally:
            archive.data.close(); archive._file.close()
        guest = '/app0/archives/' + name
        target = out / (sha_bytes(guest.encode()) + '.bbpatch')
        archive_patch.write_entries_patch(target, path, guest, entries, made.get(guest))
        _, _, ranges = archive_patch.read(target)
        if len(ranges) > 4096 or target.stat().st_size >= 64 * 1024**2:
            raise ValueError('Mod archive overlay exceeds engine limits: ' + name)
        made[guest] = target
    if len(made) > 32:
        raise ValueError('Too many archive overlays for the engine')
    return made


def for_launch(game, out, plan_path, base_patches):
    records = json.loads(Path(plan_path).read_text(encoding='utf-8'))
    return compose(game, out, [(r['operation'], r['folder']) for r in records], base_patches)
