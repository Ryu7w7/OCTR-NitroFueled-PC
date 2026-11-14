#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""SacredFire on Windows: the counterpart of run.sh. Prepares the game image and starts
sacredfire.exe.

Same environment variables as run.sh (BB_GAME_DIR, BB_DATA_DIR, BB_USER_DIR, BB_CONFIG, BB_NEO,
BB_FPS, BB_MODS_*, BB_PATCHES_*, BB_CLEAR_SHADER_CACHE, BB_SAVE_LOG, ...). Extra arguments go to
sacredfire.exe. The in-game restart ends the game with RESTART_CODE and this script starts it
again (BB_RESTART_EXIT_CODE); `--after PID` (the older BB_RESTART_COMMAND way) waits for the
previous process to end first.
Works from the MSYS2 CLANG64 Python or a python.org Python.
"""
import ctypes
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import threading
import time

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
if hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')

PORT = Path(__file__).resolve().parent
TITLE_ID = 'CUSA13795'
APP_VERSION = '01.21'
RESTART_CODE = 75  # the game asks to be started again (in-game "Apply and restart")
RANGE_TAKEN_CODE = 25  # a program took part of the PS4 address range, a different place each start
RANGE_TAKEN_TRIES = 8
START_FAULT_CODE = 3  # a fault the game could not go on from (probe.c)
START_FAULT_SECONDS = 45  # the title screen comes later than this
START_FAULT_TRIES = 2
UNSUPPORTED_GPU_CODE = 24  # no Vulkan 1.3 or no sparse buffers: GPU, driver or loader (gpu/.../vk_platform.h)
# Windows could not give the game its memory (RAM plus the page file): the game's "Runtime: cannot
# commit ... (error 1455)" line, after which it stops a moment later on a fault (code 3). The
# launchers tell the player to enlarge the page file instead of "code 3".
OUT_OF_MEMORY_CODE = 26
OUT_OF_MEMORY = re.compile(r'^Runtime: cannot commit \d+ MiB of guest memory \(error (1455|1450|8)\)')

sys.path.insert(0, str(PORT / 'scripts'))
import archive_patch  # noqa: E402  (scripts/)
import button_prompts  # noqa: E402
import season_art  # noqa: E402
import signed_in_name  # noqa: E402
import ctr_tv_logo  # noqa: E402
import track_names  # noqa: E402
import speed_mode  # noqa: E402
import item_rules  # noqa: E402
import online_mode  # noqa: E402
import perf_report  # noqa: E402
import native_shaders  # noqa: E402
import aspect_ratio  # noqa: E402
import patches  # noqa: E402
import boot_cache  # noqa: E402
import launch_lock  # noqa: E402
import ryunet_session  # noqa: E402
sys.path.pop(0)

LOG = None  # last_run.log (or BB_SAVE_LOG's file): everything this launch prints
PERF = None  # the performance diagnostic's collector (scripts/perf_report.py), fed the game's lines
# Vulkan's start-up, from the game's lines: when its window opened, whether the GPU was opened
# ("GPU: <name>, <n> MiB VRAM"), the layers installed. A player's game sat on a black window between
# the two (an overlay's Vulkan layer or a driver hanging): see watch_vulkan_start.
VULKAN_START = {'window': 0.0, 'ready': False, 'layers': ''}
VULKAN_START_SECONDS = 20


def fail(message, code=1):
    print(message, file=sys.stderr)
    if LOG:
        LOG.write(message + '\n')
        LOG.flush()
    sys.exit(code)


def blocked(exe, error):
    """Windows refused to start the game (WinError 5): an antivirus or Smart App Control blocked
    or quarantined it, usually right after an update replaced it with a new build."""
    fail(f'Windows did not let {exe.name} start ({error.strerror or error}). An antivirus or Smart App '
         f'Control blocked it, or moved it to quarantine. Restore it from the quarantine if needed, add '
         f'the game folder ({exe.parent.parent}) as an exclusion, and start the game again.\n'
         f'Windows no dejó iniciar {exe.name}. Un antivirus o el Control inteligente de aplicaciones lo '
         f'bloqueó o lo puso en cuarentena. Restáuralo si hace falta, agrega la carpeta del juego como '
         f'exclusión y vuelve a iniciar el juego.')


def wait_for(pid):
    kernel32 = ctypes.windll.kernel32
    SYNCHRONIZE = 0x00100000
    handle = kernel32.OpenProcess(SYNCHRONIZE, False, pid)
    if handle:
        kernel32.WaitForSingleObject(handle, 30000)
        kernel32.CloseHandle(handle)


def no_console():
    """Started without a console: console programs must not open their own window."""
    return 0 if ctypes.windll.kernel32.GetConsoleWindow() else subprocess.CREATE_NO_WINDOW


def parse_cpu_features(output):
    """Instruction sets `sacredfire.exe --cpu-features` reports missing ([] when complete)."""
    for line in output.splitlines():
        if line.startswith('CPU: missing '):
            return [name.strip() for name in line[len('CPU: missing '):].split(',') if name.strip()]
    return []


def isa_rewrite_wanted(missing, env):
    """BB_ISA_REWRITE=1 forces the ISA rewrite table, 0 disables it; otherwise the CPU decides."""
    policy = env.get('BB_ISA_REWRITE')
    if policy in ('0', '1'):
        return policy == '1'
    return bool(missing)


class LogLimit:
    """Keeps last_run.log small enough to send (a player's grew to about 100 MB). A line that keeps
    coming (the same words, its numbers aside) is written its first SAME times, then one in EVERY with
    its count; past MAX bytes, only the lines that say something went wrong; past twice that, nothing.
    The console and the performance report still get every line."""
    SAME, EVERY, MAX = 100, 1000, 10 << 20
    SHAPE = re.compile(r'0x[0-9a-fA-F]+|\d+')
    # what went wrong, not a count of it ("0 failed" in the renderer's statistics)
    CRITICAL = re.compile(r'(?i)\b(crash|fatal|exception|corrupt|bailing|abort|assert|unhandled)|'
                          r'(?<!\d )\b(error|errors|failed|fails)\b|^---')

    def __init__(self, written=0):
        self.seen = {}
        self.written = written  # an in-game restart appends to the same file

    def __call__(self, text):
        kept = []
        for line in text.splitlines(keepends=True):
            shape = self.SHAPE.sub('#', line[:160])
            count = self.seen.get(shape, 0) + 1
            if shape in self.seen or len(self.seen) < 50000:
                self.seen[shape] = count
            if count == self.SAME + 1:
                line = f'(log: the next line keeps coming; from here only one in {self.EVERY} is kept)\n' + line
            elif count > self.SAME:
                if count % self.EVERY:
                    continue
                line = f'(x{count}) {line}'
            if self.written >= 2 * self.MAX or (self.written >= self.MAX and not self.CRITICAL.search(line)):
                continue
            kept.append(line)
            self.written += len(line)
            if self.written >= self.MAX > self.written - len(line):
                kept.append(f'(log: over {self.MAX >> 20} MB; from here only the errors)\n')
        return ''.join(kept)


LOG_LIMIT = LogLimit()


def emit(text):
    try:
        sys.stdout.write(text)
        sys.stdout.flush()
    except Exception:
        pass
    if LOG:
        try:
            LOG.write(LOG_LIMIT(text))
            LOG.flush()
        except Exception:
            pass
    if PERF:
        try:
            PERF.feed(text)
        except Exception:
            pass
    for line in text.splitlines():
        if line.startswith('Window: ') and not VULKAN_START['window']:
            VULKAN_START['window'] = time.time()
        elif line.startswith('GPU: ') and ' MiB VRAM' in line:
            VULKAN_START['ready'] = True
        elif line.startswith('Vulkan: starting'):
            VULKAN_START['layers'] = line.partition('layers installed: ')[2].strip()
        elif OUT_OF_MEMORY.match(line):
            VULKAN_START['out_of_memory'] = True


def say(message):
    emit(message + '\n')


def startup_call(label, function, *args, **kwargs):
    if os.environ.get('BB_STARTUP_TIMING') != '1':
        return function(*args, **kwargs)
    started = time.perf_counter()
    try:
        return function(*args, **kwargs)
    finally:
        say(f'Startup: {label} {time.perf_counter() - started:.3f} s')


def run_python(script, *args, capture=False, check=True):
    # -X utf8: the embeddable Python of the portable package ignores PYTHONUTF8, and the scripts
    # print the game's title (with its trademark sign) to a pipe.
    command = [sys.executable, '-X', 'utf8', str(PORT / script), *map(str, args)]
    result = startup_call(script, subprocess.run, command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=None if capture else subprocess.STDOUT, text=True,
                            encoding='utf-8', errors='replace', creationflags=no_console())
    if not capture and result.stdout:
        emit(result.stdout)
    if check and result.returncode:
        sys.exit(result.returncode)
    return result if capture else None


def retry_failed_start(code, seconds, tries):
    """Whether to start the game again because it stopped on a fault while starting. Two such faults
    (a global object not there yet) are races: the same PCs start the next time."""
    return code == START_FAULT_CODE and seconds < START_FAULT_SECONDS and tries < START_FAULT_TRIES


def retry_taken_range(code, tries):
    """Whether to start the game again because the PS4 address range was taken: it fails in an
    instant, before the window opens, and the next start usually finds the range free."""
    return code == RANGE_TAKEN_CODE and tries < RANGE_TAKEN_TRIES


# tools/shaders/compile_d3d11.py VERSION (tests/test_d3d11_compile.py keeps them equal)
D3D11_COMPILED_VERSION = '3'
# The translation's (out/d3d11-shaders/hlsl/translated.txt): another one is translated again, its
# DXBC removed. 2 (v0.8.8): constant buffers declared as large as the shader reads.
D3D11_TRANSLATION_VERSION = '2'


def d3d11_step(script, args, pattern, start, span):
    """One step of prepare_d3d11_shaders: its "<word> N/M" lines move the launcher's progress bar
    from start to start + span percent. Whether it ended well."""
    command = [sys.executable, '-X', 'utf8', str(PORT / 'tools' / 'shaders' / script), *map(str, args)]
    last = -1
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding='utf-8', errors='replace', creationflags=no_console())
    for line in process.stdout:
        m = re.match(pattern + r' (\d+)/(\d+)', line)
        if m:
            percent = start + int(m[1]) * span // max(1, int(m[2]))
            if percent != last:
                last = percent
                emit(f'@progress native-shaders {percent} Direct3D 11\n')
        elif line.strip():
            emit('  ' + line)
    return process.wait() == 0


D3D11_ORDER = PORT / 'tools' / 'shaders' / 'd3d11_order.txt'


def prepare_d3d11_shaders(game, folder):
    """The Direct3D 11 engine's shaders, the first time, before the game starts: the game's programs
    translated to HLSL (tools/shaders/build_d3d11.py --translate-only, about 30 s), the shaders written
    for the PC (tools/shaders/native_d3d11.py) and every translation compiled to DXBC
    (tools/shaders/compile_d3d11.py, about 2-3 minutes since v0.8.8 declares the constant buffers
    as large as each shader reads: up to 30 minutes before). Kept, so later starts compile nothing.
    What fails here the game compiles again as it needs it (gpu/shim/d3d11/renderer.cpp, in the
    order of tools/shaders/d3d11_order.txt, a draw waiting for its shader). A folder an older version
    translated is translated again, one it compiled with other flags compiled again
    (compile_d3d11.py --forget). "@progress native-shaders" lines (the same as the native
    renderer's): the launchers show a progress window meanwhile."""
    hlsl = folder / 'hlsl'
    translated = (hlsl / 'shaders.idx').is_file()
    try:
        translated = translated and (hlsl / 'translated.txt').read_text().strip() == D3D11_TRANSLATION_VERSION
    except OSError:
        translated = False
    if not translated and (hlsl / 'shaders.idx').is_file():  # an older translation: none of it is kept
        for old in hlsl.glob('*.dxbc'):
            old.unlink(missing_ok=True)
        (hlsl / 'shaders.idx').unlink(missing_ok=True)
    try:  # compile_d3d11.py VERSION: the flags the DXBC in the folder were compiled with
        current = (hlsl / 'compiled.txt').read_text().strip() == D3D11_COMPILED_VERSION
    except OSError:
        current = False
    if translated and not current:
        run_python('tools/shaders/compile_d3d11.py', hlsl, '--forget', check=False)
    if translated and all(s.with_suffix('.dxbc').is_file() for s in hlsl.glob('*.hlsl')):
        return True
    say('Direct3D 11: preparing its shaders from your game (only the first time, about 3 minutes)')
    emit('@progress native-shaders 0 Direct3D 11\n')
    ok = translated or (d3d11_step('build_d3d11.py', [game, folder, '--translate-only'], 'translated', 0, 20)
                        and (hlsl / 'shaders.idx').is_file())
    if ok and not translated:
        run_python('tools/shaders/native_d3d11.py', hlsl, check=False)
        run_python('tools/shaders/compile_d3d11.py', hlsl, '--forget', check=False)
        (hlsl / 'translated.txt').write_text(D3D11_TRANSLATION_VERSION + '\n')
    if ok:  # what fails here, the game compiles again
        d3d11_step('compile_d3d11.py', [hlsl], 'compiled', 20, 80)
    emit(f'@progress native-shaders done {"ok" if ok else "failed"}\n')
    return ok


def find_executable(name):
    """bin/ (a packaged build), else out/ (built with build.sh)."""
    for candidate in (PORT / 'bin' / name, PORT / 'out' / name):
        if candidate.is_file():
            return candidate
    return None


def ini_has(path, line):
    try:
        return any(l.strip() == line for l in Path(path).read_text(encoding='utf-8', errors='replace').splitlines())
    except OSError:
        return False


def ini_value(path, key):
    try:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            name, sep, value = line.partition('=')
            if sep and not line.lstrip().startswith('#') and name.strip() == key:
                return value.strip()
    except OSError:
        pass
    return None


# bbport.ini render_res (or a DLSS quality with temporal=dlss) picks the game's rendering path
# (scripts/patches.py render_plan, docs/sacredfire/13-resolution.md): above 1080 and below it the
# PS4 Pro path; the larger GPU heap of native 4K also takes more than the Pro's 5568 MiB of direct
# memory.
def render_plan_of(config):
    sys.path.insert(0, str(PORT / 'scripts'))
    try:
        from patches import read_settings, render_plan
    finally:
        sys.path.pop(0)
    return render_plan(read_settings(Path(config)))


def apply_render_res(env, config):
    sys.path.insert(0, str(PORT / 'scripts'))
    try:
        from patches import read_settings, render_plan
    finally:
        sys.path.pop(0)
    plan = render_plan(read_settings(Path(config)))
    if plan is None:
        return
    env['BB_NEO'] = '1' if plan['pro'] else '0'
    if plan['heap'] and int(env.get('BB_DMEM_MB') or 0) < 7168:
        env['BB_DMEM_MB'] = '7168'
    path = 'PS4 Pro' if plan['pro'] else 'base PS4'
    say(f'Internal resolution: {plan["label"]} ({path} rendering path)')


# The server derives the player's ID from the nickname, so everyone left on a shared default
# name was the same player online (one wallet, and matchmaking evicting each other). These names
# are refused online; the launcher asks for a real one before it starts the game.
RESERVED_NICKS = {'ryupc', 'ryunet', 'player', 'jugador', 'guest'}
OFFLINE_NAME = 'Player'


def nick_ok(name):
    return (3 <= len(name) <= 16 and all(c.isascii() and (c.isalnum() or c in '_-') for c in name)
            and name.lower() not in RESERVED_NICKS)


def key_store():
    """Where this PC keeps its keys outside the game folders, so that a deleted, moved or
    re-downloaded folder keeps the nicknames: %APPDATA% on Windows (as the launcher keeps them),
    the XDG data folder (~/.local/share) on Linux. None when there is no such folder."""
    if os.name == 'nt':
        base = os.environ.get('APPDATA')
    else:
        base = os.environ.get('XDG_DATA_HOME') or (os.path.join(os.path.expanduser('~'), '.local', 'share')
                                                   if os.environ.get('HOME') else None)
    return Path(base) / 'CTR-NitroFueled-PC' if base else None


def valid_key(k):
    return len(k) == 64 and all(c in '0123456789abcdef' for c in k)


def known_keys():
    """Every key used on this PC (ryunet_online_keys.txt next to the backup; the Windows
    launcher's KnownKeys): a nickname this folder's key does not own may be another folder's."""
    store = key_store()
    try:
        lines = (store / 'ryunet_online_keys.txt').read_text(encoding='ascii', errors='replace').split()
    except (OSError, TypeError):
        return []
    keys = []
    for k in (line.strip().lower() for line in lines):
        if valid_key(k) and k not in keys:
            keys.append(k)
    return keys


def remember_key(key):
    store = key_store()
    if not store or not valid_key(key) or key in known_keys():
        return
    try:
        store.mkdir(parents=True, exist_ok=True)
        with open(store / 'ryunet_online_keys.txt', 'a', encoding='ascii') as f:
            f.write(key + '\n')
    except OSError:
        pass


def use_key(data, key):
    """This folder plays with another of this PC's keys from now on. True when saved."""
    try:
        (Path(data) / 'ryunet_online.key').write_text(key + '\n', encoding='ascii')
    except OSError:
        return False
    return True


def online_key(data):
    """This installation's secret: the server lets the first key to use a nickname keep it, so
    nobody else can play under it. Created once, next to run.py; the launcher reads the same
    file. Copying the folder to another PC keeps the nicknames; losing the file loses them."""
    path = Path(data) / 'ryunet_online.key'
    # A second copy outside the game folder (key_store).
    store = key_store()
    backup = store / 'ryunet_online.key' if store else None

    def read(p):
        try:
            k = p.read_text(encoding='ascii', errors='replace').strip().lower() if p else ''
        except OSError:
            return ''
        return k if valid_key(k) else ''

    key = read(path) or read(backup) or secrets.token_hex(32)
    if read(path) != key:
        try:
            path.write_text(key + '\n', encoding='ascii')
        except OSError as e:
            say(f'Could not save the online key ({e}): the nickname cannot be kept for this installation.')
            return ''
    if backup and not read(backup):
        try:
            backup.parent.mkdir(parents=True, exist_ok=True)
            backup.write_text(key + '\n', encoding='ascii')
        except OSError:
            pass
    remember_key(read(backup))
    remember_key(key)
    return key


def server_key(key, host):
    """What one server sees of the key: HMAC-SHA256(key, host). Each server gets a different
    value, so a server you try (or one an invite points to) cannot reuse it on another to take
    your nicknames. The launcher derives it the same way for its nickname check."""
    return hmac.new(bytes.fromhex(key), host.strip().lower().encode('utf-8'), hashlib.sha256).hexdigest()


def key_hash(skey):
    """What the server stores of a server key (nick_claims.go keyHash): the key of signatures."""
    return hashlib.sha256(skey.strip().lower().encode('ascii')).hexdigest()


# Seconds to add to this PC's clock to get the RyuNET server's. Signatures carry the time and the
# server takes them only within 5 minutes of its own clock: with the PC's clock off, every signed
# request was refused ("nickname signature out of time") and the nickname read as taken.
SERVER_CLOCK_OFFSET = 0
# Something on the way (an ISP's cache, an antivirus web shield) kept the server's answers and gave
# them back hours later: their Date made a right clock look hours off, and every signed request then
# went with the wrong time. Each request sends a fresh token (X-RyuNET-Clock) and the server answers
# with its time and that token (X-RyuNET-Time, its clock_stamp.go): a kept answer has an old one.
CLOCK = threading.local()  # .token: the last one this thread sent
SERVER_STAMPS = False      # the server sends X-RyuNET-Time: an answer without it is not its own


def clock_headers():
    """Headers for a request whose answer may set SERVER_CLOCK_OFFSET (note_server_clock)."""
    CLOCK.token = secrets.token_hex(8)
    return {'X-RyuNET-Clock': CLOCK.token, 'Cache-Control': 'no-cache'}


def note_server_clock(reply):
    """An answer of the RyuNET server (a urllib response or HTTPError) sets SERVER_CLOCK_OFFSET:
    its X-RyuNET-Time when it carries the token this thread just sent, else (a server older than
    that) its Date, unless a cache says it kept the answer (Age)."""
    global SERVER_CLOCK_OFFSET, SERVER_STAMPS
    import email.utils
    headers = getattr(reply, 'headers', None)
    if headers is None:
        return
    stamp = (headers.get('X-RyuNET-Time') or '').split()
    if stamp:
        SERVER_STAMPS = True
        if len(stamp) != 2 or not stamp[0].isdigit() or stamp[1] != getattr(CLOCK, 'token', None):
            return  # an answer kept on the way
        server = int(stamp[0])
    else:
        if SERVER_STAMPS or headers.get('Age'):
            return
        try:
            server = email.utils.parsedate_to_datetime(headers.get('Date', '')).timestamp()
        except (AttributeError, TypeError, ValueError, IndexError):
            return
    offset = round(server - time.time())
    # The header has whole seconds: a few seconds either way change nothing.
    SERVER_CLOCK_OFFSET = offset if abs(offset) > 5 else 0


def proof_header(khash, method, target):
    """X-RyuNET-Proof for one request (the server's nick_proof.go): signed with the key's hash, so
    the key never crosses the network, and good once, for a few minutes of the server's time."""
    ts, nonce = str(int(time.time()) + SERVER_CLOCK_OFFSET), secrets.token_hex(16)
    mac = hmac.new(khash.encode('ascii'), f'{ts}\n{nonce}\n{method}\n{target}'.encode('utf-8'),
                   hashlib.sha256).hexdigest()
    return f'1.{ts}.{nonce}.{mac}'


HTTPS_PORT = 443      # the server's nginx: /ryunet/ by HTTPS (tests point it elsewhere)
HTTPS_FAILED = set()  # servers whose HTTPS failed this run: their next requests go to the port


def machine_header():
    """This PC's anonymous fingerprint for the server's bans (scripts/machine_id.py), once."""
    global MACHINE
    if MACHINE is None:
        try:
            import machine_id
            MACHINE = machine_id.header()
        except Exception:  # never stops a launch
            MACHINE = ''
    return MACHINE


MACHINE = None


def bridge_port(port):
    """With the EOS bridge (eosbridge/: the launcher runs it when this network does not reach the
    server, and sets BB_ONLINE_BRIDGE="<udp>,<server port>:<local port>,..."): the port on
    127.0.0.1 that carries this server port there. None without it."""
    mapping = os.environ.get('BB_ONLINE_BRIDGE', '')
    if not mapping or not os.environ.get('BB_ONLINE_SERVER_IP'):
        return None
    for part in mapping.split(',')[1:]:
        server_port, _, local = part.partition(':')
        if server_port.strip() == str(port) and local.strip().isdigit():
            return int(local)
    return None


def ryunet_json(host, port, target, timeout, method='GET', headers=None, body=None, session=None):
    """A request to the RyuNET server and its JSON answer, with the reply (for its Date header).
    By HTTPS on the server's name first (443, where its nginx passes /ryunet/ to the server): a
    filter on the way cannot rewrite it, and networks that only let 80/443 out reach it. Then by
    plain HTTP on the port, by name and by address when something on the way answers for the
    name: an ISP's filter that redirects to its own warning page (Spectrum's Security Shield) or
    serves one. The game's own requests do the same (src/runtime_http.c). headers(method, target)
    makes each attempt's headers, so a signature is never sent twice. OSError when none works."""
    import ipaddress
    import socket
    import urllib.error
    import urllib.request

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            return None  # the server never redirects: a redirect comes from something on the way

    opener = urllib.request.build_opener(NoRedirect)
    def urls():
        local = bridge_port(port)
        if local:  # this network does not reach the server: the EOS bridge carries it
            yield f'http://127.0.0.1:{local}{target}', False
            return
        try:
            ipaddress.ip_address(host)
        except ValueError:  # a certificate can vouch for a name, never a bare address
            if host not in HTTPS_FAILED:
                yield f'https://{host}' + (f':{HTTPS_PORT}' if HTTPS_PORT != 443 else '') + target, True
        yield f'http://{host}:{port}{target}', False
        # Resolve the filter-bypass address only if the named endpoints did not work.
        try:
            address = socket.gethostbyname(host)
            if address != host:
                yield f'http://{address}:{port}{target}', False
        except OSError:
            pass
    failure = OSError(f'{host}: no answer')
    for url, secure in urls():
        name = url.split('/')[2]
        request = urllib.request.Request(url, method=method,
                                         data=body if body is not None else (b'' if method == 'POST' else None),
                                         headers={**(headers(method, target) if headers else {}),
                                                  **clock_headers(),
                                                  **({'X-RyuNET-Machine': machine_header()} if machine_header() else {}),
                                                  **({'Content-Type': 'application/json'} if body is not None else {})})
        try:
            response = session.open(request, timeout, opener) if session else opener.open(request, timeout=timeout)
            with response as reply:
                if 'json' not in (reply.headers.get('Content-Type') or ''):
                    failure = OSError(f'{name}: answered by something on the way, not the server')
                    HTTPS_FAILED.update([host] if secure else [])
                    continue
                return json.loads(reply.read(1 << 16)), reply  # the theme carries the item rules
        except urllib.error.HTTPError as error:
            if secure and 'json' not in (error.headers.get('Content-Type') or ''):
                HTTPS_FAILED.add(host)  # not the server's answer (no /ryunet/ there yet): the port
                failure = OSError(f'{name}: HTTPS answered {error.code}')
                continue
            if 300 <= error.code < 400:
                failure = OSError(f'{name}: redirected to {error.headers.get("Location")}')
                continue
            raise
        except OSError as error:  # no 443, a certificate that does not verify, a clock far off
            if not secure:
                raise
            HTTPS_FAILED.add(host)
            failure = OSError(f'{name}: HTTPS failed ({error})')
    raise failure


def claim_nick(host, port, name, khash, session=None):
    """Registers this installation for a free nickname, before the game logs in with it (the
    game's own requests are signed and cannot register). The key's hash crosses the network only
    here, once. Nothing happens when the server cannot be reached or is older."""
    import urllib.parse
    q = urllib.parse.urlencode({'name': name})
    try:
        status, _ = ryunet_json(host, port, f'/ryunet/nick?{q}', 4, session=session,
                                headers=lambda m, t: {'X-RyuNET-Proof': proof_header(khash, m, t)})
        if status.get('status') != 'free':
            return
        claimed, _ = ryunet_json(host, port, f'/ryunet/nick/claim?{q}', 4, method='POST', session=session,
                                 headers=lambda m, t: {'X-RyuNET-Proof': proof_header(khash, m, t),
                                                       'X-RyuNET-Key-Hash': khash})
        say(f'Nickname {name}: {claimed.get("status")} on {host}')
    except (OSError, ValueError) as error:
        say(f'Nickname {name}: could not register it on {host} ({error})')


def follow_rename(host, port, name, khash, session=None):
    """The nickname this installation plays as when it was changed on the site (the server answers
    "renamed" to its own installations only), else None."""
    import urllib.parse
    try:
        status, _ = ryunet_json(host, port, f'/ryunet/nick?{urllib.parse.urlencode({"name": name})}', 4, session=session,
                                headers=lambda m, t: {'X-RyuNET-Proof': proof_header(khash, m, t)})
    except (OSError, ValueError):
        return None
    new = status.get('renamed') if isinstance(status, dict) else None
    return new if isinstance(new, str) and nick_ok(new) else None


def set_ini_line(path, key, value):
    """key=value in an ini file: the line replaced, or added."""
    try:
        lines = Path(path).read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        lines = []
    out, done = [], False
    for line in lines:
        if line.partition('=')[0].strip() == key:
            if not done:
                out.append(f'{key}={value}')
                done = True
            continue
        out.append(line)
    if not done:
        out.append(f'{key}={value}')
    Path(path).write_text('\n'.join(out) + '\n', encoding='utf-8')


def server_settings(host, port, session=None):
    """What the server announces for every player (GET /ryunet/theme): the season theme, which
    scripts/patches.py loads in every level (THEME_VALUES), and the seconds an online race goes on
    after the first player finishes (end_race_patches). {} when it does not answer."""
    try:
        settings, reply = ryunet_json(host, port, '/ryunet/theme', 3, session=session)
    except (OSError, ValueError):
        return {}
    note_server_clock(reply)
    return settings if isinstance(settings, dict) else {}


RELEASES_API = 'https://api.github.com/repos/Ryu7w7/OCTR-NitroFueled-PC/releases/latest'


def parse_version(text):
    """(major, minor, patch, stage) as the launchers compare versions (launcher/appimage_update.py,
    launcher/windows/port_update.cpp): stage 0 a pre-release, 1 the release, 2 commits after it."""
    m = re.match(r'^\s*[vV]?(\d{1,5})\.(\d{1,5})(?:\.(\d{1,5}))?(.*?)\s*$', text or '')
    if not m:
        return None
    suffix = m.group(4)
    while True:
        for drop in ('-dirty', '-windows', '-linux'):
            if suffix.endswith(drop):
                suffix = suffix[:-len(drop)]
                break
        else:
            break
    stage = 1 if not suffix else 2 if re.fullmatch(r'-\d+-g[0-9a-fA-F]+', suffix) else 0
    return int(m.group(1)), int(m.group(2)), int(m.group(3) or 0), stage


def update_notice(env, version):
    """A release newer than this one: BB_UPDATE_AVAILABLE=<tag>, which the game shows as a notice
    (gpu/shim/bbport_overlay.cpp). Off with BB_UPDATE_CHECK=0; nothing when GitHub does not answer."""
    have = parse_version(version)
    if env.get('BB_UPDATE_CHECK') == '0' or not have:
        return
    import urllib.request
    try:
        request = urllib.request.Request(RELEASES_API, headers={'Accept': 'application/vnd.github+json',
                                                                'User-Agent': 'SacredFire-run.py'})
        with urllib.request.urlopen(request, timeout=3) as reply:
            tag = json.loads(reply.read(1 << 20)).get('tag_name', '')
    except (OSError, ValueError, AttributeError):
        return
    latest = parse_version(tag)
    if latest and latest > have:
        env['BB_UPDATE_AVAILABLE'] = tag
        say(f'A newer release is available: {tag} (update from the launcher)')


def setup_online(env, config, data):
    # The item-box rules come only from the server (below): items are drawn on each player's PC,
    # so a local setting would let one player pick their own odds.
    env.pop('BB_ITEM_RULES', None)
    ini_values = {}
    try:
        for l in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
            k, sep, v = l.partition('=')
            if sep and not k.startswith('#'):
                ini_values[k.strip()] = v.strip()
    except OSError:
        pass

    # Ignore the retired launcher toggle; the native selector owns this session's mode.
    env.pop('BB_SPEED_MODE', None)
    env['BB_PUBLIC_ITEMLESS'] = '0'

    name = env.get('BB_ONLINE_NAME', ini_values.get('online_name', ini_values.get('user_name', ''))).strip()
    online = env.get('BB_ONLINE', ini_values.get('online', '0'))
    if online == '1' and not nick_ok(name):
        fail('Online play needs your own nickname: 3 to 16 letters, digits, _ or -, and not a default '
             'name like RyuPC. Set it in the launcher (Online tab) or as online_name= in bbport.ini.\n'
             'El juego online necesita tu propio apodo: de 3 a 16 letras, números, _ o -, y no un nombre '
             'por defecto como RyuPC. Ponlo en el launcher (pestaña Online) o como online_name= en bbport.ini.')
    env['BB_USER_NAME'] = name or OFFLINE_NAME

    if online != '1':
        return

    env['BB_ONLINE'] = '1'
    env['BB_NET_TRACE'] = '1'
    server = env.get('BB_ONLINE_SERVER', ini_values.get('online_server', 'octr.ryu7w7.xyz'))
    if not server or server.strip() == '':
        server = 'octr.ryu7w7.xyz'
    host = server
    auth_port = env.get('BB_ONLINE_HTTP_PORT', ini_values.get('online_http_port', '8080'))
    if ':' in server and not server.startswith('['):
        h, _, p = server.partition(':')
        if p.isdigit():
            host = h
            auth_port = p

    env['BB_ONLINE_SERVER'] = host
    env['BB_ONLINE_HTTP_PORT'] = auth_port  # the runtime's own requests (Friends tab)
    env['BB_ONLINE_NAME'] = name
    # The server's announcements (used below); its answer also tells its clock, before anything
    # is signed.
    with ryunet_session.Session() as session:
        announced = startup_call('online.server_settings', server_settings, host, auth_port, session=session)
        if announced.get('itemless_matchmaking') == 1:
            env['BB_PUBLIC_ITEMLESS'] = '1'
        if SERVER_CLOCK_OFFSET:
            env['BB_ONLINE_CLOCK_OFFSET'] = str(SERVER_CLOCK_OFFSET)
            if abs(SERVER_CLOCK_OFFSET) > 60:
                minutes = round(abs(SERVER_CLOCK_OFFSET) / 60)
                say(f"This PC's clock is {minutes} min {'behind' if SERVER_CLOCK_OFFSET > 0 else 'ahead of'} "
                    "the server's: online uses the server's time, but sync it (Windows: Settings > Time > Sync now).")
        else:
            env.pop('BB_ONLINE_CLOCK_OFFSET', None)
        key = online_key(data)
        env.pop('BB_ONLINE_KEY', None)  # the key is not sent any more: requests are signed with its hash
        if key:
            env['BB_ONLINE_KEYHASH'] = key_hash(server_key(key, host))
            # A nickname changed on the site: this installation plays as the new one from now on.
            renamed = startup_call('online.follow_rename', follow_rename, host, auth_port, name, env['BB_ONLINE_KEYHASH'], session=session)
            if renamed and renamed != name:
                say(f'Nickname: {name} is now {renamed} (changed on the site)')
                name = renamed
                env['BB_ONLINE_NAME'] = env['BB_USER_NAME'] = name
                for key_name in ('online_name', 'user_name'):
                    try:
                        set_ini_line(config, key_name, name)
                    except OSError as error:
                        say(f'Nickname: could not save the new one in bbport.ini ({error})')
            startup_call('online.claim_nick', claim_nick, host, auth_port, name, env['BB_ONLINE_KEYHASH'], session=session)
    # online_inventory=save: keep the Pit Stop items of this save (a "100%" save) online
    # instead of the ones bought on the server, which are tied to the nickname.
    if env.get('BB_ONLINE_INVENTORY', ini_values.get('online_inventory', 'server')).strip().lower() == 'save':
        env['BB_ONLINE_INVENTORY'] = 'save'
    else:
        env.pop('BB_ONLINE_INVENTORY', None)
    account_id = env.get('BB_ONLINE_ACCOUNT_ID', ini_values.get('online_account_id', ''))
    if account_id:
        env['BB_ONLINE_ACCOUNT_ID'] = account_id
    online_port = env.get('BB_ONLINE_PORT', ini_values.get('online_port', ''))
    if online_port:
        env['BB_ONLINE_PORT'] = online_port
    friends = env.get('BB_ONLINE_FRIENDS', ini_values.get('online_friends', ''))
    if friends:
        env['BB_ONLINE_FRIENDS'] = friends

    overrides_file = data / 'host_overrides.json'
    overrides = {
        "lavender-ps4-auth3.prod.demonware.net": f"http://{host}:{auth_port}",
        "lavender-ps4-auth3.cert.demonware.net": f"http://{host}:{auth_port}",
        "lavender-ps4-auth3.dev.demonware.net":  f"http://{host}:{auth_port}",
        "prod.umbrella.demonware.net": f"http://{host}:{auth_port}",
        "cert.umbrella.demonware.net": f"http://{host}:{auth_port}",
        "dev.umbrella.demonware.net":  f"http://{host}:{auth_port}",
        "prod.uno.demonware.net": f"http://{host}:{auth_port}",
        "cert.uno.demonware.net": f"http://{host}:{auth_port}",
        "dev.uno.demonware.net":  f"http://{host}:{auth_port}",
        "lavender-ps4-lobby.prod.demonware.net": host,
        "lavender-ps4-lobby.cert.demonware.net": host,
        "lavender-ps4-lobby.dev.demonware.net":  host,
        "stun.us.demonware.net": host,
        "stun.eu.demonware.net": host,
        "stun.jp.demonware.net": host,
        "stun.au.demonware.net": host,
        # Telemetry / CDN hosts the game contacts directly over HTTPS: keep
        # them local (the server answers 404, which the game tolerates) and
        # never touch the real servers.
        "pipes-prod-glutton.public.aws.demonware.net": f"http://{host}:{auth_port}",
        "objectstore.prod.demonware.net": f"http://{host}:{auth_port}",
        # Demonware friends API (eboot rodata ~0x1bef504, base http://friends.dev...):
        # list / invitation / pending / pair. Routed to the server's friends
        # handler; without this the game gets EHOSTUNREACH and shows no friends.
        "friends.dev.demonware.net": f"http://{host}:{auth_port}",
    }
    overrides_file.write_text(json.dumps(overrides, indent=2) + '\n', encoding='utf-8')
    say(f'RyuNET Online active -> server {host}:{auth_port}, user {name}')
    # Every player loads the theme the server announces (bbport.ini theme= only applies offline;
    # christmas: the title screen's, scripts/season_art.py), and the server's race-end countdown.
    theme = announced.get('theme')
    if not env.get('BB_THEME') and theme in ('default', 'halloween', 'golden', 'christmas'):
        env['BB_THEME'] = theme
        if theme != 'default':
            say(f'Season theme from the server: {theme}')
    end_race = announced.get('end_race_timeout')
    if not env.get('BB_END_RACE_TIMEOUT') and isinstance(end_race, (int, float)) and 10 <= end_race <= 120:
        env['BB_END_RACE_TIMEOUT'] = str(end_race)
        say(f'Online race end countdown from the server: {end_race} s')
    # The item-box rules (odds, cooldowns) the server's administrators chose, for every player
    # (scripts/item_rules.py); none: the game's own.
    items = announced.get('items')
    rules_file = data / 'out' / 'item_rules' / 'server.json'
    # The rules' id (server client_gate.go): the game sends it at login once they are applied
    # (BB_ITEM_RULES_ID, set where they are); "none": the server has no rules to apply.
    items_id = announced.get('items_id')
    env['BB_ITEM_RULES_SERVER_ID'] = items_id if isinstance(items_id, str) and items_id.isalnum() else ''
    if not (isinstance(items, dict) and items) and env['BB_ITEM_RULES_SERVER_ID']:
        env['BB_ITEM_RULES_SERVER_ID'] = 'none'
    if isinstance(items, dict) and items:
        rules_file.parent.mkdir(parents=True, exist_ok=True)
        rules_file.write_text(json.dumps(items, sort_keys=True), encoding='utf-8')
        env['BB_ITEM_RULES'] = str(rules_file)
        say(f'Item rules from the server: {items.get("name") or "custom"}')


def keep_corrupt_free_log():
    """The game goes on past a corrupt free (probe.c) and the next start writes over this log, the
    only record of what the block held: a copy stays beside it, the last one."""
    if LOG and PERF.corrupt_free:
        LOG.flush()
        log = Path(LOG.name)
        kept = log.with_name(f'{log.stem}_corrupt_free.log')
        shutil.copyfile(log, kept)
        say(f'A corrupt memory block was caught: the log is kept in {kept.name}')


def cloud_sync_game(env, config, progress, title_id, after):
    """The account's trophies (before and after the game) and save (after) on the server
    (scripts/cloud_sync.py). Online only, with a signed key; bbport.ini cloud_sync=0 turns it off.
    Never stops anything."""
    host, port = env.get('BB_ONLINE_SERVER'), env.get('BB_ONLINE_HTTP_PORT', '8080')
    khash, name = env.get('BB_ONLINE_KEYHASH'), env.get('BB_ONLINE_NAME')
    if env.get('BB_ONLINE') != '1' or not (host and khash and name) or title_id not in ('CUSA13795', 'CUSA14876'):
        return
    if ini_has(config, 'cloud_sync=0'):
        return

    def call(method, target, body=None, timeout=10):
        answer, _ = ryunet_json(host, port, target, timeout, method=method, body=body,
                                headers=lambda m, t: {'X-RyuNET-Proof': proof_header(khash, m, t)})
        return answer

    import cloud_sync
    try:
        cloud_sync.sync_trophies(call, name, progress, title_id, say=say)
    except (OSError, ValueError) as error:
        say(f'Trophies: not synced with the server ({error})')
    if after:
        try:
            cloud_sync.upload_save(call, name, progress, title_id, say=say)
        except (OSError, ValueError) as error:
            say(f'Cloud save: not uploaded ({error})')


def finish_perf_report(env, config, data, version, title_id, code):
    """The anonymous performance report of this launch: kept beside the logs (perf/), sent to the
    RyuNET server only when the player said yes (bbport.ini perf_reports=1, asked once by the
    launcher). Never stops anything: a failure is one line in the log."""
    if not PERF:
        return
    try:
        keep_corrupt_free_log()
        report = PERF.report({
            'octr': version[:40],
            'edition': title_id,
            'renderer': ('d3d11' if env.get('BB_D3D11') == '1' else
                         'native' if env.get('BB_VK_NATIVE_ONLY') == '1' else 'shadps4'),
            'online': env.get('BB_ONLINE') == '1',
            'settings': perf_report.settings_of(config),
            'exit': {'code': code, 'meaning': perf_report.exit_meaning(code)},
            'mods': perf_report.mods_in_use(data, env),
        })
        if not report.get('overall') and not report.get('fault'):
            return  # nothing drawn and no crash (a start stopped early): nothing to say
        path = perf_report.keep(report, data / 'perf')
        if perf_report.consent(config) != '1':
            return
        host = env.get('BB_ONLINE_SERVER') or perf_report.settings_value(config, 'online_server') or 'octr.ryu7w7.xyz'
        port = env.get('BB_ONLINE_HTTP_PORT', '8080')
        if ':' in host and not host.startswith('['):
            host, _, p = host.partition(':')
            port = p if p.isdigit() else port
        ryunet_json(host, port, '/ryunet/perf', 4, method='POST',
                    body=json.dumps(report, separators=(',', ':')).encode())
        say(f'Performance report sent (anonymous; a copy: {path.name} in perf/)')
    except Exception as error:  # no network, an old server: the report stays local
        say(f'Performance report not sent ({type(error).__name__}); a copy stays in perf/')


def watch_vulkan_start(process, env):
    """Waits for the game; True when Vulkan never finished starting (no GPU opened
    VULKAN_START_SECONDS after the window) and the game was closed to start again without the
    overlay layers. Once without them, a hang is only reported."""
    reported = False
    while True:
        try:
            process.wait(timeout=1)
            return False
        except subprocess.TimeoutExpired:
            pass
        opened = VULKAN_START['window']
        if not opened or VULKAN_START['ready'] or time.time() - opened < VULKAN_START_SECONDS:
            continue
        layers = VULKAN_START['layers'] or 'not listed'
        if not env.get('VK_LOADER_LAYERS_DISABLE'):
            say(f'--- Vulkan did not finish starting in {VULKAN_START_SECONDS} s (black window). Usually a '
                f'program that hooks into games (an overlay or recorder: OBS, RivaTuner/Afterburner, Overwolf, '
                f'Discord, ...) or a broken driver. Starting again without the overlay layers '
                f'(installed: {layers}).')
            process.kill()
            process.wait()
            return True
        if not reported:
            reported = True
            say('--- Vulkan still did not start without the overlay layers: the graphics driver is the likely '
                'cause. Reinstall the latest driver for your GPU (on a laptop, the one from its maker too), '
                'and close overlay and recording programs.')


def pump(stream):
    for raw in iter(stream.readline, b''):
        # The game writes CRLF; the log is a text file that adds its own CR.
        emit(raw.decode('utf-8', errors='replace').replace('\r\n', '\n'))


def main():
    args = sys.argv[1:]
    restarted = len(args) >= 2 and args[0] == '--after'
    if restarted:
        wait_for(int(args[1]))
        args = args[2:]
    os.chdir(PORT)  # as run.sh: relative paths are from the port
    try:
        with launch_lock.session(Path(os.environ.get('BB_DATA_DIR', PORT))):
            return launch(args, restarted)
    except (OSError, ValueError, RuntimeError, boot_cache.prepare.GameCheckError) as error:
        fail('Launch failed: ' + str(error))


def remove_obsolete_files():
    """Files an older package left that a newer one no longer carries (an update copies the new
    package's files over the old ones and removes none): v0.8.5 builds Zydis into sacredfire.exe
    (no bin/libzydis.dll) and ships iced-x86's modules as bytecode only, so its old sources go (Python
    would keep importing a .py beside its .pyc, and its Code.py is what antivirus heuristics flagged)."""
    stale = [PORT / 'bin' / 'libzydis.dll']
    iced = PORT / 'python' / 'Lib' / 'site-packages' / 'iced_x86'
    if (iced / 'Code.pyc').is_file():
        stale += [p for p in iced.glob('*.py') if p.with_suffix('.pyc').is_file()]
        stale += list(iced.glob('*.pyi'))
    # v0.8.5's tidier folder: the third-party list in licenses\, the Steam artwork in art\, no docs\
    # (only where the new places are there, so a source checkout keeps its own).
    stale_folders = []
    if (PORT / 'licenses' / 'THIRD-PARTY.txt').is_file():  # a v0.8.5+ package, not a source checkout
        stale += [PORT / 'THIRD-PARTY.txt', PORT / 'tools' / 'merge_update.py']
        stale_folders.append(PORT / 'docs')
    if (PORT / 'art' / 'steamgrid').is_dir():
        stale_folders.append(PORT / 'steamgrid')
    removed = 0
    for path in stale:
        try:
            path.unlink()
            removed += 1
        except FileNotFoundError:
            pass
        except OSError as error:
            say(f'Could not remove the old {path.name}: {error}')
    for folder in stale_folders:
        if folder.is_dir():
            shutil.rmtree(folder, ignore_errors=True)
            removed += 1
    if removed:
        say(f'Removed {removed} file(s) an older version left behind')


def launch(args, restarted=False):
    env = os.environ
    remove_obsolete_files()
    data = Path(env.get('BB_DATA_DIR', PORT))
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    env.setdefault('BB_CONFIG', str(data / 'bbport.ini'))
    config = env['BB_CONFIG']
    # A portable package ships bbport.default.ini, so a new version unzipped over an old one keeps
    # the player's bbport.ini.
    if not Path(config).exists() and (PORT / 'bbport.default.ini').is_file():
        shutil.copyfile(PORT / 'bbport.default.ini', config)

    if 'BB_LANGUAGE' not in env:
        try:
            for l in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
                k, _, v = l.partition('=')
                if k.strip() in ('game_language', 'language') and v.strip():
                    env['BB_LANGUAGE'] = v.strip()
                    break
        except OSError:
            pass

    if 'BB_ASYNC_SUBMIT' not in env:
        v = ini_value(config, 'async_submit')
        if v is not None:
            env['BB_ASYNC_SUBMIT'] = v

    setup_online(env, config, data)

    probe = Path(env['BB_PROBE']) if env.get('BB_PROBE') else find_executable('sacredfire.exe')
    if not probe or not probe.is_file():
        fail('sacredfire.exe not found: build it with build.sh in the MSYS2 CLANG64 shell.')
    # A source build runs with the MSYS2 CLANG64 DLLs (SDL3, FFmpeg, ...).
    if not (probe.parent / 'SDL3.dll').is_file():
        clang64 = Path(env.get('MSYS2_ROOT', r'C:\msys64')) / 'clang64' / 'bin'
        if clang64.is_dir():
            env['PATH'] = f'{clang64}{os.pathsep}{env.get("PATH", "")}'
    # Instruction sets this CPU lacks (MOVBE, BMI1, LZCNT): the ISA rewrite covers them; exit 20
    # is a set nothing covers.
    try:
        features = subprocess.run([str(probe), '--cpu-features'], stdin=subprocess.DEVNULL,
                                  stdout=subprocess.PIPE, text=True, creationflags=no_console())
    except PermissionError as error:
        blocked(probe, error)
    if features.returncode == 20:
        print(features.stdout, end='', flush=True)
        sys.exit(20)
    missing = parse_cpu_features(features.stdout)

    # The output of every launch also goes to last_run.log (an in-game restart appends to it);
    # BB_SAVE_LOG=1 (with frame statistics) writes logs/<time>.log instead.
    global LOG
    if env.get('BB_SAVE_LOG') == '1':
        logs = data / 'logs'
        logs.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime('%Y%m%d_%H%M%S')
        env['BB_FRAME_STATS'] = '1'
        env.setdefault('BB_FRAME_LOG', str(logs / f'{stamp}.frames.csv'))
        env.setdefault('BB_READBACK_LOG', str(logs / f'{stamp}.readbacks.csv'))
        LOG = open(logs / f'{stamp}.log', 'w', encoding='utf-8', errors='replace')
        LOG_LIMIT.SAME = LOG_LIMIT.MAX = float('inf')  # asked for everything
        say(f'Log: {logs / (stamp + ".log")}')
    else:
        user_name = env.get('BB_USER_NAME', env.get('BB_ONLINE_NAME', ''))
        log_name = f'last_run_{user_name}.log' if user_name and user_name != OFFLINE_NAME else 'last_run.log'
        try:
            LOG = open(data / log_name, 'a' if restarted else 'w', encoding='utf-8', errors='replace')
        except OSError:
            LOG = open(data / f'last_run_{os.getpid()}.log', 'w', encoding='utf-8', errors='replace')
        LOG_LIMIT.written = LOG.tell()
    say(f'--- SacredFire launch {time.strftime("%Y-%m-%d %H:%M:%S")}{" (restart)" if restarted else ""}')

    # BB_GAME_DIR: the merged CTR dump (update 1.21 copied over the base game).
    game_dir_env = env.get('BB_GAME_DIR')
    if game_dir_env:
        game = Path(game_dir_env)
    elif (PORT / f'{TITLE_ID}-1.21' / 'eboot.bin').is_file():
        game = PORT / f'{TITLE_ID}-1.21'
    elif (PORT / TITLE_ID / 'eboot.bin').is_file():
        game = PORT / TITLE_ID
    else:
        game = Path(env.get('BB_GAME_DIR', PORT / 'orig' / TITLE_ID))
    if not (game / 'eboot.bin').is_file():
        if (game / 'Image0' / 'eboot.bin').is_file():
            fail(f'{game} has the PS4 PKG Tool layout (Image0 and Sc0): prepare it in the launcher first.')
        fail(f'No eboot.bin in {game} (set BB_GAME_DIR to your {TITLE_ID} folder).')
    # The game check compares the executable image with the supported one (CTR NF 1.21).
    check = startup_call('game_check', subprocess.run, [sys.executable, '-X', 'utf8', str(PORT / 'scripts' / 'game_check.py'), str(game)],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, encoding='utf-8', errors='replace', creationflags=no_console())
    if check.returncode:
        fail(check.stdout.strip() or 'Game check failed.', 2)
    env['BB_GAME_PROFILE'] = check.stdout.strip() or 'ctr'
    # The game's own title ID and version (the EU edition, CUSA14876 1.00, has its own patches).
    info = run_python('scripts/game_check.py', '--info', game, capture=True).stdout.split()
    title_id, app_version = (info + [TITLE_ID, APP_VERSION][len(info):])[:2]
    say(f'Game profile: {env["BB_GAME_PROFILE"]}')
    if 'BB_LANGUAGE' in env:
        say(f'Game language ID: {env["BB_LANGUAGE"]}')

    import mod_packages
    try:
        game, config = startup_call('mod_packages.prepare', mod_packages.prepare, PORT, data, env, game, config,log=say)
    except (ValueError, RuntimeError, OSError) as error:
        fail('Mod launch blocked: ' + str(error) + '\nChoose compatible settings or disable the indicated mod in Mods.')
    original_game = game.resolve()
    import mod_file_paths
    mod_file_paths.configure(env)
    # The Options menu's Graphics tab and the main menu's Quit Game (scripts/graphics_tab.py): an
    # extra archive of relabelled menus, served by the runtime as archives/update3.pak (no copy of
    # the game folder). USA/EU 1.21: the engine validates the region's hooks
    # (gpu/shim/bbport_graphics_tab.cpp), and without them the runtime leaves the menus alone.
    env.pop('BB_GRAPHICS_TAB_ARCHIVE', None)
    if title_id in ('CUSA13795', 'CUSA14876') and app_version == '01.21':
        result = run_python('scripts/graphics_tab.py', game, '--out', out, '--settings', config,
                            capture=True, check=False)
        for line in result.stdout.splitlines():
            if line.startswith('archives/update3.pak='):
                env['BB_GRAPHICS_TAB_ARCHIVE'] = line.partition('=')[2].strip()
            elif line.strip():
                say(line.strip())
        if result.returncode:
            say(f'Graphics tab: off (scripts/graphics_tab.py stopped with code {result.returncode})')
    game = Path(run_python('scripts/mods.py', game, '--out', out,
                           '--mods-dir', env.get('BB_MODS_DIR', data / 'mods'),
                           '--config', env.get('BB_MODS_CONFIG', data / 'mods.json'),
                           '--enabled', env.get('BB_MODS_ENABLED', '1'), capture=True).stdout.strip())
    mod_view = game if game.resolve() != original_game else None
    try:
        try:
            boot = startup_call('boot_cache.ensure', boot_cache.ensure, game, out, log=say)
        except (StopIteration, KeyError, IndexError) as error:
            fail('Boot preparation failed: ' + str(error))
        # A failed rewrite prints its reason and the game starts without the table.
        isa_args = []
        if isa_rewrite_wanted(missing, env):
            isa_out = boot / 'isa'
            result = run_python('scripts/isa_rewrite.py', boot / 'boot-linked.bin', '--out', isa_out,
                                capture=True, check=False)
            emit(result.stdout)
            if result.returncode == 0:
                isa_args = ['--isa-rewrite', str(isa_out / 'isa-rewrite.bin')]
        # Symbol names for the eboot's host-side imports (shadPS4's aerolib list, fetched once).
        run_python('tools/gen_import_names.py', '--out', out, '--boot-dir', boot)
        # DLC (optional): the player's add-on packages in dlc\ (BB_DLC_DIR), reported installed.
        run_python('scripts/content_profile.py', game, '--out', out, '--sku', env.get('BB_CONTENT_SKU', 'full'),
                   '--dlc-dir', env.get('BB_DLC_DIR') or data / 'dlc')  # empty: dlc\, as run.sh
        # The shader recipe (shaders/recipe.zip): prepared for this GPU at the first start, from the
        # game's own shaders, so new tracks and racers do not stop the game to compile them.
        # BB_SHADER_RECIPE=0 leaves it out; another path uses that recipe.
        recipe_setting = env.get('BB_SHADER_RECIPE', '')
        recipe = Path(recipe_setting) if recipe_setting not in ('', '0') else PORT / 'shaders' / 'recipe.zip'
        if recipe_setting != '0' and recipe.is_file():
            run_python('scripts/gcn_shaders.py', game, '--out', out)
            env['BB_SHADER_RECIPE'] = str(recipe)
            env['BB_GCN_SHADERS'] = str(out / 'gcn_shaders.bin')
        else:
            env.pop('BB_SHADER_RECIPE', None)
            env.pop('BB_GCN_SHADERS', None)
        # Every race track in the online playlist, at the same weight (scripts/online_playlist.py),
        # always: public rooms draw from the same pool for everyone. The game reads it through an
        # archive patch; the archives stay as they are. When it cannot be built, the game's own.
        env.pop('BB_ARCHIVE_PATCHES', None)
        run_python('scripts/online_playlist.py', game, '--out', out)
        playlist_patch = out / 'online_playlist.bbpatch'
        base_patches = {}
        if playlist_patch.is_file():
            base_patches[archive_patch.read(playlist_patch)[1]] = playlist_patch
        # Xbox button prompts (bbport.ini button_prompts, scripts/button_prompts.py): the fonts'
        # button image through archive patches, each keeping the playlist's for the same archive.
        try:
            base_patches.update(startup_call('button_prompts.prepare', button_prompts.prepare, game, out, config, env, base_patches))
        except (OSError, ValueError) as error:
            say(f'Button prompts: {error}; PlayStation buttons')
        # A seasonal title screen (bbport.ini title_theme, scripts/season_art.py): the start screen's
        # background through archive patches, each keeping the ones before for the same archive.
        try:
            base_patches.update(startup_call('season_art.prepare', season_art.prepare, game, out, config, env, base_patches))
        except (OSError, ValueError) as error:
            say(f"Title theme: {error}; the game's own")
        # Online, the nickname bottom left of the menus' button bar, as the Xbox One game shows the
        # gamertag (scripts/signed_in_name.py): one menu script's console switch, by archive patch.
        try:
            base_patches.update(startup_call('signed_in_name.prepare', signed_in_name.prepare, game, out, config, env, base_patches))
        except (OSError, ValueError) as error:
            say(f'Nickname in the menus: {error}; not shown')
        # The port's own CTR TV logo (bbport.ini ctr_tv_logo, scripts/ctr_tv_logo.py), by archive patch.
        try:
            base_patches.update(startup_call('ctr_tv_logo.prepare', ctr_tv_logo.prepare, game, out, config, env, base_patches))
        except (OSError, ValueError) as error:
            say(f"CTR TV logo: {error}; the game's own")
        # Discord presence (the launchers): the game writes what the player does to BB_PRESENCE_FILE
        # (src/runtime_presence.c), and the tracks' own names go beside it (scripts/track_names.py).
        presence = Path(env.setdefault('BB_PRESENCE_FILE', str((out / 'presence' / 'presence.txt').resolve())))
        try:
            startup_call('track_names.prepare', track_names.prepare, game, presence.parent)
        except (OSError, ValueError, KeyError) as error:
            say(f'Track names for Discord: {error}')
        env.setdefault('BB_NEO', '0')
        # The aspect ratio (bbport.ini aspect_ratio, scripts/aspect_ratio.py): the render
        # path and the patches read its settings (the player's, with the ones it was made for),
        # and it adds its archive patches, executable patch and HUD scale. Read again at a restart.
        layout_saved = {}

        def apply_layout():
            env.pop('BB_SPEED_MODE', None)
            for key, value in layout_saved.items():
                if value is None:
                    env.pop(key, None)
                else:
                    env[key] = value
            layout_saved.clear()
            layout = startup_call('aspect_ratio.prepare', aspect_ratio.prepare, game, out, config, env, title_id, app_version, base_patches, boot_dir=boot)
            if mod_packages.ACTIVE and aspect_ratio.selection(aspect_ratio.read_ini(config)) and not layout:
                raise ValueError('The selected mods cannot use this aspect ratio; see the preparation message above.')
            if layout:
                layout_saved.update({key: env.get(key) for key in layout['env']})
                env.update(layout['env'])
            archives = aspect_ratio.archive_list(base_patches, layout)
            if env.get('BB_MOD_ARCHIVE_PLAN'):
                from mod_overlays import for_launch
                builtins = {archive_patch.read(p)[1]: Path(p) for p in archives.split('|') if p} if archives else {}
                composed = for_launch(game, out / 'mod-overlays', env['BB_MOD_ARCHIVE_PLAN'], builtins)
                archives = '|'.join(str(p) for p in composed.values())
            # Item-box rules (scripts/item_rules.py): the server's, which setup_online saved, last:
            # over every systemData.igz copy the game loads, a mod's included.
            env.pop('BB_ITEM_RULES_ID', None)
            try:
                rules_path = env.get('BB_ITEM_RULES')
                rules = json.loads(Path(rules_path).read_text(encoding='utf-8')) if rules_path else None
                before = archives
                archives = startup_call('item_rules.with_rules', item_rules.with_rules, game, out, rules, archives, log=say)
                server_id = env.get('BB_ITEM_RULES_SERVER_ID', '')
                if server_id == 'none' or (server_id and rules and archives != before):
                    env['BB_ITEM_RULES_ID'] = server_id
            except (OSError, ValueError, KeyError, TypeError) as error:
                say(f"Item rules: {error}; the game's own odds")
            env.pop('BB_PRIVATE_ARCHIVE_PATCHES', None)
            env.pop('BB_ONLINE_MODE_HOOK', None)
            # Private Itemless AI follows the game's own Time Trial ghosts (scripts/tt_reference.py,
            # src/runtime_race_options.c); without them it keeps the native route guidance.
            env.pop('BB_TT_REFERENCE', None)
            if title_id in ('CUSA13795', 'CUSA14876') and app_version == '01.21':
                tt = run_python('scripts/tt_reference.py', game, '--out', out / 'tt-reference',
                                capture=True, check=False)
                if tt.returncode == 0 and (out / 'tt-reference/reference.bin').is_file():
                    env['BB_TT_REFERENCE'] = str((out / 'tt-reference/reference.bin').resolve())
                else:
                    say('Time Trial references not prepared: the AI keeps its native route guidance')
            if env.get('BB_ONLINE') == '1' or env.get('BB_SPEED_SOLO_TEST') == '1':
                if title_id in ('CUSA13795', 'CUSA14876') and app_version == '01.21':
                    base = [Path(p) for p in archives.split('|') if p]
                    if env.get('BB_ONLINE') == '1':
                        base, env['BB_ONLINE_MODE_HOOK'] = startup_call('online_mode.prepare', online_mode.prepare,
                            game, out / 'online-mode', boot / 'eboot.elf', base)
                        env['BB_MOD_FILE_MAP'] = str((out / 'online-mode/cup-files.map').resolve())
                        env.setdefault('BB_MOD_SOURCE_GAME', str(game))
                        env['BB_KNOWN_HEROES'] = startup_call('online_mode.known_heroes', online_mode.known_heroes, game)
                    try:
                        normal_patches, speed_patches, track_count = startup_call('speed_mode.prepare', speed_mode.prepare,
                            game, out / 'speed-mode', base)
                    except (ValueError, OSError, KeyError, IndexError) as error:
                        # a damaged game file stops Speed Mode only: online plays as before
                        say(f'ITEMLESS not available: {error}')
                        archives = '|'.join(map(str, base))
                    else:
                        archives = '|'.join(map(str, normal_patches))
                        env['BB_PRIVATE_ARCHIVE_PATCHES'] = '|'.join(map(str, speed_patches))
                        say(f'ITEMLESS: HUD prepared; native Time Trial track rules cover {track_count} tracks')
            if archives:
                env['BB_ARCHIVE_PATCHES'] = archives
            else:
                env.pop('BB_ARCHIVE_PATCHES', None)
            return (layout['config'], layout['patches_dir']) if layout else (config, None)

        settings, layout_patches = apply_layout()
        if mod_packages.ACTIVE:
            startup_call('mod_packages.preflight', mod_packages.preflight, Path(env['BB_PATCHES_DIR']).parent, settings, Path(env['BB_PATCHES_CONFIG']),
                                   [layout_patches] if layout_patches else [], title_id, app_version, game=game)
        apply_render_res(env, settings)
        say(f'PS4 Pro mode: {env["BB_NEO"]} (BB_NEO=1 turns it on)')
        user_dir = Path(env.get('BB_USER_DIR', data / 'user')).resolve()
        if env.get('BB_CLEAR_SHADER_CACHE', '0') == '1':
            shutil.rmtree(user_dir / 'cache' / title_id, ignore_errors=True)
            say('Shader cache: cleared for this launch (BB_CLEAR_SHADER_CACHE=1)')
        env.setdefault('BB_PREUPLOAD', '1')
        # The PC memory model (dma-buf guest memory) is Linux only: the 0.3 model on Windows.
        env['BB_GUEST_IN_PLACE'] = '0'
        env.setdefault('BB_UFFD', '0')
        env.setdefault('BB_COPY_GPU_BUFFERS', '1')
        env.setdefault('BB_GPU_WRITE_TWINS', '1')
        env.setdefault('BB_GPU_WRITE_TWINS_MAX', '65536')
        fps = env.get('BB_FPS', '30')
        user_vblank = env.get('BB_VBLANK_HZ')

        version = ''
        for candidate in (probe.parent / 'VERSION', PORT / 'VERSION'):
            if candidate.is_file():
                version = candidate.read_text().strip()
                break
        say(f'SacredFire {version or "unknown version"}')
        update_notice(env, version)
        env['BB_OCTR_VERSION'] = version.removeprefix('v').split('-')[0] if version else ''  # the game's login says it
        env['BB_MACHINE_ID'] = machine_header()  # this PC's anonymous fingerprint, for bans
        # The graphics engine (bbport.ini renderer): the native Vulkan renderer is experimental; its
        # shaders are built from this game the first time (scripts/native_shaders.py, ~5 minutes).
        # Anything that stops it starts the game with shadPS4's Vulkan, as before.
        if ini_value(config, 'renderer') == 'd3d11':
            env['BB_D3D11'] = '1'
            shaders = out / 'd3d11-shaders' / 'hlsl'
            prepare_d3d11_shaders(game, out / 'd3d11-shaders')  # quick when ready
            if (shaders / 'shaders.idx').is_file():
                env.setdefault('BB_D3D11_SHADERS', str(shaders))
                env.setdefault('BB_D3D11_ORDER', str(D3D11_ORDER))  # what the game compiles first
                say('Direct3D 11 (experimental)')
            else:
                say('Direct3D 11 (experimental): no shaders in out/d3d11-shaders, so no picture of the game')
        if (ini_value(config, 'renderer') or 'shadps4') == 'native' and 'BB_NATIVE_VK' not in env:
            if not native_shaders.supported(title_id, app_version):
                say(f'Native Vulkan: only the USA or EU 1.21 game (CUSA13795, CUSA14876) for now; this is {title_id} '
                    f'{app_version}, so the game starts with Vulkan (shadPS4)')
            else:
                shaders = native_shaders.shader_dir(data, title_id, app_version)
                if not native_shaders.ready(shaders):
                    say('Native Vulkan: preparing its shaders from your game (only the first time, a few minutes)')
                    # "@progress" lines: the launchers show a window with a progress bar meanwhile
                    last = [-1]

                    def progress(percent, stage):
                        if percent != last[0]:
                            last[0] = percent
                            emit(f'@progress native-shaders {percent} {stage}\n')
                    built = native_shaders.build(PORT, game, shaders, say, progress=progress)
                    emit(f'@progress native-shaders done {"ok" if built else "failed"}\n')
                if native_shaders.ready(shaders):
                    env.update(native_shaders.environment(shaders, ini_value(config, 'temporal') or ''))
                    say('Graphics engine: native Vulkan (experimental)')
                else:
                    say('Native Vulkan: its shaders are not ready, so the game starts with Vulkan (shadPS4)')
        # The GPU library builds its path table when it loads: the user directory must exist.
        user_dir.mkdir(parents=True, exist_ok=True)
        env['BB_GPU_USER_DIR'] = str(user_dir)
        # The in-game "Apply and restart" ends the game with RESTART_CODE; it is started again
        # here, with the patches chosen in the menu, in the same console and log.
        env['BB_RESTART_EXIT_CODE'] = str(RESTART_CODE)
        env.pop('BB_RESTART_COMMAND', None)
        command = [str(probe), str(boot / 'boot-linked.bin'), '--import-names', str(out / 'import_names.txt'),
                   '--content-profile', str(out / 'content.bin'), '--patches', str(out / 'patches.bin'),
                   '--app0', str(game), '--user', str(user_dir),
                   '--timeout', env.get('BB_TIMEOUT', '0'), *isa_args, *args]
        progress_dir = Path(env.get('BB_PROGRESS_DIR') or user_dir)
        startup_call('online.cloud_sync', cloud_sync_game, env, config, progress_dir, title_id, False)
        first = True
        range_tries = start_tries = 0
        global PERF
        PERF = perf_report.Collector()
        while True:
            if not first:  # the in-game menu may have changed render_res
                settings, layout_patches = apply_layout()
                apply_render_res(env, settings)
                if mod_packages.ACTIVE:
                    startup_call('mod_packages.preflight', mod_packages.preflight, Path(env['BB_PATCHES_DIR']).parent, settings, Path(env['BB_PATCHES_CONFIG']),
                                           [layout_patches] if layout_patches else [], title_id, app_version, game=game)
            first = False
            run_python('scripts/patches.py', '--patches-dir', env.get('BB_PATCHES_DIR', 'patches'),
                       *(['--strict-overlap'] if mod_packages.ACTIVE else []),
                       *(['--patches-dir', layout_patches] if layout_patches else []),
                       '--patches-config', env.get('BB_PATCHES_CONFIG', data / 'patches.json'),
                       '--app-version', app_version, '--title-id', title_id, '--out', out, '--boot-dir', boot,
                       '--settings', settings)
            # Ultrawide: videos and cutscenes at 16:9 (src/runtime_cinematic.c) unless turned off.
            for key in ('BB_CINEMATIC_16X9', 'BB_CUTSCENE'):
                env.pop(key, None)
            plan = render_plan_of(settings)
            if plan and plan.get('output') and not ini_has(settings, 'ultrawide_cinematics=0'):
                env['BB_CINEMATIC_16X9'] = '1'
                env['BB_CUTSCENE'] = run_python('scripts/patches.py', '--print-cutscene', '--out', out, '--boot-dir', boot,
                                                capture=True).stdout.strip()
            # Ultrawide: the title and customization screens fill the width (src/runtime_frontend.c).
            env.pop('BB_FRONTEND_FOV', None)
            if plan and plan.get('output') and not ini_has(settings, 'ultrawide_frontend=0'):
                env['BB_FRONTEND_FOV'] = run_python('scripts/patches.py', '--print-frontend', '--out', out, '--boot-dir', boot,
                                                    capture=True).stdout.strip()
            # Disable Motion Blur: the eboot patch stops the camera's blur; the blur shaders go too
            # (objects moving on their own: the crowd, plants; bb_motion_blur.h)
            env['BB_NO_MOTION_BLUR'] = '1' if ini_has(config, 'patch_no_motion_blur=1') else '0'
            # Started from Steam (a non-Steam shortcut), Steam turns on its Fossilize layer, which
            # records every pipeline the game builds: thousands at start, so the window stayed
            # black for minutes. Steam never ships a shader cache for a shortcut, so it only costs.
            # The overlay layer (Shift+Tab) stays.
            env['DISABLE_VK_LAYER_VALVE_steam_fossilize_1'] = '1'
            applied = patches.applied_game_patches(out)  # None: an older compile, trusted
            if user_vblank:
                env['BB_VBLANK_HZ'] = user_vblank
            elif ini_has(config, 'patch_60fps=1') and (applied is None or 'patch_60fps' in applied):
                # The game's own limiter paces frames; a 60 Hz vblank would quantize late frames.
                env['BB_VBLANK_HZ'] = '480'
                say('60 FPS patch on: virtual vblank 480 Hz')
            elif ini_has(config, 'patch_60fps=1'):
                # The patch was skipped (its code is not in this build of the update): the game
                # still waits for vblanks at 30 FPS, and a 480 Hz vblank would run it 8 times fast.
                env['BB_VBLANK_HZ'] = {'uncap': '480', '90': '90'}.get(fps, '60')
                say('60 FPS patch: not applied (see "Patches: skipped" above); the game runs at '
                    'its own 30 FPS with a 60 Hz vblank')
            else:
                env['BB_VBLANK_HZ'] = {'uncap': '480', '90': '90'}.get(fps, '60')
            # No stdin: an inherited pipe (Git Bash, MSYS2) cost the game its console output.
            try:
                start_time = time.monotonic()
                process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, creationflags=no_console())
            except PermissionError as error:
                blocked(probe, error)
            reader = threading.Thread(target=pump, args=(process.stdout,), daemon=True)
            VULKAN_START.update(window=0.0, ready=False, out_of_memory=False)
            reader.start()
            hung = watch_vulkan_start(process, env)
            code = process.wait()
            reader.join(timeout=5)
            if hung:
                # the loader's own switch: no implicit layer this time (the system is not changed)
                env['VK_LOADER_LAYERS_DISABLE'] = '~implicit~'
                say(f'--- SacredFire launch {time.strftime("%Y-%m-%d %H:%M:%S")} (without overlay layers)')
                continue
            if code not in (0, RESTART_CODE) and VULKAN_START.get('out_of_memory'):
                # no point starting again: the memory is still missing
                say(f'--- the game ended with code {code}')
                say('--- Windows ran out of memory for the game (error 1455: RAM plus the page file are too small). '
                    'Enlarge the page file (System > Advanced system settings > Performance > Advanced > Virtual '
                    'memory: "Automatically manage", or at least 8192 MB on C:), restart the PC, and close other '
                    'programs and overlays before playing.')
                finish_perf_report(env, config, data, version, title_id, code)
                cloud_sync_game(env, config, progress_dir, title_id, True)
                mod_packages.finish(code)
                return OUT_OF_MEMORY_CODE
            if retry_taken_range(code, range_tries):
                range_tries += 1
                say(f'--- SacredFire launch {time.strftime("%Y-%m-%d %H:%M:%S")} '
                    f'(the memory the game needs was taken: try {range_tries + 1} of {RANGE_TAKEN_TRIES + 1})')
                continue
            if retry_failed_start(code, time.monotonic() - start_time, start_tries):
                start_tries += 1
                finish_perf_report(env, config, data, version, title_id, code)  # each failed start is counted
                PERF = perf_report.Collector()
                say(f'--- SacredFire launch {time.strftime("%Y-%m-%d %H:%M:%S")} '
                    f'(the game stopped while starting: try {start_tries + 1} of {START_FAULT_TRIES + 1})')
                continue
            if code != RESTART_CODE:
                say(f'--- the game ended with code {code}')
                finish_perf_report(env, config, data, version, title_id, code)
                cloud_sync_game(env, config, progress_dir, title_id, True)
                if code == RANGE_TAKEN_CODE:
                    say('--- Another program keeps taking the memory the game needs (see the "Module from '
                        'outside" lines above): close overlays and tuning tools, and exclude the game folder '
                        'from your antivirus.')
                if code == UNSUPPORTED_GPU_CODE:
                    say('--- This GPU or its driver lacks what the game needs from Vulkan (version 1.3 and '
                        'sparse buffers; the STOP line above says which). Update the graphics driver; if it '
                        'is already the latest, the GPU is too old for this system.')
                mod_packages.finish(code)
                return code
            say(f'--- SacredFire launch {time.strftime("%Y-%m-%d %H:%M:%S")} (restart from the menu)')
    finally:
        if mod_view:
            shutil.rmtree(mod_view, ignore_errors=True)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
