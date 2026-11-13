"""The Direct3D 11 engine's translations compiled to DXBC, at the first start (run.py
prepare_d3d11_shaders, after build_d3d11.py --translate-only).

Each <name>.hlsl without its <name>.dxbc is compiled exactly as the engine would compile it in the
game (gpu/shim/d3d11/renderer.cpp CompileTranslation: fxc without its optimizer, which the GPU
driver's own compiler makes up for, and strictness) and written whole (a temporary file renamed).
Compiling them in the game instead left draws out until their shader was ready (flicker, missing
and wrongly coloured surfaces) and took the CPU from the game for many minutes: here it takes
about 5 minutes on 8 threads, once, like the native Vulkan renderer's shaders.

  python tools/shaders/compile_d3d11.py <folder with the .hlsl> [threads]

Prints "compiled N/M" lines (run.py turns them into the launcher's progress bar).
"""
import concurrent.futures
import ctypes
import os
import pathlib
import sys
import time

FLAGS = (1 << 2) | (1 << 11)  # D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_STRICTNESS

d3dc = ctypes.WinDLL('d3dcompiler_47.dll')
d3dc.D3DCompile.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p,
                            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint,
                            ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]


def blob_bytes(p):
    if not p.value:
        return b''
    vt = ctypes.cast(p, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    pointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])
    data = ctypes.string_at(pointer(p), size(p))
    release(p)
    return data


def target(name):
    kind = name.rsplit('_', 1)[-1]
    return b'gs_5_0' if kind == 'gs' else b'vs_5_0' if kind in ('vs', 'es') else b'ps_5_0'


def compile_one(source):
    """None when written, else the error."""
    name = source.stem
    text = source.read_bytes()
    code, err = ctypes.c_void_p(), ctypes.c_void_p()
    hr = d3dc.D3DCompile(text, len(text), name.encode(), None, None, b'main', target(name), FLAGS, 0,
                         ctypes.byref(code), ctypes.byref(err))
    data, message = blob_bytes(code), blob_bytes(err)
    if hr != 0 or not data:
        return f'{name}: {message.decode("latin1", "replace")[:160] or hex(hr & 0xffffffff)}'
    out = source.with_suffix('.dxbc')
    temp = source.with_name(f'{name}.dxbc.tmp{os.getpid()}')
    temp.write_bytes(data)
    os.replace(temp, out)
    return None


def main(folder, threads=None):
    folder = pathlib.Path(folder)
    todo = [s for s in sorted(folder.glob('*.hlsl')) if not s.with_suffix('.dxbc').is_file()]
    # the slowest (vertex shaders) first, so the last minute is not one long shader alone
    todo.sort(key=lambda s: 0 if s.stem.endswith(('_vs', '_es', '_gs')) else 1)
    threads = threads or max(1, os.cpu_count() or 1)
    t0 = time.time()
    failed = []
    print(f'compiled 0/{len(todo)}', flush=True)
    with concurrent.futures.ThreadPoolExecutor(threads) as pool:  # D3DCompile runs without the GIL
        for n, error in enumerate(pool.map(compile_one, todo), 1):
            if error:
                failed.append(error)
            if n % 25 == 0 or n == len(todo):
                print(f'compiled {n}/{len(todo)}', flush=True)
    for error in failed[:10]:
        print('  does not compile:', error)
    print(f'{len(todo) - len(failed)} of {len(todo)} shaders compiled in {time.time() - t0:.0f} s ({threads} threads)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else None))
