# SPDX-License-Identifier: GPL-2.0-or-later
"""External content: tracks (later racers and karts) added to the game, never replacing it.

The custom_content folder beside the launcher holds content packages, ZIPs (or folders, for
creators) with this layout:

    octr-content.required        "OCTR Content Package 1"
    content.json                 what the package adds (below)
    archives/<id>*.pak           the track's own archives: names start with its id, and no game
                                 archive has the name
    files/<logical path>         its own game files (zone info, menu picture texture, materials):
                                 paths the game does not have; they go in the generated update
                                 archive and stream with the track
    shared/<logical path>        unchanged copies of game files the content needs loaded with it
                                 (Neo Bowser City: Sewer Speedway's scripts and menu textures,
                                 which the game keeps in archives it loads only for that track);
                                 they must be the game's bytes, which the launcher checks; they
                                 go in the update archive, and in the track's streaming list only
                                 when "stream" names them
    README*, preview.png ...     text and pictures at the top, for people

content.json (format 1):

    {"format": 1, "type": "track", "id": "t922_suzuka",
     "name": {"en": "Suzuka"}, "version": "0.7.2", "author": "CTRNFModding",
     "map": "octane/t922_suzuka/t922_suzuka",
     "image": "LevelImage_Suzuka",                        optional: the picture material its zone
     "texture": "GuiStandard_diffuse,textures@...",        info names in LevelSelect_materials, and
                                                          that picture's texture (in files/)
     "online": "offline-only",                            or "private": private rooms
     "credits": "...", "convertedFrom": {...},            optional, shown to people
     "stream": ["textures/...igz"],                       optional: shared/ files streamed with
                                                          the track (as the converted mod had them)
     "files": [{"file": "archives/t922_suzuka.pak", "sha256": "...", "bytes": 560351667}, ...]}

A package cannot replace a game file, carry executable patches or change the registration files
itself: the launcher registers its track (scripts/track_registry.py) in an update archive it
builds (scripts/content_build.py). Choices are kept in content-state/selection.json:
{"enabled": true, "disabled": [package ids], "page": "own"}; "enabled" turns all external content
off at once; "page" puts the tracks on a page of their own after the game's ("own") or at the end
of its last page, Bonus Tracks ("bonus").
"""
import hashlib
import json
import re
import shutil
import stat
import struct
import zipfile
from pathlib import Path

from mod_components import long_path, safe_path, save_json, sha, under

FOLDER = 'custom_content'
STATE = 'content-state'
MARKER = 'octr-content.required'
MARKER_TEXT = 'OCTR Content Package 1'
MANIFEST = 'content.json'
FORMAT = 1
TYPES = ('track',)
ONLINE = ('offline-only', 'private')
PAGES = ('own', 'bonus')
ID = re.compile(r'[a-z][a-z0-9_]{2,63}\Z')
IMAGE = re.compile(r'LevelImage_[A-Za-z0-9_]{1,64}\Z')
TEXTURE = re.compile(r'[^/\\:\0]{1,200}\Z')
EXTRAS = ('.txt', '.md', '.png', '.jpg', '.jpeg')  # at the top of a package only
MAX_EXPANDED = 12 * 1024 ** 3
FIELDS = {'format', 'type', 'id', 'name', 'version', 'author', 'map', 'image', 'texture', 'online',
          'credits', 'convertedFrom', 'files', 'stream'}
GAME_INDEX_VERSION = 1


def state(root):
    """content-state beside custom_content (the portable root's)."""
    return long_path(Path(root).parent / STATE)


# --- the manifest ------------------------------------------------------------------------------

def text(value, what, limit=200):
    if not isinstance(value, str) or not value.strip() or len(value) > limit or any(ord(c) < 32 for c in value):
        raise ValueError(f'{what}: expected a single line of text (at most {limit} characters)')
    return value


