"""No game build: exercise the AppImage bridge against a synthetic child process."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
CHILD = r'''
import ctypes, mmap, struct, time
m = mmap.mmap(-1, 0x3000000, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
base = ctypes.addressof(ctypes.c_char.from_buffer(m))
def pointer(at, value): struct.pack_into('<Q', m, at, base+value)
def integer(at, value): struct.pack_into('<i', m, at, value)
m[0x94efa0:0x94efa7] = bytes.fromhex('488b05314d1602')
manager, octane, racers, settings = 0x2b00000, 0x2b01000, 0x2b02000, 0x2b02100
lst, entries, racer = 0x2b02200, 0x2b02300, 0x2b02400
loading = 0x2b04000
pointer(manager+0x88, loading); pointer(loading, 0x2540eb8)
pointer(manager+0xc0, 0x2b04100)
track = b"octane/t213_tiny_temple/t213_tiny_temple\0"
m[0x2b04100:0x2b04100+len(track)] = track
pointer(0x2ab3cd8, manager); integer(manager+0x144, 8)
pointer(0x2acde48, octane); integer(octane+0xd4c, 2)
pointer(0x2ad4b70, racers); pointer(racers+0x10, lst)
integer(lst+0xc, 1); pointer(lst+0x20, entries); pointer(entries, racer)
pointer(racer, 0x26d5f78); m[racer+0xb0] = 1; integer(racer+0x4c, 2)
pointer(0x2ad4e30, settings); integer(settings+0xc, 3)
pool, gens, values, objects = 0x2b03000, 0x2b03100, 0x2b03200, 0x2b03300
entity, components, component_entries = 0x2b03400, 0x2b03500, 0x2b03600
clock, timer, data, gametime = 0x2b03700, 0x2b03800, 0x2b03900, 0x2b03a00
integer(0x2a92888, 0x8000); pointer(0x2a92880, pool)
pointer(pool+0x28, gens); integer(gens+0xc, 1); pointer(gens+0x20, values)
struct.pack_into('<H', m, values, 0x8000)
pointer(pool+0x20, objects); pointer(objects, entity); pointer(entity+0x18, components)
integer(components+0xc, 1); pointer(components+0x20, component_entries); pointer(component_entries, clock)
pointer(clock, 0x2592f98); pointer(clock+0x28, timer); pointer(clock+0x48, data)
integer(timer+0xc, 8192); m[timer+0x14] = 1
pointer(0x2ab4100, gametime); integer(gametime+0x20, 84*8192); m[gametime+0x28] = 1
time.sleep(30)
'''


@unittest.skipUnless(sys.platform == "linux", "Linux /proc reader")
class BridgeTests(unittest.TestCase):
    def launch(self, directory, code):
        env = dict(os.environ, BB_CONFIG=str(Path(directory)/'bbport.ini'),
                   PYTHONPATH=str(ROOT/'tools/re'))
        return subprocess.Popen([sys.executable, str(ROOT/'packaging/presence_bridge.py'),
                                 sys.executable, '-c', code], env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def test_live_child_snapshot_and_signal_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            process = self.launch(directory, CHILD)
            state = Path(directory)/'ryunet_state.txt'
            try:
                deadline = time.monotonic()+12
                body = ''
                while time.monotonic() < deadline and 'race_seconds=83\n' not in body:
                    body = state.read_text() if state.exists() else ''
                    time.sleep(0.1)
                self.assertIn('phase=Paused\n', body)
                self.assertIn('race=Time Trial\n', body)
                self.assertIn('track=octane/t213_tiny_temple/t213_tiny_temple\n', body)
                self.assertIn('lap=2\n', body)
                self.assertIn('laps=3\n', body)
                self.assertIn('race_seconds=83\n', body)
            finally:
                process.send_signal(signal.SIGTERM)
                output, errors = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 143, errors)
            self.assertIn('bridge connected', output)
            self.assertFalse(state.exists())

    def test_loading_overrides_stale_racer_and_menu_omits_mode(self):
        for loading_state, count, phase in ((1, 1, 'Loading'), (2, 1, 'Loading'),
                                             (3, 1, 'Loading'), (4, 1, 'Loading'),
                                             (0, 0, 'In menus'), (0, 1, 'In menus')):
            with self.subTest(state=loading_state), tempfile.TemporaryDirectory() as directory:
                code = CHILD.replace('time.sleep(30)',
                    f'integer(loading+0xc, {loading_state}); integer(lst+0xc, {count}); time.sleep(30)')
                if phase == 'In menus' and count == 1:
                    code = code.replace('octane/t213_tiny_temple/t213_tiny_temple',
                                        'Octane/Octane_StartScreen/Octane_StartScreen')
                process = self.launch(directory, code)
                try:
                    state = Path(directory)/'ryunet_state.txt'
                    deadline = time.monotonic()+12
                    body = ''
                    while time.monotonic() < deadline and f'phase={phase}\n' not in body:
                        body = state.read_text() if state.exists() else ''
                        time.sleep(0.1)
                    self.assertIn(f'phase={phase}\n', body)
                    for field in ('mode=', 'race=', 'track=', 'lap=', 'race_seconds='):
                        self.assertNotIn(field, body)
                finally:
                    process.terminate()
                    process.communicate(timeout=5)

    def test_running_clock_epoch_tracks_game_time(self):
        with tempfile.TemporaryDirectory() as directory:
            code = CHILD.replace('m[gametime+0x28] = 1', 'm[gametime+0x28] = 0')
            process = self.launch(directory, code)
            try:
                state = Path(directory)/'ryunet_state.txt'
                deadline = time.monotonic()+12
                body = ''
                while time.monotonic() < deadline and 'timer_start=' not in body:
                    body = state.read_text() if state.exists() else ''
                    time.sleep(0.1)
                values = dict(line.split('=', 1) for line in body.splitlines())
                self.assertEqual(values['phase'], 'Racing')
                self.assertLessEqual(abs(int(values['at']) - int(values['timer_start']) - 83), 1)
            finally:
                process.terminate()
                process.communicate(timeout=5)

    def test_original_exit_status(self):
        with tempfile.TemporaryDirectory() as directory:
            process = self.launch(directory, 'raise SystemExit(7)')
            _, errors = process.communicate(timeout=5)
            self.assertEqual(process.returncode, 7, errors)


if __name__ == '__main__':
    unittest.main()
