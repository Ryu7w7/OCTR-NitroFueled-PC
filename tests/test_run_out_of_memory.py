"""run.py: a game that Windows could not give its memory ends with OUT_OF_MEMORY_CODE, not "code 3"."""
import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('run_for_out_of_memory', ROOT / 'run.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)


class OutOfMemoryTests(unittest.TestCase):
    def setUp(self):
        self.log, run.LOG = run.LOG, None
        run.VULKAN_START.update(window=0.0, ready=False, out_of_memory=False)

    def tearDown(self):
        run.LOG = self.log

    def test_the_page_file_line_is_noticed(self):
        # a player's log (2026-11-13): a 940MX laptop with a small page file
        run.emit('Runtime: cannot commit 3996 MiB of guest memory (error 1455); close other programs or '
                 'enlarge the page file\n')
        self.assertTrue(run.VULKAN_START['out_of_memory'])

    def test_other_lines_are_not(self):
        run.emit('Runtime: mapping direct memory 0, size=4190109696 failed (error 0x8002000c)\n')
        run.emit('Runtime: cannot commit 16 MiB of guest memory (error 87)\n')
        self.assertFalse(run.VULKAN_START['out_of_memory'])

    def test_its_code_is_not_one_the_game_uses(self):
        self.assertNotIn(run.OUT_OF_MEMORY_CODE, (0, run.RESTART_CODE, run.RANGE_TAKEN_CODE, run.START_FAULT_CODE,
                                                  run.UNSUPPORTED_GPU_CODE))
        launcher = (ROOT / 'launcher' / 'windows' / 'launcher.cpp').read_text(encoding='utf-8')
        self.assertIn(f'kOutOfMemoryCode = {run.OUT_OF_MEMORY_CODE};', launcher)


if __name__ == '__main__':
    unittest.main()
