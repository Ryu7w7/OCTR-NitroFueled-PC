"""Validate passive matchmaking timer against both native regional layouts."""
import os
import struct
import unittest
from collections import Counter
from pathlib import Path

import gcn_shaders as iga
from igz_file import Igz, unpack_offsets
import online_mode
from matchmaking_timer import add_timer


@unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local game dump')
class MatchmakingTimerTests(unittest.TestCase):
    def test_passive_native_countdown_clone(self):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update2.pak'
        entries = iga.pak_entries(pak)
        def read(path):
            index = next(i for i, entry in enumerate(entries) if entry[0].lower() == path)
            return online_mode.current_entry(pak, entries, index, None)
        source = read('temporary/octane/data/ps4/output/ui/octane_lobby_timer.igz')
        native = Igz(source)
        lobby = online_mode.public_lobby_card(read(online_mode.PUBLIC_LOBBY_ENTRY))
        image = Igz(add_timer(lobby, source))
        image.sites['RHND'] = image.read_list('RHND', unpack_offsets)
        objects = dict(image.objects())
        tags = {image.string_at(o+0x28): o for o, kind in objects.items()
                if kind == 'igGuiPlaceable' and o+0x28 in image.sites['RSTT']}
        panel, background = tags['OCTR_SEARCH_PANEL'], tags['OCTR_SEARCH_BACKGROUND']
        self.assertEqual(dict(native.objects())[0x12e0], 'igGuiSprite')
        for target, original in ((panel, 0x140), (background, 0x3b0)):
            self.assertEqual(image.data[target+0x60:target+0xd0], native.data[original+0x60:original+0xd0])
        self.assertEqual(image.u64(panel+0x40), tags['OCTR_PUBLIC_LOBBY'])
        self.assertTrue(image.u32(panel+12) & 0x400)
        children = image.u64(panel+0x48)
        self.assertEqual(image.u32(children+12), 4)
        previous_bottom = None
        for name in ('TITLE', 'TIME', 'RANGE'):
            node = tags['OCTR_SEARCH_'+name]
            x, y = struct.unpack_from('<2f', image.data, node+0x60)
            width, height = struct.unpack_from('<2f', image.data, node+0x90)
            self.assertEqual(x, -39)
            self.assertGreater(width, 0)
            # Native scale .4 keeps each fitted text inside the 256x148 artwork.
            self.assertLessEqual(width*.4, 256)
            if previous_bottom is not None:
                self.assertGreater(y-height*.4/2, previous_bottom)
            previous_bottom = y+height*.4/2
        counts = Counter(image.u64(at) for at in image.sites['ROFS'])
        new_start = len(Igz(lobby).data)
        for obj, kind in objects.items():
            if obj < new_start:
                continue
            self.assertEqual(image.u32(obj+8) & 0x7ffff, counts[obj], kind)
            if kind == 'igGuiPlaceable':
                for field in (0x30,0xe8,0xf0,0xf8):
                    self.assertEqual(image.u64(obj+field), 0)
        for at in image.sites['ROFS']:
            self.assertLess(image.u64(at), len(image.data))
        self.assertEqual(source, native.build())


if __name__ == '__main__':
    unittest.main()
