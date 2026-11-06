from paths import ROOT
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform == 'linux' and shutil.which('cc'), 'requires Linux and a C compiler')
class FriendsNativeTests(unittest.TestCase):
    def test_cache_invalidation(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'friends-native-test'
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'src'), str(ROOT / 'tests/test_friends_native.c'),
                            '-o', str(exe)], check=True, capture_output=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