def validate_manifest(m):
    """The manifest's fields, checked; raises ValueError naming the first problem."""
    if not isinstance(m, dict):
        raise ValueError('content.json: expected an object')
    unknown = set(m) - FIELDS
    if unknown:
        raise ValueError('content.json: unknown fields ' + ', '.join(sorted(unknown)))
    if m.get('format') != FORMAT:
        raise ValueError(f'content.json: format must be {FORMAT}')
    if m.get('type') not in TYPES:
        raise ValueError('content.json: type must be one of ' + ', '.join(TYPES))
    if not isinstance(m.get('id'), str) or not ID.match(m['id']):
        raise ValueError('content.json: id must be lower-case letters, digits and _ (like t922_suzuka)')
    ident = m['id']
    names = m.get('name')
    if not isinstance(names, dict) or 'en' not in names:
        raise ValueError('content.json: name must hold at least an "en" name')
    for lang, value in names.items():
        if not re.match(r'[a-z]{2}\Z', str(lang)):
            raise ValueError('content.json: name languages are two-letter codes')
        text(value, 'content.json: name.' + lang, 80)
    text(m.get('version'), 'content.json: version', 40)
    text(m.get('author'), 'content.json: author', 120)
    if m.get('map', '').casefold() != f'octane/{ident}/{ident}':
        raise ValueError(f'content.json: map must be octane/{ident}/{ident}')
    if ('image' in m) != ('texture' in m):
        raise ValueError('content.json: image and texture go together')
    if 'image' in m and (not isinstance(m['image'], str) or not IMAGE.match(m['image'])):
        raise ValueError('content.json: image must be a LevelImage_<name> material name')
    if 'texture' in m and (not isinstance(m['texture'], str) or not TEXTURE.match(m['texture'])):
        raise ValueError('content.json: texture must be a texture asset name')
    if m.get('online', 'offline-only') not in ONLINE:
        raise ValueError('content.json: online must be one of ' + ', '.join(ONLINE))
    if 'credits' in m:
        text(m['credits'], 'content.json: credits', 1000)
    if 'convertedFrom' in m and not isinstance(m['convertedFrom'], dict):
        raise ValueError('content.json: convertedFrom must be an object')
    files = m.get('files')
    if not isinstance(files, list) or not files:
        raise ValueError('content.json: files must list every file of the package')
    seen = set()
    for record in files:
        if not isinstance(record, dict) or set(record) != {'file', 'sha256', 'bytes'}:
            raise ValueError('content.json: each file needs file, sha256 and bytes')
        path = record['file']
        safe_path(path)
        if not re.match(r'[0-9a-f]{64}\Z', str(record['sha256'])) or not isinstance(record['bytes'], int) \
                or record['bytes'] < 0:
            raise ValueError('content.json: bad sha256/bytes for ' + str(path))
        key = path.casefold()
        if key in seen:
            raise ValueError('content.json: listed twice: ' + path)
        seen.add(key)
        kind_of(path, ident)
    if not any(kind_of(r['file'], ident) == 'archive' for r in files):
        raise ValueError('content.json: a track needs its own archive (archives/' + ident + '.pak)')
    if 'texture' in m and 'files/' + texture_file(m['texture']).casefold() not in seen:
        raise ValueError('content.json: the picture texture must be in files/')
    stream = m.get('stream', [])
    if not isinstance(stream, list) or not all(isinstance(p, str) and 'shared/' + p.casefold() in seen for p in stream):
        raise ValueError('content.json: stream lists files of shared/ (their logical paths)')
    return m


def texture_file(texture):
    return 'textures/' + texture + '.igz'


def kind_of(path, ident):
    """What a package file is: 'archive', 'file' (a game file it adds) or 'extra' (for people)."""
    parts = path.split('/')
    if parts[0] == 'archives':
        name = parts[-1].casefold()
        if len(parts) != 2 or not name.endswith('.pak') or not name.startswith(ident):
            raise ValueError(f'{path}: archives are archives/{ident}*.pak')
        if re.match(r'update\d*\.pak\Z', name):
            raise ValueError(f'{path}: content packages cannot ship update archives')
        return 'archive'
    if parts[0] in ('files', 'shared') and len(parts) > 1:
        if path.casefold().endswith(('.xml', '.pak', '.elf', '.prx', '.bin', '.dll', '.exe')):
            raise ValueError(f'{path}: not a game asset')
        return 'file' if parts[0] == 'files' else 'shared'
    if len(parts) == 1 and (path.casefold().endswith(EXTRAS) or path.casefold().startswith('readme')):
        return 'extra'
    raise ValueError(f'{path}: content packages hold archives/, files/, shared/ and text or pictures at the top')


