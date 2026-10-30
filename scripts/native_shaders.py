"""The native Vulkan renderer's shaders, built on the player's PC from their own game (bbport.ini
renderer=native; nothing derived from the game ships with OCTR).

tools/shaders/build_spirv.py does the work: the game's GCN programs (archives/shaders_ps4.pak and
the few in eboot.bin) -> HLSL (third_party/gcn2hlsl) -> SPIR-V (DXC, bin/dxc). About 5 minutes and
90 MB the first time; the result is byte-identical to the shaders the renderer was tested with.

<data>/native-shaders/<title>-<version>-g<GENERATION>/ holds hlsl/shaders.idx, spirv/, gcn/ (the
renderer reads each program's OrbShdr hash there), native/ and, later, the renderer's own
pipelines.bin and pipeline_cache.bin. complete.json marks a finished build: an interrupted one
is built again. GENERATION changes when the translator does, so every player rebuilds once."""
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

GENERATION = 2  # 2: quad-uniform branches in pixel shaders (AMD fur)
# The editions whose engine the renderer knows (gpu/shim/bbport_native_vk.cpp, bbport_hook.h): USA
# and EU 1.21, the same engine structures with the two hooked functions 0x10 apart.
SUPPORTED = {('CUSA13795', '01.21'), ('CUSA14876', '01.21')}
# Pixel shaders compiled without DXC's optimizations: NVIDIA 610.88's shader compiler crashed on the
# fur pipeline built from the -O3 one (native/ takes the place of spirv/ in the renderer).
UNOPTIMIZED = ('Fur_3e704240_ps',)
MIN_SHADERS = 3000


def supported(title_id, app_version):
    return (title_id, app_version) in SUPPORTED


def shader_dir(data, title_id, app_version):
    return Path(data) / 'native-shaders' / f'{title_id}-{app_version}-g{GENERATION}'


def ready(folder):
    try:
        return json.loads((Path(folder) / 'complete.json').read_text(encoding='utf-8')).get('generation') == GENERATION
    except (OSError, ValueError):
        return False


def find_dxc(port):
    """DXC: BB_DXC, the package's bin/dxc, the Vulkan SDK's, or one on PATH."""
    exe = 'dxc.exe' if os.name == 'nt' else 'dxc'
    candidates = [os.environ.get('BB_DXC', ''), str(Path(port) / 'bin' / 'dxc' / exe)]
    if os.environ.get('VULKAN_SDK'):
        candidates.append(str(Path(os.environ['VULKAN_SDK']) / ('Bin' if os.name == 'nt' else 'bin') / exe))
    candidates.append(shutil.which('dxc') or '')
    return next((c for c in candidates if c and Path(c).is_file()), None)


def _unoptimized(port, dxc, folder):
    """native/<name>.spv for UNOPTIMIZED, from the same HLSL as spirv/ but with -O0."""
    sys.path.insert(0, str(Path(port) / 'tools' / 'shaders'))
    try:
        import build_spirv
    finally:
        sys.path.pop(0)
    native = folder / 'native'
    native.mkdir(exist_ok=True)
    for name in UNOPTIMIZED:
        hlsl = folder / 'hlsl' / f'{name}.hlsl'
        if not hlsl.is_file():
            continue
        src = native / f'{name}.hlsl'
        src.write_text(build_spirv.pin_locations(hlsl.read_text()))
        cmd = [dxc, '-spirv', '-T', 'ps_6_0', '-E', 'main', '-HV', '2018', '-fspv-target-env=vulkan1.1',
               '-fvk-b-shift', str(build_spirv.B_SHIFT), '0', '-fvk-t-shift', str(build_spirv.T_SHIFT), '0',
               '-fvk-s-shift', str(build_spirv.S_SHIFT), '0', '-fvk-u-shift', str(build_spirv.U_SHIFT), '0',
               '-O0', '-Fo', str(native / f'{name}.spv'), str(src)]
        subprocess.run(cmd, capture_output=True, check=True, creationflags=_no_console())
        src.unlink()


def _no_console():
    return 0x08000000 if os.name == 'nt' else 0  # CREATE_NO_WINDOW


def _percent(line):
    """(percent, stage) for a line of build_spirv.py, else None. The stages' shares follow their
    measured times: translating ~45% of the build, compiling ~50%."""
    def fraction(text):
        done, _, total = text.partition('/')
        return int(done) / int(total) if done.isdigit() and total.isdigit() and int(total) else 0.0
    if line.startswith('1/4'):
        return 1, 'extract'
    if line.startswith('2/4'):
        return 4, 'export'
    if line.startswith(('3/4', '1-2/4')):
        return 8, 'translate'
    if line.startswith('translated '):
        return 8 + int(44 * fraction(line.split()[1])), 'translate'
    if line.startswith('4/4'):
        return 52, 'compile'
    if line.startswith('compiled '):
        return 52 + int(46 * fraction(line.split()[1])), 'compile'
    return None


