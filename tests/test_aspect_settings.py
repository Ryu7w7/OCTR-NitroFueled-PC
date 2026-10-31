"""The in-game settings keep aspect ratios and load older widescreen preferences."""
from paths import ROOT
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

COMPILER = shutil.which('c++') or shutil.which('g++')


@unittest.skipUnless(COMPILER, 'requires a C++ compiler')
class AspectSettingsTests(unittest.TestCase):
    def test_load_save_and_legacy_preferences(self):
        with tempfile.TemporaryDirectory(prefix='aspect-settings-test-') as tmp:
            exe = Path(tmp) / 'aspect-settings-test.exe'
            subprocess.run([COMPILER, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-UNDEBUG',
                            '-I' + str(ROOT), '-I' + str(ROOT / 'gpu/shim'),
                            str(ROOT / 'tests/test_aspect_settings.cpp'),
                            str(ROOT / 'gpu/shim/bbport_settings.cpp'),
                            str(ROOT / 'gpu/shim/bbport_menu_es.cpp'), '-o', str(exe)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn('PASS: aspect ratios', result.stdout)


if __name__ == '__main__':
    unittest.main()
