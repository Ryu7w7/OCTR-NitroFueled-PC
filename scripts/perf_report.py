"""The anonymous performance diagnostic (bbport.ini perf_reports=1, asked once by the launcher).

The game logs one "Perf:" line every 5 seconds (gpu/shim/bbport_perf.h): a histogram of its frame
times. run.py feeds every line the game prints to a Collector, which groups those lines by track
("LOADING LEVEL"), and when the game ends builds a small report:

- the hardware and system: GPU model and driver, VRAM, CPU model and threads, RAM, OS;
- the build and the settings that change performance (resolution, upscaler, FPS limit, layout...),
  the renderer (shadPS4's Vulkan or the native one), the edition, online or not;
- per track: seconds played, average FPS, 1% low, share of slow frames, loading frames;
- how the game ended (its exit code: 23 is a lost GPU device).

Nothing personal goes into it: no nickname, key, IP, paths, friends or room. The report is always
kept beside the logs (perf/, the last 10) and is sent to the RyuNET server only with consent."""
import json
import os
import platform
import re
import secrets
import time
from pathlib import Path

EDGES = [5.0, 7.0, 8.5, 10.0, 12.0, 14.0, 16.0, 17.5, 20.0, 25.0, 33.4, 50.0, 100.0, 250.0]
REPORT_VERSION = 1
KEEP = 10
# bbport.ini keys that change performance; nothing else of the file is read into the report.
SETTINGS = ('render_res', 'output_res', 'upscaler', 'temporal', 'fps_limit', 'present_mode', 'preset',
            'msaa', 'live_resolution', 'widescreen_layout', 'widescreen_output', 'fullscreen', 'borderless',
            'patch_60fps', 'patch_no_motion_blur', 'patch_pro_1080p', 'async_pipelines', 'model_lod')
EXIT_CODES = {0: 'normal', 23: 'gpu_device_lost', 24: 'gpu_unsupported', 75: 'restart'}

_PERF = re.compile(r'^Perf: frames (\d+) ms ([\d.]+) worst ([\d.]+) long (\d+) h ([\d,]+)\s*$')
_LEVEL = re.compile(r"LOADING LEVEL: '([^']+)'")
_LOADED = re.compile(r'level finished loading in ([\d.]+) secs')
_GPU = re.compile(r'^GPU: (.+?), (\d+) MiB VRAM; (.+?), Vulkan ([\d.]+)\s*$')


def short_level(path):
    """octane/t131_coco_park/t131_coco_park -> t131_coco_park; menus and screens as 'menus'."""
    name = path.replace('\\', '/').rstrip('/').split('/')[-1].lower()
    if name.startswith('octane_') or not name:
        return 'menus'
    return re.sub(r'[^a-z0-9_]', '', name)[:48] or 'other'


class Level:
    def __init__(self):
        self.frames = 0
        self.ms = 0.0
        self.worst = 0.0
        self.long = 0
        self.hist = [0] * len(EDGES)

    def add(self, frames, ms, worst, long_frames, hist):
        self.frames += frames
        self.ms += ms
        self.worst = max(self.worst, worst)
        self.long += long_frames
        for i, n in enumerate(hist[:len(EDGES)]):
            self.hist[i] += n

    def percentile_ms(self, share):
        """The upper edge of the bucket holding the slowest `share` of frames (1% low: 0.01)."""
        if not self.frames:
            return None
        limit = self.frames * (1.0 - share)
        seen = 0
        for edge, n in zip(EDGES, self.hist):
            seen += n
            if seen >= limit:
                return edge
        return EDGES[-1]

    def summary(self):
        p99 = self.percentile_ms(0.01)
        p999 = self.percentile_ms(0.001)
        slow = sum(n for edge, n in zip(EDGES, self.hist) if edge > 33.4)
        return {
            'seconds': round(self.ms / 1000.0, 1),
            'avg_fps': round(self.frames * 1000.0 / self.ms, 1) if self.ms else None,
            'low_1pct_fps': round(1000.0 / p99, 1) if p99 else None,
            'low_01pct_fps': round(1000.0 / p999, 1) if p999 else None,
            'slow_frames_pct': round(100.0 * slow / self.frames, 2) if self.frames else None,
            'worst_ms': round(self.worst, 1),
            'loading_frames': self.long,
            'hist': self.hist,
        }


