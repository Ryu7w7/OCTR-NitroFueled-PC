"""Validated archive/file/shader primitives. Author: tcosi. GPL-2.0-or-later."""
from pathlib import Path, PurePosixPath
import hashlib,json,os,re,shutil,struct,uuid
from archive_reader import Archive

def sha_bytes(data):
    return hashlib.sha256(data).hexdigest()

def sha(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

def save_json(path, obj):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    temp.write_text(json.dumps(obj, indent=2) + '\n', encoding='utf-8')
    os.replace(temp, path)

def long_path(path):
    """On Windows, the absolute path in extended-length form (\\\\?\\C:\\...): a package's files
    (archive/folder/asset names nested in the mod) pass the 259-character limit of a library in
    a deep folder, which Python only lifts this way unless the system enables long paths."""
    path = Path(path)
    if os.name != 'nt' or str(path).startswith('\\\\?\\'):
        return path
    text = os.path.abspath(path)
    return Path('\\\\?\\UNC\\' + text[2:] if text.startswith('\\\\') else '\\\\?\\' + text)

def plain_path(path):
    """A long_path in the usual form, for what the engine opens and for messages."""
    text = str(path)
    if text.startswith('\\\\?\\UNC\\'):
        return Path('\\\\' + text[8:])
    return Path(text[4:]) if text.startswith('\\\\?\\') else Path(text)

def safe_path(value):
    if not isinstance(value, str) or not value or '\\' in value or (':' in value):
        raise ValueError('Expected a relative slash-separated path: ' + str(value))
    p = PurePosixPath(value)
    if p.is_absolute() or any((x in ('', '..', '.') for x in value.split('/'))):
        raise ValueError('Unsafe path: ' + value)
    for part in p.parts:
        if part.endswith((' ', '.')) or re.match('^(con|prn|aux|nul|com[0-9]|lpt[0-9])(?:\\.|$)', part, re.I):
            raise ValueError('Unsafe Windows path: ' + value)
    return p

def under(root, value):
    p = Path(root).joinpath(*safe_path(value).parts)
    if not p.resolve().is_relative_to(Path(root).resolve()):
        raise ValueError('Path escapes root')
    return p

def regular_tree(root):
    root = Path(root)
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            p = Path(directory) / name
            if p.is_symlink() or (hasattr(p, 'is_junction') and p.is_junction()):
                raise ValueError('Linked input is unsupported: ' + str(p))
        for name in files:
            yield (Path(directory) / name)

def snapshot(root):
    return {p.relative_to(root).as_posix(): [p.stat().st_size, p.stat().st_mtime_ns] for p in regular_tree(root)}

def copy_tree(source, target):
    for p in regular_tree(source):
        dest = Path(target) / p.relative_to(source)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dest)

def read_ini(path):
    result = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        k, sep, v = line.partition('=')
        if sep and (not k.lstrip().startswith('#')):
            result[k.strip()] = v.strip()
    return result

def payload_bytes(op, folder, original):
    if sha_bytes(original) != op['original_sha256']:
        raise ValueError('Original content checksum mismatch')
    if 'payload' in op:
        result = under(folder, op['payload']).read_bytes()
    else:
        result = bytearray(original)
        for offset, old, new in op['runs']:
            old = bytes.fromhex(old)
            new = bytes.fromhex(new)
            if offset < 0 or len(old) != len(new) or result[offset:offset + len(old)] != old:
                raise ValueError('Invalid byte delta')
            result[offset:offset + len(old)] = new
        result = bytes(result)
    if sha_bytes(result) != op['patched_sha256']:
        raise ValueError('Patched content checksum mismatch')
    return result

