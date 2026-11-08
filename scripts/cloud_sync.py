# SPDX-License-Identifier: GPL-2.0-or-later
"""The player's trophies and save on the RyuNET server, so they follow the account to every PC
(the server's trophies.go and cloud_saves.go).

Trophies: <progress>/trophies/<TITLE>/unlocked.txt, one "<id> <unix time>" line each (the runtime's
src/runtime_services.c). Before the game starts and after it ends the file goes to the server,
which keeps the union of every PC's and answers it; that answer is written back, so a trophy
earned on another PC is there before the game reads the file.

Save: <progress>/savedata/1/<TITLE>/savedata0 and savedata1 (with their .sce_sys), zipped and
uploaded after the game ends, only when it changed since the last upload. Replays stay local.
Bringing a save down is the player's choice (the launcher's button), never automatic.

Every request is signed by run.py (call); nothing here stops a launch: a failure is one line.
"""
import hashlib
import io
import os
import zipfile
from pathlib import Path

SAVE_FOLDERS = ('savedata0', 'savedata1', 'savedata0.sce_sys', 'savedata1.sce_sys')
SAVE_MAX = 24 << 20


def trophy_file(progress, title):
    return Path(progress) / 'trophies' / title / 'unlocked.txt'


def parse_unlocks(text):
    """id -> unix time; the earliest of repeated ids (the runtime only appends)."""
    out = {}
    for line in (text or '').splitlines():
        parts = line.split()
        if len(parts) < 2 or not parts[0].isdigit() or not parts[1].isdigit():
            continue
        tid, at = int(parts[0]), int(parts[1])
        out[tid] = min(at, out.get(tid, at))
    return out


def sync_trophies(call, name, progress, title, say=print):
    """Sends this PC's unlocks and writes back the account's (only adding: a local unlock the
    server refused, such as one of OCTR's own, stays). The number of trophies on the account."""
    path = trophy_file(progress, title)
    try:
        local = path.read_text(encoding='utf-8', errors='replace') if path.exists() else ''
    except OSError:
        local = ''
    from urllib.parse import urlencode
    answer = call('POST', '/ryunet/trophies/sync?' + urlencode({'name': name, 'title': title}), local.encode('utf-8'))
    merged = parse_unlocks(answer.get('unlocked', '')) if isinstance(answer, dict) else {}
    mine = parse_unlocks(local)
    new = {tid: at for tid, at in merged.items() if tid not in mine}
    if new:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, 'a', encoding='utf-8') as f:
            for tid in sorted(new):
                f.write(f'{tid} {new[tid]}\n')
        say(f'Trophies: {len(new)} earned on another PC brought here ({title})')
    return len(merged)


def save_dir(progress, title):
    return Path(progress) / 'savedata' / '1' / title


def zip_save(progress, title):
    """The save as the server keeps it (a zip of the save folders, in a fixed order with fixed
    dates, so the same save gives the same bytes), or None when there is none."""
    base = save_dir(progress, title)
    files = []
    for top in SAVE_FOLDERS:
        root = base / top
        if root.is_dir():
            for dirpath, _, names in os.walk(root):
                for n in names:
                    full = Path(dirpath) / n
                    files.append((full.relative_to(base).as_posix(), full))
    if not any(rel.startswith(('savedata0/', 'savedata1/')) for rel, _ in files):
        return None
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for rel, full in sorted(files):
            info = zipfile.ZipInfo(rel, date_time=(2019, 6, 21, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(info, full.read_bytes())
    data = buf.getvalue()
    return data if len(data) <= SAVE_MAX else None


def upload_save(call, name, progress, title, say=print):
    """Uploads the save when it changed since this PC's last upload. The server's version id, or
    None when nothing was sent."""
    data = zip_save(progress, title)
    if data is None:
        return None
    digest = hashlib.sha256(data).hexdigest()
    marker = Path(progress) / 'cloud' / f'{title}.uploaded'
    try:
        if marker.read_text(encoding='ascii').strip() == digest:
            return None
    except OSError:
        pass
    from urllib.parse import urlencode
    answer = call('POST', '/ryunet/saves/upload?' + urlencode({'name': name, 'title': title}), data, timeout=60)
    if not isinstance(answer, dict) or 'id' not in answer:
        say(f'Cloud save: not kept ({answer.get("error") if isinstance(answer, dict) else "no answer"})')
        return None
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(digest + '\n', encoding='ascii')
    say(f'Cloud save: uploaded ({len(data) // 1024} KB, {title})')
    return answer['id']


def restore_save(data, progress, title):
    """Writes a downloaded save over this PC's, after keeping the current one in
    <progress>/cloud/backup-<title>.zip. Only the save folders inside the zip are written."""
    base = save_dir(progress, title)
    current = zip_save(progress, title)
    if current:
        backup = Path(progress) / 'cloud' / f'backup-{title}.zip'
        backup.parent.mkdir(parents=True, exist_ok=True)
        backup.write_bytes(current)
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        names = zf.namelist()
        for n in names:
            parts = Path(n).parts
            if not parts or parts[0] not in SAVE_FOLDERS or '..' in parts or Path(n).is_absolute() or ':' in n or '\\' in n:
                raise ValueError(f'unexpected path in the save: {n}')
        for top in SAVE_FOLDERS:
            if any(Path(n).parts[0] == top for n in names) and (base / top).exists():
                import shutil
                shutil.rmtree(base / top)
        for n in names:
            if n.endswith('/'):
                continue
            target = base / n
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(zf.read(n))
    marker = Path(progress) / 'cloud' / f'{title}.uploaded'
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(hashlib.sha256(zip_save(progress, title) or b'').hexdigest() + '\n', encoding='ascii')
