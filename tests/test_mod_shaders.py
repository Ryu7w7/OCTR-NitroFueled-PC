"""The shader setup's Linux side (scripts/mod_shaders.py): the setup game is closed through a quit
request only once it is our engine, and the setup then ends normally."""
from paths import ROOT
import itertools, os, subprocess, sys, tempfile, unittest
from pathlib import Path
import mod_shaders

# A stand-in engine: quits with code 0 on SIGTERM, as SDL's quit request makes the game do.
CHILD = 'import signal,sys,time; signal.signal(signal.SIGTERM, lambda *a: sys.exit(0)); print("ready", flush=True); time.sleep(30)'


@unittest.skipIf(os.name == 'nt', 'Linux only')
class PosixSetupTests(unittest.TestCase):
    def start(self):
        child = subprocess.Popen([sys.executable, '-c', CHILD], stdout=subprocess.PIPE, text=True)
        self.addCleanup(child.kill)
        child.stdout.readline()
        return child

    def test_closes_only_our_engine(self):
        child = self.start()
        self.assertFalse(mod_shaders.close_owned_game_posix(child.pid, '/bin/true'))
        self.assertIsNone(child.poll())
        self.assertTrue(mod_shaders.close_owned_game_posix(child.pid, sys.executable))
        self.assertEqual(child.wait(10), 0)

    def test_setup_waits_for_the_cache_then_closes(self):
        with tempfile.TemporaryDirectory() as temporary:
            user = Path(temporary)
            cache = user / 'cache/CUSA13795'
            cache.mkdir(parents=True)
            for name in ('profile.bin', 'a.meta', 'a.key'):
                (cache / name).write_bytes(b'x')
            child = self.start()
            clock = itertools.count(0, 1.0)  # each poll a second later
            mod_shaders.wait_for_setup(child, user, [], sys.executable, lambda *a: None,
                                       clock=lambda: next(clock), sleep=lambda _s: None)
            self.assertEqual(child.returncode, 0)


if __name__ == '__main__':
    unittest.main()
