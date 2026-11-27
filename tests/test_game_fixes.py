"""Fixes to the game's own levels (scripts/game_fixes.py)."""
import hashlib
import unittest

from paths import ROOT  # noqa: F401  (scripts/ on the path)
import game_fixes


class GameFixesTests(unittest.TestCase):
    def fix_for(self, data):
        fixed = bytearray(data)
        fixed[4:6] = b'\xaa\xbb'
        return {'id': 'x', 'by': 'test', 'path': 'maps/x.igz', 'changes': [(4, 'aabb')],
                'original': hashlib.sha256(data).hexdigest(), 'fixed': hashlib.sha256(bytes(fixed)).hexdigest()}

    def test_applies_only_to_its_file(self):
        data = bytes(range(16))
        fix = self.fix_for(data)
        self.assertEqual(game_fixes.apply(fix, data)[:8], bytes([0, 1, 2, 3, 0xaa, 0xbb, 6, 7]))
        self.assertIsNone(game_fixes.apply(fix, data[:-1] + b'\xff'))   # another file (a mod's, an edition's)
        bad = dict(fix, changes=[(4, 'aacc')])                          # bytes that do not give the result
        self.assertIsNone(game_fixes.apply(bad, data))

    def test_fixes_are_well_formed(self):
        for fix in game_fixes.FIXES:
            self.assertEqual(len(fix['original']), 64)
            self.assertEqual(len(fix['fixed']), 64)
            self.assertTrue(fix['path'] == fix['path'].lower())
            for offset, hex_bytes in fix['changes']:
                self.assertGreaterEqual(offset, 0)
                self.assertTrue(bytes.fromhex(hex_bytes))


if __name__ == '__main__':
    unittest.main()
