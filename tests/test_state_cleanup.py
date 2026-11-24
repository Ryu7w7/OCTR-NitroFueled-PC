"""state_cleanup.py: unpacked copies and prepared sessions nothing uses any more."""
import os
import shutil
import tempfile
import time
import unittest
from pathlib import Path

import paths  # noqa: F401
import state_cleanup as sc
from mod_session import launch_lock

A, B, C = 'a' * 64, 'b' * 64, 'c' * 64


class CleanupTests(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.packages = self.dir / 'content-state' / 'packages'
        for name in (A, B, 'stage-x'):
            (self.packages / name / 'pkg').mkdir(parents=True)
            (self.packages / name / 'pkg' / 'archive.pak').write_bytes(b'x' * 100)
        self.logged = []

    def test_copies_no_package_uses_are_removed(self):
        self.assertEqual(sc.unpacked(self.packages, {A}, 'External content', self.logged.append), 100)
        self.assertEqual(sorted(p.name for p in self.packages.iterdir()), [A, 'stage-x'])
        self.assertIn('removed 1 unused copies', self.logged[0])

    def test_nothing_is_removed_while_a_session_runs(self):
        state = self.dir / 'mod-state'
        with launch_lock(state):
            self.assertEqual(sc.unpacked_unless_running(self.packages, {A}, state, 'x', self.logged.append), 0)
        self.assertTrue((self.packages / B).is_dir())
        self.assertEqual(sc.unpacked_unless_running(self.packages, {A}, state, 'x', self.logged.append), 100)
        self.assertFalse((self.packages / B).exists())

    def test_the_latest_prepared_sessions_are_kept(self):
        overlays = self.dir / 'overlays'
        names = ['s1', 's2', 's3', 's4', 'current']
        for i, name in enumerate(names):
            (overlays / name).mkdir(parents=True)
            ready = overlays / name / 'ready.json'
            ready.write_text('{}')
            os.utime(ready, (time.time() - 100 + i, time.time() - 100 + i))
        (overlays / 'building-x').mkdir()
        sc.sessions(overlays, overlays / 'current', self.logged.append)
        self.assertEqual(sorted(p.name for p in overlays.iterdir()), ['current', 's3', 's4'])


if __name__ == '__main__':
    unittest.main()