def stream_kind(logical):
    """The streaming entry kind the game's packages give a file of this path."""
    p = logical.casefold()
    if p.startswith('textures/'):
        return 'texture'
    if p.startswith('materialinstances/'):
        return 'material_instances'
    if p.startswith('sound_samples/'):
        return 'sound_sample'
    if p.startswith('sounds/banks/'):
        return 'sound_bank'
    if p.startswith('vsc/'):
        return 'vsc'
    if p.endswith('.lng'):
        return 'lang_file'
    return 'igx_file'


# --- reading packages --------------------------------------------------------------------------

def read_folder(folder, verify=True):
    """The manifest of an unpacked package, checked against its files."""
    folder = Path(folder)
    marker = folder / MARKER
    if not marker.is_file() or marker.read_text(encoding='utf-8', errors='replace').strip() != MARKER_TEXT:
        raise ValueError(f'missing {MARKER} ("{MARKER_TEXT}")')
    try:
        m = json.loads((folder / MANIFEST).read_text(encoding='utf-8'))
    except FileNotFoundError:
        raise ValueError('missing content.json') from None
    except json.JSONDecodeError as error:
        raise ValueError(f'content.json: {error}') from None
    validate_manifest(m)
    listed = {r['file'].casefold(): r for r in m['files']}
    present = {}
    for path in folder.rglob('*'):
        if path.is_symlink():
            raise ValueError('links are not supported: ' + path.name)
        if path.is_file():
            rel = path.relative_to(folder).as_posix()
            if rel in (MARKER, MANIFEST):
                continue
            present[rel.casefold()] = path
    missing = sorted(set(listed) - set(present))
    extra = sorted(set(present) - set(listed))
    if missing or extra:
        raise ValueError('files differ from content.json: ' +
                         ', '.join(['missing ' + p for p in missing] + ['not listed ' + p for p in extra]))
    for key, record in listed.items():
        path = present[key]
        if path.stat().st_size != record['bytes'] or (verify and sha(path) != record['sha256']):
            raise ValueError('checksum mismatch: ' + record['file'])
    return m


def unpack(package, dest):
    """A content ZIP unpacked into dest (one wrapper folder is accepted). Returns its root."""
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(package) as z:
        seen, total = set(), 0
        for info in z.infolist():
            name = info.filename.rstrip('/')
            if not name:
                continue
            safe_path(name)
            if name.casefold() in seen:
                raise ValueError('duplicate ZIP path ' + name)
            seen.add(name.casefold())
            mode = info.external_attr >> 16
            if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) and not (stat.S_ISREG(mode) or stat.S_ISDIR(mode))):
                raise ValueError('ZIP links and special files are not supported')
            if info.flag_bits & 1:
                raise ValueError('encrypted ZIPs are not supported')
            total += info.file_size
            if total > MAX_EXPANDED:
                raise ValueError('ZIP exceeds the expanded size limit')
            if info.is_dir():
                continue
            target = under(dest, name)
            target.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, target.open('wb') as out:
                shutil.copyfileobj(src, out)
    root = dest
    while not (root / MANIFEST).is_file():
        dirs = [p for p in root.iterdir() if p.is_dir()]
        if len(dirs) != 1 or any(p.is_file() for p in root.iterdir()):
            raise ValueError('missing content.json at the top of the ZIP')
        root = dirs[0]
    return root


