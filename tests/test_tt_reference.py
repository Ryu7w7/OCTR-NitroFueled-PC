"""Local-asset decoder checks; never launches the game."""
import os
import struct
import unittest
from pathlib import Path

import tt_reference as ghosts
from igz_file import Igz
import gcn_shaders as iga


class TTReferenceTests(unittest.TestCase):
    def test_rejects_truncated_or_unknown_schema(self):
        for blob in (b'', bytes(48), struct.pack('<12I', 0x134, 13, 0, 23, 900, 3, 0, 0, 0, 0, 0, 0)):
            with self.assertRaises(ValueError):
                ghosts.decode(blob)

    def test_binary_joins_snapshots_and_preserves_jump_edges(self):
        entries = [('Octane/Test/Test', 'test.igb')]
        data = {'test.igb': {'race_start_seconds': 1.,
                            'moves': [(0., 0., 0., 0.), (1., 1., 2., 3.), (1.1, 4., 5., 6.)],
                            'snapshots': [(0., 0, 1, 0, 0, 0, 0, 0, 0),
                                          (1., 0, 1, 0, 0, 1, 0, 1, 1)]}}
        binary = ghosts.reference_binary(entries, data)
        self.assertEqual(binary[:8], b'BBGHOST1')
        self.assertEqual(struct.unpack_from('<I', binary, 8)[0], 1)
        self.assertEqual(binary[12:17], b'test\0')
        self.assertEqual(struct.unpack_from('<I', binary, 76)[0], 2)
        self.assertEqual(struct.unpack_from('<4fI', binary, 80), (1., 1., 2., 3., 8195))

    def test_sustained_drift_boost_and_uturn_inputs(self):
        rows = [(1., 127, 1, 0, 0, 1, 0, 1, 0),
                (2., -127, 1, 0, 0, 1, 1, 0, 0),
                (3., -127, 0, 1, 1, 1, 0, 0, 0),
                (4., 127, 1, 1, 1, 0, 0, 0, 0),
                (5., 0, 1, 0, 0, 1, 0, 0, 0)]
        data = {'fixture': {'race_start_seconds': 1., 'snapshots': rows,
                           'moves': [(r[0], 1., 2., 3.) for r in rows]}}
        binary = ghosts.reference_binary([('test', 'fixture')], data)
        flags = [struct.unpack_from('<4fI', binary, 80 + i*20)[4] for i in range(5)]
        self.assertEqual(flags[0] & 385, 1 | 128)  # Held drift, despite no start_drift pulse.
        self.assertEqual((flags[0] >> 16) & 255, 127)
        self.assertTrue(flags[1] & 8)  # Opposite shoulder boost edge.
        self.assertEqual(flags[2] & 112, 16 | 64)  # Left U-turn + jump held.
        self.assertEqual(flags[4] & 385, 0)  # Plain hop without start_drift.
        self.assertEqual(flags[1] & 385, 1 | 128)  # Countersteer cannot reverse entry direction.
        self.assertEqual((flags[1] >> 16) & 255, 127)
        self.assertFalse(flags[3] & 16)  # Acceleration still held: no U-turn command.

    def test_drift_transition_between_spatial_samples_is_retained(self):
        rows = [(0., 0, 1, 0, 0, 0, 0, 0, 0),
                (1., 90, 1, 0, 0, 1, 0, 0, 0),
                (1.03, -127, 1, 0, 0, 1, 0, 1, 0),
                (1.04, 127, 1, 0, 0, 1, 0, 0, 0),
                (2., 0, 1, 0, 0, 0, 0, 0, 0)]
        data = {'fixture': {'race_start_seconds': 1., 'snapshots': rows,
                           'moves': [(t, 1., 2., 3.) for t in (1., 1.1, 2.)]}}
        binary = ghosts.reference_binary([('test', 'fixture')], data)
        flags = [struct.unpack_from('<4fI', binary, 80+i*20)[4]
                 for i in range(struct.unpack_from('<I', binary, 76)[0])]
        self.assertEqual(flags[0] & 385, 1 | 256)
        self.assertEqual(flags[1] & 385, 1 | 256)
        self.assertEqual(flags[-1] & 385, 0)

    def test_jump_edge_is_one_marker_not_early_window(self):
        moves = [(t, t*1000, 0., 0.) for t in (.8, .9, 1., 1.1, 1.2)]
        snapshots = [(0., 0, 1, 0, 0, 0, 0, 0, 0), (1., 0, 1, 0, 0, 1, 0, 0, 0)]
        data = {'fixture': {'race_start_seconds': .8, 'snapshots': snapshots, 'moves': moves}}
        binary = ghosts.reference_binary([('test', 'fixture')], data)
        flags = [struct.unpack_from('<4fI', binary, 80+i*20)[4] for i in range(5)]
        self.assertEqual([i for i, flag in enumerate(flags) if flag & 2], [2])

    def test_takeoff_between_movement_keys_keeps_original_timestamp(self):
        data = {'fixture': {'race_start_seconds': 1.,
                'moves': [(1., 0., 0., 0.), (1.1, 300., 0., 100.)],
                'snapshots': [(1., 0, 1, 0, 0, 0, 0, 0, 0),
                              (1.03, 0, 1, 0, 0, 1, 0, 0, 0)]}}
        binary = ghosts.reference_binary([('test', 'fixture')], data)
        self.assertEqual(struct.unpack_from('<I', binary, 76)[0], 3)
        points = [struct.unpack_from('<4fI', binary, 80+i*20) for i in range(3)]
        jump = [p for p in points if p[4] & 2]
        self.assertEqual(len(jump), 1)
        self.assertAlmostEqual(jump[0][0], 1.03, places=5)
        self.assertAlmostEqual(jump[0][1], 90., places=4)
        self.assertAlmostEqual(jump[0][3], 30., places=4)

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local game dump')
    def test_all_tt_references_and_corrupt_records(self):
        game = Path(os.environ['BB_TEST_GAME'])
        pak = game / 'archives/update2.pak'
        entry = next(e for e in iga.pak_entries(pak) if e[0].lower().endswith('/loosedata/ghostdata.igz'))
        with pak.open('rb') as f:
            blob = iga.read_entry(f, *entry[1:])
            entries = [entry for tier in range(4) for entry in ghosts.tt_entries(blob, tier)]
        self.assertEqual(len(entries), 160)
        self.assertEqual(len(set(name for _, name in entries)), 156)
        for track, name in entries:
            with self.subTest(track=track):
                blob = (game / 'ghosts' / name).read_bytes()
                data = ghosts.decode(blob)
                self.assertEqual(len(data['moves']), 2100)
                self.assertGreater(len(data['snapshots']), 100)
                self.assertEqual(data['moves'][0][0], 0)
                self.assertAlmostEqual(data['finish_seconds'], float(name.split('-')[-2]), places=4)
                self.assertAlmostEqual(data['moves'][-1][0] - data['race_start_seconds'],
                                       data['finish_seconds'], delta=.1)
                # Serialize order is not chronology; the actual key-list references are.
                self.assertTrue(all(a[0] <= b[0] for a, b in zip(data['moves'], data['moves'][1:])))
                size, _, tail = struct.unpack_from('<3I', blob, 16)
                broken = bytearray(blob)
                struct.pack_into('<I', broken, len(blob) - tail - size + 4, 0)
                with self.assertRaises(ValueError):
                    ghosts.decode(broken)
                broken = bytearray(blob)
                struct.pack_into('<I', broken, len(blob) - tail, 0xffffffff)
                with self.assertRaises(ValueError):
                    ghosts.decode(broken)


if __name__ == '__main__':
    unittest.main()
