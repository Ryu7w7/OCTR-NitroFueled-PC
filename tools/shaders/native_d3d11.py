"""The shaders written for the PC (gpu/shim/native_shaders), for the Direct3D 11 engine.

Direct3D 11 links stages by register, so a pixel shader takes every varying of its vertex shader in
order: the inputs these shaders leave out for Vulkan's locations go back in, unused. Each result is
checked against the translation it replaces (same input and output signatures) and saved next to it
as <name>.pc.dxbc, which the engine prefers.

  python tools/shaders/native_d3d11.py <folder with the translated .hlsl and .dxbc>
"""
import ctypes
import pathlib
import re
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
NATIVE = HERE.parent.parent / 'gpu' / 'shim' / 'native_shaders'
FLAGS = (1 << 11) | (1 << 15)  # strictness, level 3
VARYINGS = range(15, 25)

d3dc = ctypes.WinDLL('d3dcompiler_47.dll')
d3dc.D3DCompile.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
                            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint,
                            ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
d3dc.D3DDisassemble.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p,
                                ctypes.POINTER(ctypes.c_void_p)]


class Vtbl(ctypes.Structure):
    _fields_ = [('qi', ctypes.c_void_p), ('addref', ctypes.c_void_p), ('release', ctypes.c_void_p),
                ('pointer', ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)),
                ('size', ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p))]


class Blob(ctypes.Structure):
    _fields_ = [('vtbl', ctypes.POINTER(Vtbl))]


def blob_bytes(p):
    v = ctypes.cast(p, ctypes.POINTER(Blob)).contents.vtbl.contents
    return ctypes.string_at(v.pointer(p), v.size(p))


def compile_ps(source, flags=FLAGS):
    code, err = ctypes.c_void_p(), ctypes.c_void_p()
    hr = d3dc.D3DCompile(source, len(source), None, None, None, b'main', b'ps_5_0', flags, 0,
                         ctypes.byref(code), ctypes.byref(err))
    if hr != 0:
        raise SystemExit('fxc: ' + (blob_bytes(err).decode('latin1') if err else hex(hr & 0xffffffff)))
    return blob_bytes(code)


def instructions(dxbc):
    text = ctypes.c_void_p()
    if d3dc.D3DDisassemble(dxbc, len(dxbc), 0, None, ctypes.byref(text)) != 0:
        return -1
    m = re.search(r'// Approximately (\d+) instruction', blob_bytes(text).decode('latin1'))
    return int(m[1]) if m else -1


def signature(dxbc, tag):
    """(semantic, index, register, mask) of a signature chunk."""
    for i in range(struct.unpack_from('<I', dxbc, 28)[0]):
        o = struct.unpack_from('<I', dxbc, 32 + i * 4)[0]
        if dxbc[o:o + 4] == tag:
            base = o + 8
            out = []
            for k in range(struct.unpack_from('<I', dxbc, base)[0]):
                name, index, _, _, register, mask = struct.unpack_from('<6I', dxbc, base + 8 + k * 24)
                text = dxbc[base + name:dxbc.index(b'\0', base + name)].decode()
                out.append((text, index, register, mask & 0xff))
            return out
    return []


def for_d3d11(text):
    """The HLSL without Vulkan's attributes, with every varying in the input struct."""
    text = re.sub(r'\[\[vk::[^\]]*\]\]\s*', '', text)
    m = re.search(r'struct PSIn \{\n(.*?)\n\};', text, re.S)
    lines = m[1].split('\n')
    declared = {}
    others = []
    for line in lines:
        v = re.search(r':\s*VARYING(\d+)', line)
        if v:
            declared[int(v[1])] = line
        elif 'SV_Position' not in line:
            others.append(line)
    body = ['    float4 pos : SV_Position;']
    for n in VARYINGS:
        body.append(declared.get(n, f'    float4 unused{n} : VARYING{n};'))
    if not any('SV_IsFrontFace' in line for line in others):
        others.append('    bool ff : SV_IsFrontFace;')
    return text[:m.start(1)] + '\n'.join(body + others) + text[m.end(1):]


def main(folder):
    folder = pathlib.Path(folder)
    for source in sorted(NATIVE.glob('*.hlsl')):
        name = source.stem
        dxbc = folder / (name + '.dxbc')
        if not dxbc.is_file():
            # a build that only translated (build_d3d11.py --translate-only): this translation is
            # compiled here, as the full build did (fxc level 0, strictness), to compare signatures
            if not (folder / (name + '.hlsl')).is_file():
                print(f'{name}: not in this game, skipped')
                continue
            dxbc.write_bytes(compile_ps((folder / (name + '.hlsl')).read_bytes(), (1 << 14) | (1 << 11)))
        translated = dxbc.read_bytes()
        pc = compile_ps(for_d3d11(source.read_text(encoding='utf-8')).encode('utf-8'))
        for tag in (b'ISGN', b'OSGN'):
            ours, theirs = signature(pc, tag), signature(translated, tag)
            # a varying no one reads keeps its register: only that has to match
            a = [(s, i, r) for s, i, r, _ in ours]
            b = [(s, i, r) for s, i, r, _ in theirs]
            if a != b:
                raise SystemExit(f'{name}: {tag} differs\n{a}\n{b}')
        (folder / (name + '.pc.dxbc')).write_bytes(pc)
        print(f'{name}: {instructions(translated)} -> {instructions(pc)} instructions')


if __name__ == '__main__':
    main(sys.argv[1])
