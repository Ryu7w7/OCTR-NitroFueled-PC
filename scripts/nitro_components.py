"""Validated asset primitives (checksums, paths, byte and copy deltas) and the HUD shader
transform's reference (position_scale; the GPU library applies it, bb_hud_scale.h). Author: tcosi.
GPL-2.0-or-later."""
from pathlib import Path, PurePosixPath
import hashlib,json,os,re,struct

def sha_bytes(data):
    return hashlib.sha256(data).hexdigest()

def sha(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()

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
        if op['payload'].endswith('.delta.json'):
            result = copy_delta(original, json.loads(result))
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

def copy_delta(original, delta):
    """Rebuild resized assets from the user's input, without shipping whole assets."""
    if not isinstance(delta, dict) or set(delta) != {'format', 'source_bytes', 'result_bytes', 'operations'}:
        raise ValueError('Invalid copy delta')
    if delta['format'] != 'copy-delta-v1' or delta['source_bytes'] != len(original):
        raise ValueError('Copy delta source mismatch')
    limit = delta['result_bytes']
    if type(limit) is not int or not 0 <= limit <= 64 * 1024 * 1024:
        raise ValueError('Invalid copy delta result size')
    if not isinstance(delta['operations'], list):
        raise ValueError('Invalid copy delta operations')
    result = bytearray()
    for item in delta['operations']:
        if not isinstance(item, list) or not item:
            raise ValueError('Invalid copy delta operation')
        if item[0] == 'copy' and len(item) == 3:
            offset, length = item[1:]
            if type(offset) is not int or type(length) is not int or offset < 0 or length < 0 or offset + length > len(original):
                raise ValueError('Copy delta range outside source')
            chunk = original[offset:offset + length]
        elif item[0] == 'bytes' and len(item) == 2 and isinstance(item[1], str):
            chunk = bytes.fromhex(item[1])
        else:
            raise ValueError('Unknown copy delta operation')
        if len(result) + len(chunk) > limit:
            raise ValueError('Copy delta exceeds declared result')
        result.extend(chunk)
    if len(result) != limit:
        raise ValueError('Incomplete copy delta result')
    return bytes(result)


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
