"""Check the Ranked score asset against the shipped native menu/reward layouts."""
from collections import Counter
import os
import struct
import unittest
from pathlib import Path
import gcn_shaders as iga
from igz_file import Igz, unpack_offsets
from ranked_results import score_card
from ranked_ui_assets import TIERS


@unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local game dump')
class RankedScoreTests(unittest.TestCase):
    def test_card_and_native_reverse_animation(self):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update2.pak'
        entries = iga.pak_entries(pak)
        with pak.open('rb') as f:
            def read(name):
                entry = next(e for e in entries if e[0].lower().endswith('/ui/'+name+'.igz'))
                return iga.read_entry(f, *entry[1:])
            native = read('octane_race_ranking')
            g = Igz(score_card(native, read('octane_aar_wumpaonly')))
        rows = {g.string_at(o+0x28): o for o,t in g.objects()
                if t == 'igGuiPlaceable' and o+0x28 in g.sites['RSTT']
                and g.string_at(o+0x28).startswith('OCTR_RESULT_')}
        self.assertEqual(len(rows), len(TIERS)+3)
        for row in rows.values():
            self.assertEqual(g.u64(row+0xe8), 0)  # No input or graph ownership.
            self.assertEqual(struct.unpack_from('<f',g.data,row+0xe0)[0], 0)
        # All inserted rows participate in both native traversal and child tables.
        table = g.u64(0x6a0+0x48)
        children = [g.u64(g.u64(table+0x20)+8*i) for i in range(g.u32(table+12))]
        self.assertTrue(set(rows.values()).issubset(children))
        chain, row = set(), 0x6a0
        while row:
            self.assertNotIn(row,chain)
            chain.add(row); row = g.u64(row+0x50)
        self.assertTrue(set(rows.values()).issubset(chain))
        icon = rows['OCTR_RESULT_ICON_0_1']
        animations = g.u64(g.u64(icon+0xf0)+0x18)
        scales = []
        for i in range(2):
            a = g.u64(animations+8*i)
            tracks = g.u64(a+0x18)
            self.assertEqual(g.u32(tracks+12),1)
            track = g.u64(g.u64(tracks+0x20))
            keys = g.u64(track+0x10)
            values = [struct.unpack_from('<2f',g.data,g.u64(g.u64(keys+0x20)+8*k)+0x18)
                      for k in range(g.u32(keys+12))]
            scales.append(values)
        self.assertEqual(scales[0],list(reversed(scales[1])))
        self.assertEqual(scales[0][-1],(0,0))
        self.assertAlmostEqual(scales[1][-1][0],2.8,places=5)
        # Serialized animation references are owning references, including table keys.
        references = Counter(g.u64(at) for at in g.sites['ROFS'])
        for off, kind in g.objects():
            if off >= len(Igz(native).data) and kind in (
                    'igGuiAnimationTable', 'igGuiAnimationTag', 'igGuiAnimation',
                    'igGuiTrackList', 'igGuiVec3Track', 'igGuiKeyframeList',
                    'igGuiVec3LinearKeyframe'):
                self.assertEqual(g.u32(off+8) & 0x7ffff, references[off],
                                 f'{kind} at {off:x} has wrong serialized ownership count')
        # Every relocated local pointer reaches data; every object type is serialized.
        for at in g.sites['ROFS']:
            self.assertLess(g.u64(at),len(g.data))
            self.assertNotEqual(g.u64(at),0)
        for at in g.sites['RVTB']:
            self.assertLess(g.u64(at),len(g.types))
        self.assertEqual(g.types, Igz(native).types)
        self.assertEqual(g.sizes, Igz(native).sizes)
        self.assertNotIn('igGuiEffect',g.types)
        self.assertNotIn('Octane_EndRaceMenu',g.strings)
