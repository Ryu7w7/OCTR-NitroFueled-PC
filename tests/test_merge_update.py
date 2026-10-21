from paths import ROOT
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


def write(path, text='x'):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


class MergeUpdateTests(unittest.TestCase):
    def merge(self, base, update, out):
        return subprocess.run([sys.executable, str(ROOT / 'tools/merge_update.py'), str(base), str(update), str(out)],
                              capture_output=True, text=True)

    def test_ps4_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            write(tmp / 'base/eboot.bin', 'old')
            write(tmp / 'base/sce_sys/param.sfo', 'base')
            write(tmp / 'base/sce_module/libc.prx')
            write(tmp / 'patch/eboot.bin', 'new')
            write(tmp / 'patch/sce_sys/param.sfo', 'patch')
            self.assertEqual(self.merge(tmp / 'base', tmp / 'patch', tmp / 'out').returncode, 0)
            self.assertEqual((tmp / 'out/eboot.bin').read_text(), 'new')
            self.assertEqual((tmp / 'out/sce_sys/param.sfo').read_text(), 'patch')
            self.assertTrue((tmp / 'out/sce_module/libc.prx').is_file())

    def test_pkg_tool_layout(self):
        # PS4 PKG Tool: Image0 holds the files, Sc0 the sce_sys ones; the update was fixed by
        # hand (eboot.bin and param.sfo copied out), which still counts as Image0 + Sc0.
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            write(tmp / 'base/Image0/eboot.bin', 'old')
            write(tmp / 'base/Image0/sce_module/libc.prx')
            write(tmp / 'base/Image0/memoryconfiguration.igz')
            write(tmp / 'base/Sc0/param.sfo', 'base')
            write(tmp / 'patch/Image0/eboot.bin', 'new')
            write(tmp / 'patch/Sc0/param.sfo', 'patch')
            write(tmp / 'patch/eboot.bin', 'new')
            write(tmp / 'patch/sce_sys/param.sfo', 'patch')
            self.assertEqual(self.merge(tmp / 'base', tmp / 'patch', tmp / 'out').returncode, 0)
            out = tmp / 'out'
            self.assertEqual((out / 'eboot.bin').read_text(), 'new')
            self.assertEqual((out / 'sce_sys/param.sfo').read_text(), 'patch')
            self.assertTrue((out / 'sce_module/libc.prx').is_file())
            self.assertTrue((out / 'memoryconfiguration.igz').is_file())
            self.assertFalse((out / 'Image0').exists())
            self.assertFalse((out / 'Sc0').exists())

    def test_not_a_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'base').mkdir()
            write(tmp / 'patch/eboot.bin')
            self.assertNotEqual(self.merge(tmp / 'base', tmp / 'patch', tmp / 'out').returncode, 0)


if __name__ == '__main__':
    unittest.main()