def scan(root, log=print):
    """The packages of custom_content: ZIPs are unpacked once (content-state/packages/<sha>),
    folders read in place. Returns {'packages': {id: record}, 'messages': [...]} and keeps it in
    content-state/library.json."""
    root = Path(root)
    store = state(root)
    library = store / 'library.json'
    previous = json.loads(library.read_text(encoding='utf-8')) if library.is_file() else {}
    known = previous.get('sources', {})
    packages, messages, sources = {}, [], {}
    if root.is_dir():
        for source in sorted(root.iterdir(), key=lambda p: p.name.casefold()):
            try:
                if source.is_symlink():
                    raise ValueError('links are not supported')
                if source.is_file() and source.suffix.casefold() == '.zip':
                    st = source.stat()
                    old = known.get(source.name)
                    if old and old['bytes'] == st.st_size and old['mtime_ns'] == st.st_mtime_ns \
                            and Path(old['folder']).is_dir():
                        digest, folder, verify = old['sha256'], Path(old['folder']), False
                    else:
                        digest = sha(source)
                        unpacked = store / 'packages' / digest
                        if unpacked.exists():
                            shutil.rmtree(unpacked)
                        folder, verify = unpack(source, unpacked), True
                    m = read_folder(folder, verify)
                    sources[source.name] = {'bytes': st.st_size, 'mtime_ns': st.st_mtime_ns,
                                            'sha256': digest, 'folder': str(folder)}
                elif source.is_dir():
                    folder, digest = source, None
                    m = read_folder(folder, verify=False)
                else:
                    continue
                if m['id'] in packages:
                    raise ValueError(f'{m["id"]} is also {packages[m["id"]]["source"]}')
                packages[m['id']] = {'id': m['id'], 'source': source.name, 'folder': str(folder),
                                     'sha256': digest or manifest_digest(m), 'manifest': m}
            except (ValueError, OSError, zipfile.BadZipFile) as error:
                messages.append(f'{source.name}: {error}')
    for message in messages:
        log('External content: ' + message)
    save_json(library, {'sources': sources, 'packages': packages, 'messages': messages})
    return {'packages': packages, 'messages': messages}


def manifest_digest(m):
    return hashlib.sha256(json.dumps(m, sort_keys=True).encode()).hexdigest()


# --- the player's choices ----------------------------------------------------------------------

def selection(root):
    path = state(root) / 'selection.json'
    value = json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {}
    enabled = value.get('enabled', True)
    disabled = value.get('disabled', [])
    page = value.get('page', 'own')
    if not isinstance(enabled, bool) or not isinstance(disabled, list) or \
            not all(isinstance(d, str) for d in disabled) or page not in PAGES:
        raise ValueError('content-state/selection.json: expected {"enabled": true|false, "disabled": [ids], '
                         '"page": "own"|"bonus"}')
    return {'enabled': enabled, 'disabled': disabled, 'page': page}


def save_selection(root, enabled=None, disable=(), enable=(), page=None):
    value = selection(root)
    if enabled is not None:
        value['enabled'] = bool(enabled)
    if page is not None:
        if page not in PAGES:
            raise ValueError('page must be one of ' + ', '.join(PAGES))
        value['page'] = page
    value['disabled'] = sorted((set(value['disabled']) | set(disable)) - set(enable))
    save_json(state(root) / 'selection.json', value)
    return value


def selected(root, online=False, log=print):
    """The enabled packages, by id: [(manifest, folder)]. With online on, only those allowed in
    private rooms."""
    choice = selection(root)
    if not choice['enabled']:
        return []
    catalog = scan(root, log)
    out = []
    for ident, record in sorted(catalog['packages'].items()):
        if ident in choice['disabled']:
            continue
        m = record['manifest']
        if online and m.get('online', 'offline-only') != 'private':
            log(f'External content: {ident} is offline only; not loaded with Online on')
            continue
        out.append((m, Path(record['folder'])))
    return out


# --- the game's own paths ----------------------------------------------------------------------

