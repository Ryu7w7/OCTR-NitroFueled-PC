"""The port's content files kept encrypted on disk (27/11): the game reads them through the port's file layer,
decrypted in memory (src/runtime_content_crypt.c, which describes the format); archive tools see only noise.

    content_crypt.py encrypt-track content/tracks/<id>       its level archives and preview video, in place
    content_crypt.py encrypt SOURCE TARGET                   one file
    content_crypt.py decrypt SOURCE TARGET                   back (a check, or to edit the track again)
    content_crypt.py info FILE...

The key (64 hex digits) is in a file kept out of the repository: --key, else $OCTR_CONTENT_KEY_FILE, else
octr-content.key beside the checkout (the one official builds are made with, gpu/CMakeLists.txt). Needs
Python's cryptography package (pip install cryptography); the game does not.

An IGA v13 archive keeps its index (header, hashes, records, tables) and its names as they are: the port's
scripts read them (scripts/track_content.py hides and renames entries through archive patches). A track's
registration archive (update3.pak: the menus' lists, read by the scripts when the game starts) is not encrypted.
"""
import argparse
import json
import os
import secrets
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TRAILER = 48
MAGIC = b'OCTRENC1'


def key_file(path=None):
    if path:
        return Path(path)
    if os.environ.get('OCTR_CONTENT_KEY_FILE'):
        return Path(os.environ['OCTR_CONTENT_KEY_FILE'])
    return ROOT.parent / 'octr-content.key'


def load_key(path=None):
    f = key_file(path)
    try:
        text = f.read_text(encoding='ascii').strip()
    except OSError:
        sys.exit(f'No content key at {f} (make one with: content_crypt.py new-key)')
    if len(text) != 64:
        sys.exit(f'{f}: the key must be 64 hex digits')
    return bytes.fromhex(text)


def clear_ranges(data):
    """(prefix, tail_at, tail_len): an IGA v13 archive's index and names; (0, 0, 0) for any other file."""
    if len(data) < 0x38:
        return 0, 0, 0
    u32 = lambda o: struct.unpack_from('<I', data, o)[0]
    if u32(0) != 0x1A414749 or u32(4) != 13:
        return 0, 0, 0
    count = u32(0xC)
    tables_size = u32(0x1C) * 4 + u32(0x20) * 2 + u32(0x24)
    if u32(8) != count * 20 + tables_size:
        raise ValueError('an IGA v13 header whose index size does not add up')
    prefix = 0x38 + u32(8)
    names_at, names_size = struct.unpack_from('<Q', data, 0x28)[0], u32(0x30)
    if prefix > len(data) or names_at + names_size > len(data):
        raise ValueError('an IGA v13 archive shorter than its index says')
    return prefix, names_at, names_size


def trailer(data):
    """The file's trailer as a dict, or None when it is not an encrypted file."""
    if len(data) < TRAILER or data[-TRAILER:-TRAILER + 8] != MAGIC:
        return None
    t = data[-TRAILER:]
    nonce = t[8:20]
    prefix, tail_at, tail_len, zero, size = struct.unpack_from('<IQIIQ', t, 20)
    if size != len(data) - TRAILER or zero:
        raise ValueError('a damaged content trailer')
    return {'nonce': nonce, 'prefix': prefix, 'tail_at': tail_at, 'tail_len': tail_len, 'size': size}


def stream_xor(key, nonce, data):
    """data XOR the ChaCha20 stream (RFC 7539: block counter from 0 at the file's first byte)."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
    return Cipher(algorithms.ChaCha20(key, b'\0\0\0\0' + nonce), mode=None).encryptor().update(data)


def keep_clear(out, plain, prefix, tail_at, tail_len):
    out[:prefix] = plain[:prefix]
    out[tail_at:tail_at + tail_len] = plain[tail_at:tail_at + tail_len]


def encrypt(key, plain):
    if trailer(plain) is not None:
        raise ValueError('already encrypted')
    prefix, tail_at, tail_len = clear_ranges(plain)
    nonce = secrets.token_bytes(12)
    out = bytearray(stream_xor(key, nonce, plain))
    keep_clear(out, plain, prefix, tail_at, tail_len)
    out += MAGIC + nonce + struct.pack('<IQIIQ', prefix, tail_at, tail_len, 0, len(plain))
    return bytes(out)


def decrypt(key, data):
    t = trailer(data)
    if t is None:
        raise ValueError('not an encrypted content file')
    body = data[:t['size']]
    out = bytearray(stream_xor(key, t['nonce'], body))
    keep_clear(out, body, t['prefix'], t['tail_at'], t['tail_len'])
    return bytes(out)


def write_atomic(path, data):
    path = Path(path)
    temp = path.with_name(path.name + '.tmp')
    temp.write_bytes(data)
    os.replace(temp, path)


def encrypt_file(key, source, target):
    plain = Path(source).read_bytes()
    data = encrypt(key, plain)
    if decrypt(key, data) != plain:   # never write a file that does not come back
        raise RuntimeError(f'{source}: decrypting it again did not give it back')
    write_atomic(target, data)
    return len(plain)


def track_files(folder):
    """A track package's files to encrypt: its level archives and its preview video (track.json)."""
    m = json.loads((Path(folder) / 'track.json').read_text(encoding='utf-8'))
    names = list(m.get('archives', []))
    if m.get('preview'):
        names.append(m['preview'])
    return [Path(folder) / n for n in names]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--key', help='the content key file')
    sub = ap.add_subparsers(dest='command', required=True)
    sub.add_parser('new-key', help='make the key file (once; keep a copy safe, never commit it)')
    t = sub.add_parser('encrypt-track')
    t.add_argument('folder')
    for name in ('encrypt', 'decrypt'):
        c = sub.add_parser(name)
        c.add_argument('source')
        c.add_argument('target')
    i = sub.add_parser('info')
    i.add_argument('files', nargs='+')
    a = ap.parse_args(argv)
    if a.command == 'new-key':
        f = key_file(a.key)
        if f.exists():
            sys.exit(f'{f} exists already: content encrypted with it would no longer open with a new one')
        f.write_text(secrets.token_hex(32) + '\n', encoding='ascii')
        print(f'Content key made: {f} (keep a copy safe; never commit or share it)')
    elif a.command == 'info':
        for name in a.files:
            data = Path(name).read_bytes()
            t = trailer(data)
            print(f'{name}: ' + ('not encrypted' if t is None else
                                 f"encrypted, {t['size']} bytes, clear {t['prefix']} + {t['tail_len']} at {t['tail_at']}"))
    elif a.command == 'encrypt-track':
        key = load_key(a.key)
        for path in track_files(a.folder):
            if trailer(path.read_bytes()) is not None:
                print(f'{path.name}: encrypted already')
                continue
            size = encrypt_file(key, path, path)
            print(f'{path.name}: encrypted ({size} bytes)')
        print('Now make the content manifest again (the files\' SHA-256 changed) and upload the files.')
    elif a.command == 'encrypt':
        size = encrypt_file(load_key(a.key), a.source, a.target)
        print(f'{a.target}: encrypted ({size} bytes)')
    else:
        write_atomic(a.target, decrypt(load_key(a.key), Path(a.source).read_bytes()))
        print(f'{a.target}: decrypted')


if __name__ == '__main__':
    main()
