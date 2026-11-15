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
    def run_settings_check(self, source, expected):
        with tempfile.TemporaryDirectory(prefix='aspect-settings-test-') as tmp:
            exe = Path(tmp) / 'aspect-settings-test.exe'
            subprocess.run([COMPILER, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-UNDEBUG',
                            '-I' + str(ROOT), '-I' + str(ROOT / 'gpu/shim'),
                            str(ROOT / 'tests' / source),
                            str(ROOT / 'gpu/shim/bbport_settings.cpp'),
                            str(ROOT / 'gpu/shim/bbport_menu_es.cpp'), '-o', str(exe)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn(expected, result.stdout)

    def test_load_save_and_legacy_preferences(self):
        self.run_settings_check('test_aspect_settings.cpp', 'PASS: aspect ratios')

    def test_live_sync_and_concurrent_writers(self):
        self.run_settings_check('test_settings.cpp', 'PASS: live settings merge')


if __name__ == '__main__':
    unittest.main()
