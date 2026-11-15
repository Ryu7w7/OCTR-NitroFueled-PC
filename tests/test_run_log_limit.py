"""run.py's LogLimit: last_run.log stays small however long the game runs."""
import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location('run_for_log_limit', ROOT / 'run.py')
run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run)


class LogLimitTests(unittest.TestCase):
    def test_a_line_that_keeps_coming_is_thinned_out_with_its_count(self):
        limit = run.LogLimit()
        out = ''.join(limit(f'Native AI: control id={i % 8} tick={i}\n') for i in range(5000)).splitlines()
        self.assertEqual(out[:limit.SAME], [f'Native AI: control id={i % 8} tick={i}' for i in range(limit.SAME)])
        self.assertIn('only one in', out[limit.SAME])
        self.assertEqual(out[-1], '(x5000) Native AI: control id=7 tick=4999')
        self.assertLess(len(out), limit.SAME + 10)

    def test_past_the_size_only_what_went_wrong(self):
        limit = run.LogLimit(written=run.LogLimit.MAX)
        self.assertEqual(limit('Native Vulkan: 3 textures, 0 failed\n'), '')
        self.assertEqual(limit('Launch failed: no game\n'), 'Launch failed: no game\n')
        self.assertEqual(limit('--- the game ended with code 1\n'), '--- the game ended with code 1\n')
        limit.written = 2 * run.LogLimit.MAX
        self.assertEqual(limit('Exception 0xc0000005 at 0x1234\n'), '')

    def test_the_size_note_is_written_once(self):
        limit = run.LogLimit(written=run.LogLimit.MAX - 10)
        out = ''.join(limit(f'line {"x" * (i % 50)}{chr(65 + i % 26)}\n') for i in range(200))
        self.assertEqual(out.count('from here only the errors'), 1)


if __name__ == '__main__':
    unittest.main()
