"""speed_mode.prepare keeps its overlays while nothing they are made from changed."""
from paths import ROOT  # noqa: F401  (puts scripts/ on the path)
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import speed_mode


class SpeedModeCacheTests(unittest.TestCase):
    def test_kept_until_an_input_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'game/archives').mkdir(parents=True)
            pak = root / 'game/archives/t111_crash_cove.pak'
            pak.write_bytes(b'TRACK')
            base = root / 'online-mode.bbpatch'
            base.write_bytes(b'BASE 1')
            out = root / 'speed-mode'
            builds = []

            def build(game, output, base_patches):
                builds.append(1)
                normal, speed = output / 'a.normal.bbpatch', output / 'a.speed.bbpatch'
                normal.write_bytes(b'N')
                speed.write_bytes(b'S')
                return [normal, *base_patches], [speed, *base_patches], 40

            with mock.patch.object(speed_mode, 'build', build):
                first = speed_mode.prepare(root / 'game', out, [base])
                self.assertEqual(speed_mode.prepare(root / 'game', out, [base]), first)
                self.assertEqual(len(builds), 1)
                base.write_bytes(b'BASE 2')  # an earlier overlay changed (item rules, a mod)
                speed_mode.prepare(root / 'game', out, [base])
                self.assertEqual(len(builds), 2)
                pak.write_bytes(b'TRACK, updated')  # the game's archive changed
                speed_mode.prepare(root / 'game', out, [base])
                self.assertEqual(len(builds), 3)
                (out / 'a.speed.bbpatch').unlink()  # an overlay is gone
                speed_mode.prepare(root / 'game', out, [base])
                self.assertEqual(len(builds), 4)
                speed_mode.prepare(root / 'game', out, [base])
                self.assertEqual(len(builds), 4)

            def broken(game, output, base_patches):
                raise ValueError('damaged')

            base.write_bytes(b'BASE 3')
            with mock.patch.object(speed_mode, 'build', broken):
                with self.assertRaises(ValueError):
                    speed_mode.prepare(root / 'game', out, [base])
            self.assertFalse((out / 'cache.json').exists())  # nothing half-built is trusted


if __name__ == '__main__':
    unittest.main()