class Collector:
    """Fed every line the game prints (from run.py's reader thread)."""

    def __init__(self):
        self.level = 'start'
        self.levels = {}
        self.loads = {}  # level -> the game's own load times in seconds ("level finished loading")
        self.gpu = {}
        self.started = time.time()

    def feed(self, text):
        for line in text.splitlines():
            m = _PERF.match(line)
            if m:
                hist = [int(x) for x in m[5].split(',') if x]
                self.levels.setdefault(self.level, Level()).add(int(m[1]), float(m[2]), float(m[3]),
                                                                int(m[4]), hist)
                continue
            m = _LEVEL.search(line)
            if m:
                self.level = short_level(m[1])
                continue
            m = _LOADED.search(line)
            if m:
                self.loads.setdefault(self.level, []).append(round(float(m[1]), 2))
                continue
            if not self.gpu:
                m = _GPU.match(line)
                if m:
                    self.gpu = {'model': m[1][:80], 'vram_mib': int(m[2]), 'driver': m[3][:60],
                                'vulkan': m[4][:16]}

    def report(self, meta):
        tracks = {name: lv.summary() for name, lv in self.levels.items() if lv.frames}
        for name, seconds in self.loads.items():
            tracks.setdefault(name, {})['load_seconds'] = seconds[:20]
        everything = Level()
        for lv in self.levels.values():
            everything.add(lv.frames, lv.ms, lv.worst, lv.long, lv.hist)
        return {
            'report_version': REPORT_VERSION,
            'id': secrets.token_hex(8),  # one per report (to drop duplicates), never per player
            'minutes': round((time.time() - self.started) / 60.0, 1),
            'gpu': self.gpu,
            **system_info(),
            **meta,
            'overall': everything.summary() if everything.frames else None,
            'tracks': tracks,
        }


def _windows_cpu():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as k:
            return str(winreg.QueryValueEx(k, 'ProcessorNameString')[0]).strip()
    except OSError:
        return platform.processor()