def game_index(game, store):
    """{'paths': set of logical paths (no case), 'archives': set of archive names (no case)} of
    the game, cached in content-state while its archives stay the same."""
    from graphics_tab import Archive
    folder = Path(game) / 'archives'
    paks = sorted(p for p in folder.glob('*.pak'))
    stamp = hashlib.sha256(json.dumps(
        [GAME_INDEX_VERSION] + [[p.name, p.stat().st_size, p.stat().st_mtime_ns] for p in paks]).encode()).hexdigest()
    cache = Path(store) / 'game-index' / (stamp[:24] + '.json')
    if cache.is_file():
        data = json.loads(cache.read_text(encoding='utf-8'))
        return {'paths': set(data['paths']), 'archives': set(data['archives'])}
    paths = set()
    for pak in paks:
        try:
            archive = Archive(pak)
        except (ValueError, struct.error):
            continue
        try:
            paths.update(archive.logical(i).casefold() for i in range(len(archive.names)))
        finally:
            archive.f.close()
    archives = {p.name.casefold() for p in paks}
    if cache.parent.is_dir():
        for old in cache.parent.glob('*.json'):
            old.unlink()
    save_json(cache, {'paths': sorted(paths), 'archives': sorted(archives)})
    return {'paths': paths, 'archives': archives}


def check_against_game(packages, index):
    """Every package only adds: its archives and files are names the game does not have, and no
    two packages share one. Raises ValueError naming the package."""
    owners = {}
    for m, _ in packages:
        for record in m['files']:
            kind = kind_of(record['file'], m['id'])
            if kind == 'extra':
                continue
            if kind == 'shared':  # a copy of a game file: checked byte for byte when built
                key = record['file'].split('/', 1)[1].casefold()
                if key not in index['paths']:
                    raise ValueError(f'{m["id"]}: shared/{key} is not a game file; put it in files/')
                continue
            if kind == 'archive':
                name = record['file'].split('/', 1)[1].casefold()
                if name in index['archives']:
                    raise ValueError(f'{m["id"]}: archives/{name} is a game archive; content cannot replace it')
                key = 'archives/' + name
            else:
                key = record['file'].split('/', 1)[1].casefold()
                if key in index['paths']:
                    raise ValueError(f'{m["id"]}: {key} is a game file; content can only add files')
            if key in owners:
                raise ValueError(f'{m["id"]} and {owners[key]} both have {key}')
            owners[key] = m['id']


def archives(m, folder):
    """[(target, source)] of the package's own archives."""
    return [('archives/' + r['file'].split('/', 1)[1], Path(folder) / r['file'])
            for r in m['files'] if kind_of(r['file'], m['id']) == 'archive']


def game_files(m, folder):
    """[(logical path, source)] of the game files the package adds."""
    return [(r['file'].split('/', 1)[1], Path(folder) / r['file'])
            for r in m['files'] if kind_of(r['file'], m['id']) == 'file']


def shared_files(m, folder):
    """[(logical path, source)] of the game files the package needs loaded with it (shared/)."""
    return [(r['file'].split('/', 1)[1], Path(folder) / r['file'])
            for r in m['files'] if kind_of(r['file'], m['id']) == 'shared']


def track(m):
    """The track_registry.Track of a track package."""
    from track_registry import Track
    # shared/ files are in the update archive, found by path; the streaming list gets only those
    # "stream" names (a converted mod's own list: a sound stream in it breaks the game's loading).
    files = [(stream_kind(p), p) for p, _ in game_files(m, '.')]
    files += [(stream_kind(p), p) for p in m.get('stream', [])]
    return Track(m['map'], image=m.get('image'), texture=m.get('texture'), files=files)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description='List or check the packages in custom_content.')
    parser.add_argument('root', type=Path, help='the custom_content folder')
    args = parser.parse_args()
    result = scan(args.root)
    for ident, record in sorted(result['packages'].items()):
        m = record['manifest']
        print(f'{ident}: {m["name"]["en"]} {m["version"]} by {m["author"]} ({record["source"]})')
    raise SystemExit(1 if result['messages'] else 0)
