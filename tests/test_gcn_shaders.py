"""scripts/gcn_shaders.py: the game's shaders taken from its .pak archives for the shader recipe,
checked on a made-up archive (no game data)."""
from paths import ROOT
import json
import struct
import tempfile
import unittest
from pathlib import Path
import gcn_shaders

TOKEN = 0xBEEB03FF
ENDPGM = 0xBF810000


def shader(shader_hash, shader_type, body_words=0):
    """A GCN program as the PS4 shader compiler lays it out: s_mov_b32 vcc_hi with the distance to
    the binary info, the code up to s_endpgm, padding, then the "OrbShdr" info."""
    words = [TOKEN, 0] + [0xBF800000] * body_words + [ENDPGM]  # s_nop filler
    length = 4 * len(words)
    pad = (-length) % 8 + 8
    info_at = length + pad
    words[1] = info_at // 8 - 1
    code = struct.pack(f'<{len(words)}I', *words)
    bits = (length << 8) | (shader_type << 2)
    info = b'OrbShdr' + bytes([1]) + struct.pack('<I', bits) + bytes(4) + struct.pack('<QI', shader_hash, 0)
    return code, code + bytes(pad) + info


def pak(entries):
    """An IGA v13 archive with stored entries {name: data}."""
    n = len(entries)
    header = bytearray(0x38)
    struct.pack_into('<I', header, 0xc, n)
    table = bytearray(4 * n)
    data_start = 0x38 + 4 * n + 16 * n
    blobs = bytearray()
    rows = bytearray()
    for name, data in entries.items():
        rows += struct.pack('<IIII', data_start + len(blobs), 0, len(data), 0xFFFFFFFF)
        blobs += data
    names = bytearray(4 * n)
    strings = bytearray()
    for i, name in enumerate(entries):
        struct.pack_into('<I', names, 4 * i, 4 * n + len(strings))
        strings += name.encode() + b'\0'
    names += strings
    struct.pack_into('<QI', header, 0x28, data_start + len(blobs), len(names))
    return bytes(header + table + rows + blobs + names)


def read_pack(path):
    data = path.read_bytes()
    assert data[:8] == b'BBGCN002'
    count, = struct.unpack_from('<I', data, 8)
    out = {}
    for i in range(count):
        h, offset, size, typ, _ = struct.unpack_from('<QIIII', data, 16 + 24 * i)
        out[(h, typ)] = data[offset:offset + size]
    return out


class GcnShaderTests(unittest.TestCase):
    def test_shaders_by_hash_and_type(self):
        vs_code, vs = shader(0x1111, 1, body_words=5)
        gs_code, gs = shader(0x2222, 5, body_words=9)
        copy_code, copy = shader(0x2222, 6)  # a geometry shader's copy shader shares its hash
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp) / 'game'
            (game / 'archives').mkdir(parents=True)
            (game / 'archives' / 'shaders_ps4.pak').write_bytes(pak({
                'temporary/octane/data/ps4/output/shaders/Standard.igz': b'\x01' * 40 + vs + b'\x02' * 12,
                'temporary/octane/data/ps4/output/shaders/Fur.igz': gs + bytes(16) + copy,
                'temporary/octane/data/ps4/output/textures/x.igz': vs,  # not a shaders/ entry
            }))
            out = Path(tmp) / 'out'
            self.assertEqual(gcn_shaders.build(game, out), 0)
            found = read_pack(out / 'gcn_shaders.bin')
            self.assertEqual(found, {(0x1111, 1): vs_code, (0x2222, 5): gs_code, (0x2222, 6): copy_code})
            # Up to date: not written again.
            before = (out / 'gcn_shaders.bin').stat().st_mtime_ns
            self.assertEqual(gcn_shaders.build(game, out), 0)
            self.assertEqual((out / 'gcn_shaders.bin').stat().st_mtime_ns, before)
            self.assertEqual(json.loads((out / 'gcn_shaders.json').read_text())['format'], gcn_shaders.FORMAT)

    def test_no_archives(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(gcn_shaders.build(Path(tmp), Path(tmp) / 'out'), 0)
            self.assertFalse((Path(tmp) / 'out' / 'gcn_shaders.bin').exists())


if __name__ == '__main__':
    unittest.main()
