import os
import sys
from paths import ROOT
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(ROOT / 'tools'))
import build_version

class BuildIdentity(unittest.TestCase):
    def test_release_and_source_identity(self):
        with tempfile.TemporaryDirectory() as d, patch.dict(os.environ, {'BB_VERSION': ''}):
            root = Path(d)
            for name in ('src/runtime.c', 'scripts/data.py', 'build.sh', 'gpu/CMakeLists.txt', 'tools/build_version.py'):
                p = root/name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b'first\n')
            with patch('build_version.subprocess.check_output', side_effect=[b'v1.2.3\n', b'']):
                self.assertEqual(build_version.identity(root), 'v1.2.3')
            with patch('build_version.subprocess.check_output', return_value=b'dirty'):
                first = build_version.identity(root)
                self.assertRegex(first, r'^dev-[a-f0-9]{20}$')
                (root/'src/runtime.c').write_bytes(b'first\r\n')
                self.assertEqual(build_version.identity(root), first)
                (root/'src/runtime.c').write_bytes(b'changed\n')
                self.assertNotEqual(build_version.identity(root), first)
            with patch.dict(os.environ, {'BB_VERSION': 'v1.2.4'}):
                self.assertEqual(build_version.identity(root), 'v1.2.4')
            for value in ('v1.2.3&release=old', 'v1.2.3"', 'unknown'):
                with patch.dict(os.environ, {'BB_VERSION': value}), self.assertRaises(ValueError):
                    build_version.identity(root)

if __name__ == '__main__':
    unittest.main()
