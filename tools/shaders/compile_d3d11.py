"""The Direct3D 11 engine's translations compiled to DXBC, before the game starts:
run.py compiles every translation here at the first start (about 2-3 minutes on 8 threads since
v0.8.8 sizes the constant buffers); what fails, the game compiles as it needs it
(gpu/shim/d3d11/renderer.cpp CompileTranslation, the same flags as here).

Each <name>.hlsl without its <name>.dxbc is compiled exactly as the engine would compile it in the
game (flags() below) and written whole (a temporary file renamed).

  python tools/shaders/compile_d3d11.py <folder with the .hlsl> [threads]
  python tools/shaders/compile_d3d11.py <folder with the .hlsl> --forget
  python tools/shaders/compile_d3d11.py <folder with the .hlsl> --start tools/shaders/d3d11_order.txt

Prints "compiled N/M" lines (run.py turns them into the launcher's progress bar).
"""
import concurrent.futures
import ctypes
import os
import pathlib
import sys
import time

FLAGS = (1 << 2) | (1 << 11)  # D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_ENABLE_STRICTNESS
# Every pixel shader with the optimizer (v0.8.8). Unoptimized, the GPU driver runs some of them wrong
# where the shader debugger does not: the post process drew wrong colours and green and pink
# smudges, and the grass, leaves and palms killed every pixel at their alpha test (RenderDoc: the
# same HLSL optimized draws them). The vertex shaders, the slowest to compile, stay unoptimized:
# the pixel shaders take about 4 times longer this way, the first start about twice as long.
OPTIMIZED_FLAGS = (1 << 14) | (1 << 11)  # D3DCOMPILE_OPTIMIZATION_LEVEL0 | D3DCOMPILE_ENABLE_STRICTNESS
# compiled.txt in the folder names this; an older folder's pixel shaders are compiled again
VERSION = '3'


def optimized(name):
    return name.endswith('_ps')


def flags(name):
    return OPTIMIZED_FLAGS if optimized(name) else FLAGS


def forget_outdated(folder):
    """Removes the .dxbc an older version compiled differently (run.py, before its check)."""
    folder = pathlib.Path(folder)
    stamp = folder / 'compiled.txt'
    try:
        if stamp.read_text().strip() == VERSION:
            return 0
    except OSError:
        pass
    removed = 0
    for source in folder.glob('*.hlsl'):
        if optimized(source.stem) and source.with_suffix('.dxbc').is_file():
            source.with_suffix('.dxbc').unlink()
            removed += 1
    return removed

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
    hr = d3dc.D3DCompile(text, len(text), name.encode(), None, None, b'main', target(name), flags(name), 0,
                         ctypes.byref(code), ctypes.byref(err))
    data, message = blob_bytes(code), blob_bytes(err)
    if hr != 0 or not data:
        return f'{name}: {message.decode("latin1", "replace")[:160] or hex(hr & 0xffffffff)}'
    out = source.with_suffix('.dxbc')
    temp = source.with_name(f'{name}.dxbc.tmp{os.getpid()}')
    temp.write_bytes(data)
    os.replace(temp, out)
    return None


def start_names(order):
    """The [start] section of tools/shaders/d3d11_order.txt (the title screen's and menus' shaders)."""
    names, section = [], None
    for line in pathlib.Path(order).read_text(encoding='utf-8').splitlines():
        line = line.strip()
        if line.startswith('['):
            section = line
        elif line and not line.startswith('#') and section == '[start]':
            names.append(line)
    return names


def main(folder, threads=None, only=None):
    """Compiles the folder's translations without their DXBC; `only`: just these names (--start)."""
    folder = pathlib.Path(folder)
    if forget_outdated(folder):
        print('pixel shaders compiled again with the optimizer', flush=True)
    todo = [s for s in sorted(folder.glob('*.hlsl')) if not s.with_suffix('.dxbc').is_file()]
    if only is not None:
        wanted = set(only)
        todo = [s for s in todo if s.stem in wanted]
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
    (folder / 'compiled.txt').write_text(VERSION + '\n')
    print(f'{len(todo) - len(failed)} of {len(todo)} shaders compiled in {time.time() - t0:.0f} s ({threads} threads)')
    return 0


def forget(folder):
    """--forget (run.py, v0.8.8): the DXBC an older version compiled otherwise go, for the game to
    compile them again as it runs; the folder is then marked as this version's."""
    folder = pathlib.Path(folder)
    removed = forget_outdated(folder)
    (folder / 'compiled.txt').write_text(VERSION + '\n')
    print(f'{removed} shaders compiled by an older version removed (the game compiles them again)')
    return 0


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[2] == '--forget':
        sys.exit(forget(sys.argv[1]))
    if len(sys.argv) > 3 and sys.argv[2] == '--start':  # run.py: before the game starts
        sys.exit(main(sys.argv[1], None, start_names(sys.argv[3])))
    sys.exit(main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else None))