def apply_file(path, op, folder):
    exists = path.exists()
    if op.get('original_sha256') is None:
        if exists:
            raise ValueError('Expected a new file: ' + str(path))
        data = under(folder, op['payload']).read_bytes()
        if sha_bytes(data) != op['patched_sha256']:
            raise ValueError('Replacement checksum mismatch')
    else:
        if not exists:
            raise ValueError('Missing original file: ' + str(path))
        data = payload_bytes(op, folder, path.read_bytes())
    if exists and path.stat().st_nlink != 1:
        raise ValueError('Refusing to write through a hardlink')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)

def patch_archive(path, items):
    if path.stat().st_nlink != 1:
        raise ValueError('Refusing to patch a shared archive')
    a = Archive(path)
    pending = []
    try:
        count = len(a.files)
        sector = a.sector
        for op, folder in items:
            entry = a.files[op['index']]
            if entry['path'] != op['asset']:
                raise ValueError('Archive asset/index mismatch')
            payload = payload_bytes(op, folder, a.extract(entry))
            offset = 56 + count * 4 + op['index'] * 16
            header = struct.unpack_from('<4I', a.data, offset)
            pending.append((offset, header, payload, op))
    finally:
        a.data.close()
        a._file.close()
    with path.open('r+b') as f:
        for offset, header, payload, op in pending:
            f.seek(0, 2)
            end = f.tell()
            position = (end + sector - 1) // sector * sector
            f.write(b'\x00' * (position - end))
            f.write(payload)
            f.seek(offset)
            f.write(struct.pack('<4I', position & 4294967295, header[1] & 4294967040 | position >> 32, len(payload), 4294967295))
        f.flush()
        os.fsync(f.fileno())
    a = Archive(path)
    try:
        for _, _, _, op in pending:
            if sha_bytes(a.extract(a.files[op['index']])) != op['patched_sha256']:
                raise ValueError('Archive output verification failed')
    finally:
        a.data.close()
        a._file.close()

def apply_shader(path, op, folder):
    if op['kind'] == 'shader_file':
        return apply_file(path, op, folder)
    if not path.is_file():
        raise ValueError('Shader cache missing. Launch without shader mods to the main menu, then close normally first.')
    if op['kind'] == 'shader_bytes':
        return apply_file(path, op, folder)
    original = path.read_bytes()
    if sha_bytes(original) != op['original_sha256']:
        raise ValueError('Unrecognized shader cache version')
    data = position_scale(original, float(op['factor']))
    if sha_bytes(data) != op['patched_sha256']:
        raise ValueError('Shader transformation checksum mismatch')
    if path.stat().st_nlink != 1:
        raise ValueError('Shared shader file')
    path.write_bytes(data)

def position_scale(data, factor):
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    if words[0] != 119734787:
        raise ValueError('Invalid SPIR-V shader')
    ins = []
    offset = 5
    while offset < len(words):
        length = words[offset] >> 16
        if not length or offset + length > len(words):
            raise ValueError('Invalid SPIR-V instruction')
        ins.append((words[offset] & 65535, words[offset + 1:offset + length]))
        offset += length
    position = next((a[0] for op, a in ins if op == 71 and a[1:] == [11, 0]))
    ft = next((a[0] for op, a in ins if op == 22 and a[1:] == [32]))
    vt = next((a[0] for op, a in ins if op == 23 and a[1:] == [ft, 4]))
    half, vector, x, scaled, result = range(words[3], words[3] + 5)
    output = words[:5]
    output[3] += 5

    def emit(op, *args):
        output.extend([len(args) + 1 << 16 | op, *args])
    declared = False
    returns = 0
    for op, args in ins:
        if op == 54 and (not declared):
            emit(43, ft, half, struct.unpack('<I', struct.pack('<f', factor))[0])
            declared = True
        if op == 253:
            emit(61, vt, vector, position)
            emit(81, ft, x, vector, 0)
            emit(133, ft, scaled, x, half)
            emit(82, vt, result, scaled, vector, 0)
            emit(62, position, result)
            returns += 1
        emit(op, *args)
    if returns != 1:
        raise ValueError('Unexpected shader control flow')
    return struct.pack('<%dI' % len(output), *output)
