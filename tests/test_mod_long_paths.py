"""The mod library's paths in Windows' extended-length form (mod_components.long_path): a package
in a deep folder (C:\\Users\\<name>\\Documents\\...) has files past 259 characters, which failed
with "[WinError 3]" on PCs without long paths enabled."""
from paths import ROOT
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'scripts'))
from mod_components import long_path, plain_path  # noqa: E402


@unittest.skipUnless(os.name == 'nt', 'extended-length paths are a Windows form')
class LongPathTests(unittest.TestCase):
    def test_forms(self):
        self.assertEqual(str(long_path(Path('C:/CTR/mod-state'))), '\\\\?\\C:\\CTR\\mod-state')
        self.assertEqual(str(long_path(Path('\\\\nas\\games\\ctr'))), '\\\\?\\UNC\\nas\\games\\ctr')
        self.assertEqual(long_path(long_path(Path('C:/CTR'))), long_path(Path('C:/CTR')))
        self.assertEqual(plain_path(long_path(Path('C:/CTR/a'))), Path('C:/CTR/a'))
        self.assertEqual(plain_path(long_path(Path('\\\\nas\\games'))), Path('\\\\nas\\games'))
        self.assertEqual(plain_path(Path('C:/CTR')), Path('C:/CTR'))

    def test_files_past_the_limit(self):
        with tempfile.TemporaryDirectory() as temp:
            deep = long_path(Path(temp)) / ('d' * 120) / ('e' * 120) / 'payloads'
            deep.mkdir(parents=True)
            target = deep / 'textures' / ('GuiStandard_diffuse,textures@!ui!Octane!Boost!' + 'x' * 40 + '.igz')
            target.parent.mkdir()
            target.write_bytes(b'igz')
            self.assertGreater(len(str(plain_path(target))), 259)
            copy = deep / 'copy.igz'
            shutil.copy2(target, copy)
            self.assertEqual(copy.read_bytes(), b'igz')
            shutil.rmtree(long_path(Path(temp)) / ('d' * 120))


if __name__ == '__main__':
    unittest.main()
