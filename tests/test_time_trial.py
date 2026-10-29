"""Progression checks; BB_TEST_GAME additionally checks a local USA or EU 1.21 executable."""
from paths import ROOT
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from prepare import parse_self


@unittest.skipUnless(sys.platform == 'linux' and shutil.which('cc'), 'requires Linux and a C compiler')
class TimeTrialTests(unittest.TestCase):
    def test_progression_and_native_bridge(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'time-trial-test'
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'src'), str(ROOT / 'tests/test_time_trial.c'),
                            '-o', str(exe)], check=True, capture_output=True)
            args = [str(exe)]
            if os.environ.get('BB_TEST_GAME'):
                elf = Path(tmp) / 'eboot.elf'
                elf.write_bytes(parse_self((Path(os.environ['BB_TEST_GAME']) / 'eboot.bin').read_bytes())[0])
                args.append(str(elf))
            result = subprocess.run(args, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('progression and bridge checks passed', result.stdout)