def _windows_ram_mib():
    import ctypes

    class Status(ctypes.Structure):
        _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong),
                    ('ullTotalPhys', ctypes.c_ulonglong), ('ullAvailPhys', ctypes.c_ulonglong),
                    ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                    ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong),
                    ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
    s = Status()
    s.dwLength = ctypes.sizeof(Status)
    if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(s)):
        return int(s.ullTotalPhys // (1 << 20))
    return None


def _linux_value(path, key):
    try:
        for line in Path(path).read_text(encoding='utf-8', errors='replace').splitlines():
            k, _, v = line.partition(':' if path.startswith('/proc') else '=')
            if k.strip() == key:
                return v.strip().strip('"')
    except OSError:
        pass
    return None


def system_info():
    """CPU, RAM and OS, as anyone's system information tool shows them (no serials, no names)."""
    if os.name == 'nt':
        cpu, ram = _windows_cpu(), _windows_ram_mib()
        os_name = f'Windows {platform.release()} ({platform.version()})'
    else:
        cpu = _linux_value('/proc/cpuinfo', 'model name') or platform.processor()
        kb = _linux_value('/proc/meminfo', 'MemTotal')
        ram = int(kb.split()[0]) // 1024 if kb and kb.split()[0].isdigit() else None
        distro = _linux_value('/etc/os-release', 'PRETTY_NAME') or 'Linux'
        deck = ' (Steam Deck)' if 'steamdeck' in (_linux_value('/etc/os-release', 'VARIANT_ID') or '') else ''
        os_name = f'{distro}{deck}, kernel {platform.release()}'
    return {'cpu': {'model': (cpu or '')[:80], 'threads': os.cpu_count()}, 'ram_mib': ram,
            'os': os_name[:120]}


def settings_of(config):
    values = {}
    try:
        for line in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
            k, sep, v = line.partition('=')
            if sep and k.strip() in SETTINGS:
                values[k.strip()] = v.strip()[:24]
    except OSError:
        pass
    return values


def consent(config):
    """'1' sends, '0' does not, '' not asked yet (the launcher asks once): nothing is sent."""
    return settings_value(config, 'perf_reports')


def settings_value(config, key):
    try:
        for line in Path(config).read_text(encoding='utf-8', errors='replace').splitlines():
            k, sep, v = line.partition('=')
            if sep and k.strip() == key:
                return v.strip()
    except OSError:
        pass
    return ''


def keep(report, folder):
    """Writes the report to folder/<time>.json and keeps the last KEEP; its path."""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / time.strftime('%Y%m%d_%H%M%S.json')
    path.write_text(json.dumps(report, indent=1), encoding='utf-8')
    for old in sorted(folder.glob('*.json'))[:-KEEP]:
        try:
            old.unlink()
        except OSError:
            pass
    return path


def exit_meaning(code):
    return EXIT_CODES.get(code, 'crash' if code else 'normal')


# ---- run.sh's way in (Linux): run.py feeds a Collector itself ------------------------------------
# The game's output goes through `perf_report.py --tee <state>` on its way to the launcher (every
# line passed on at once), then `perf_report.py --finish <state> --code <exit>` builds, keeps and
# (with consent) sends the report.

def _state_of(c):
    return {'level': c.level, 'loads': c.loads, 'gpu': c.gpu, 'started': c.started,
            'levels': {n: [lv.frames, lv.ms, lv.worst, lv.long, lv.hist] for n, lv in c.levels.items()}}


def _collector_of(state):
    c = Collector()
    c.level, c.loads, c.gpu = state.get('level', 'start'), state.get('loads', {}), state.get('gpu', {})
    c.started = state.get('started', c.started)
    for name, (frames, ms, worst, long_frames, hist) in state.get('levels', {}).items():
        c.levels.setdefault(name, Level()).add(frames, ms, worst, long_frames, hist)
    return c


def tee(state_path, source=None, sink=None):
    """Passes every line from `source` to `sink` as it comes, feeding a Collector; its state goes
    to `state_path` at the end of the stream."""
    import sys
    source = source or sys.stdin.buffer
    sink = sink or sys.stdout.buffer
    c = Collector()
    for raw in iter(source.readline, b''):
        try:
            sink.write(raw)
            sink.flush()
        except OSError:
            pass  # the launcher went away: keep reading so the game is never blocked
        try:
            c.feed(raw.decode('utf-8', errors='replace'))
        except Exception:
            pass
    Path(state_path).write_text(json.dumps(_state_of(c)), encoding='utf-8')


def finish(state_path, code, config, data, octr, edition, say=print):
    try:
        c = _collector_of(json.loads(Path(state_path).read_text(encoding='utf-8')))
    except (OSError, ValueError):
        return
    report = c.report({
        'octr': octr[:40], 'edition': edition,
        'renderer': 'native' if os.environ.get('BB_VK_NATIVE_ONLY') == '1' else 'shadps4',
        'online': os.environ.get('BB_ONLINE') == '1',
        'settings': settings_of(config),
        'exit': {'code': code, 'meaning': exit_meaning(code)},
    })
    if not report.get('overall'):
        return
    path = keep(report, Path(data) / 'perf')
    if consent(config) != '1':
        return
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    try:
        import run  # its ryunet_json: HTTPS first, plain HTTP after it, as the game's own requests
        host = os.environ.get('BB_ONLINE_SERVER') or settings_value(config, 'online_server') or 'octr.ryu7w7.xyz'
        port = os.environ.get('BB_ONLINE_HTTP_PORT', '8080')
        if ':' in host and not host.startswith('['):
            host, _, p = host.partition(':')
            port = p if p.isdigit() else port
        run.ryunet_json(host, port, '/ryunet/perf', 4, method='POST',
                        body=json.dumps(report, separators=(',', ':')).encode())
        say(f'Performance report sent (anonymous; a copy: {path.name} in perf/)')
    except Exception as error:
        say(f'Performance report not sent ({type(error).__name__}); a copy stays in perf/')


if __name__ == '__main__':
    import argparse
    import sys
    parser = argparse.ArgumentParser()
    parser.add_argument('--tee', metavar='STATE')
    parser.add_argument('--finish', metavar='STATE')
    parser.add_argument('--code', type=int, default=0)
    parser.add_argument('--config', default='bbport.ini')
    parser.add_argument('--data', default='.')
    parser.add_argument('--octr', default='')
    parser.add_argument('--edition', default='')
    a = parser.parse_args()
    if a.tee:
        tee(a.tee)
    elif a.finish:
        finish(a.finish, a.code, a.config, a.data, a.octr, a.edition)
        try:
            os.unlink(a.finish)
        except OSError:
            pass
    sys.exit(0)