def build(port, game, folder, say, python=None, progress=None):
    """Builds the shaders into `folder`; True when they are ready. `say` gets a few progress lines,
    `progress(percent, stage)` every step (stage: extract, export, translate, compile, finish)."""
    progress = progress or (lambda _percent, _stage: None)
    port, folder = Path(port), Path(folder)
    dxc = find_dxc(port)
    if not dxc:
        say('Native Vulkan: DXC (the shader compiler) is missing from bin/dxc')
        return False
    work = folder.with_name(folder.name + '.building')
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    started = time.time()
    env = dict(os.environ, GCN2HLSL_NO_FXC='1', PYTHONIOENCODING='utf-8')
    cmd = [python or sys.executable, '-X', 'utf8', str(port / 'tools' / 'shaders' / 'build_spirv.py'),
           str(port / 'third_party' / 'gcn2hlsl'), str(game), str(work), '--dxc', str(Path(dxc).resolve())]
    process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, cwd=str(work),
                               creationflags=_no_console())
    for raw in process.stdout:
        line = raw.decode('utf-8', errors='replace').rstrip()
        step = _percent(line)
        if step:
            progress(*step)
        # the steps, every 500th translated shader, the result; not the per-file lines
        if line[:2] in ('1/', '2/', '3/', '4/', '1-') or line.startswith(('SPIR-V:', 'Done in')) or \
                (line.startswith('translated ') and line.split()[1].split('/')[0].isdigit()
                 and int(line.split()[1].split('/')[0]) % 500 == 0):
            say(f'Native Vulkan shaders: {line}')
    if process.wait() != 0:
        say('Native Vulkan: the shader build failed (see the lines above)')
        return False
    try:
        results = json.loads((work / 'spirv' / 'spirv.json').read_text(encoding='utf-8'))
    except (OSError, ValueError):
        results = {}
    good = sum(1 for r in results.values() if r.get('ok'))
    if good < MIN_SHADERS:
        say(f'Native Vulkan: only {good} shaders compiled')
        return False
    progress(99, 'finish')
    try:
        _unoptimized(port, dxc, work)
    except (OSError, subprocess.CalledProcessError) as error:
        say(f'Native Vulkan: the unoptimized fur shader was not built ({error}); the usual one is used')
    # keep what the renderer reads; the game's own shader files and the HLSL go
    shutil.rmtree(work / 'igz', ignore_errors=True)
    for f in (work / 'hlsl').iterdir():
        if f.suffix in ('.hlsl', '.dxbc'):
            f.unlink()
    (work / 'complete.json').write_text(json.dumps({'generation': GENERATION, 'shaders': good,
                                                    'seconds': round(time.time() - started)}), encoding='utf-8')
    shutil.rmtree(folder, ignore_errors=True)
    work.rename(folder)
    say(f'Native Vulkan: {good} shaders ready in {time.time() - started:.0f} s')
    return True


def environment(folder, temporal):
    """The variables that start the native renderer (the lab's "native 100" set)."""
    env = {
        'BB_NATIVE_VK': '1', 'BB_VK_SHOW': '1', 'BB_VK_ALONE': '1', 'BB_VK_NATIVE_ONLY': '1',
        'BB_VK_EARLY_LABELS': '1',
        # NVIDIA 610.88 crashed compiling two fur pipelines at once
        'BB_VK_COMPILE_THREADS': '1',
        'BB_VK_SHADERS': str(folder),
    }
    if temporal in ('fsr3', 'dlss'):
        env['BB_TEMPORAL'] = 'fsr1'  # FSR 3 and DLSS live in shadPS4's rasterizer
    return env


def main(argv=None):
    """run.sh's way in (run.py calls the functions above): prepares the shaders when needed and
    writes the native renderer's variables to --env-file (KEY=VALUE, each ending in a NUL byte);
    nothing is written when the game starts with Vulkan (shadPS4). "@progress" lines go to stdout
    for the launcher's progress window."""
    import argparse
    parser = argparse.ArgumentParser()
    for name in ('--data', '--game', '--title-id', '--app-version', '--env-file'):
        parser.add_argument(name, required=True)
    parser.add_argument('--temporal', default='')
    a = parser.parse_args(argv)
    say = lambda text: print(text, flush=True)  # noqa: E731
    if not supported(a.title_id, a.app_version):
        say(f'Native Vulkan: only the USA or EU 1.21 game (CUSA13795, CUSA14876) for now; this is {a.title_id} '
            f'{a.app_version}, so the game starts with Vulkan (shadPS4)')
        return 0
    folder = shader_dir(a.data, a.title_id, a.app_version)
    if not ready(folder):
        say('Native Vulkan: preparing its shaders from your game (only the first time, a few minutes)')
        last = [-1]

        def progress(percent, stage):
            if percent != last[0]:
                last[0] = percent
                say(f'@progress native-shaders {percent} {stage}')
        built = build(Path(__file__).resolve().parent.parent, a.game, folder, say, progress=progress)
        say(f'@progress native-shaders done {"ok" if built else "failed"}')
    if not ready(folder):
        say('Native Vulkan: its shaders are not ready, so the game starts with Vulkan (shadPS4)')
        return 0
    with open(a.env_file, 'wb') as out:
        for key, value in environment(folder, a.temporal).items():
            out.write(f'{key}={value}'.encode('utf-8') + b'\0')
    say('Graphics engine: native Vulkan (experimental)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
