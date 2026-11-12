"""Validate passive matchmaking timer against both native regional layouts."""
import math
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
        # Native traversal must retain the panel's full subtree after attaching it.
        expected=[panel]+[image.u64(image.u64(children+32)+8*i) for i in range(4)]
        node=panel
        for i,wanted in enumerate(expected):
            self.assertEqual(node,wanted)
            if i: self.assertEqual(image.u64(node+0x58),expected[i-1])
            node=image.u64(node+0x50)
        self.assertEqual(node,0)
        previous_bottom = None
        angle = struct.unpack_from('<f',image.data,background+0x80)[0]
        cosine,sine = math.cos(math.radians(angle)),math.sin(math.radians(angle))
        for name,source_offset in (('TITLE',0x5f0),('TIME',0x4d0),('RANGE',0x5f0)):
            node = tags['OCTR_SEARCH_'+name]
            x, y = struct.unpack_from('<2f', image.data, node+0x60)
            width, height = struct.unpack_from('<2f', image.data, node+0x90)
            self.assertEqual(image.data[node+0x98:node+0xb0],image.data[background+0x98:background+0xb0])
            # Keep the native text's local rotation: the parent supplies the -7 degree tilt.
            self.assertEqual(image.data[node+0x78:node+0x84],native.data[source_offset+0x78:source_offset+0x84])
            self.assertEqual(x,-39)
            self.assertEqual(width,560)
            # Every corner must fit the tilted artwork, not only the label's center.
            for dx in (-width*.4/2,width*.4/2):
                for dy in (-height*.4/2,height*.4/2):
                    local_x=cosine*dx+sine*(y-84+dy)
                    local_y=-sine*dx+cosine*(y-84+dy)
                    self.assertLessEqual(abs(local_x),128-12)
                    self.assertLessEqual(abs(local_y),74-20)
            if previous_bottom is not None:
                self.assertGreater(y-height*.4/2,previous_bottom)
                self.assertLessEqual(y-height*.4/2-previous_bottom,2)
            previous_bottom=y+height*.4/2
        # Resolve each exported text asset and verify native fit-to-box, single-line centering.
        names=next(o for o,k in objects.items() if k=='igNameList')
        exports={image.string_at(image.u64(names+32)+16*i):image.u64(image.u64(32)+8*i)
                 for i in range(image.u32(names+12))}
        for name in ('TITLE','TIME','RANGE'):
            text=exports['OCTR_SEARCH_'+name+'_ASSET']
            self.assertGreaterEqual(struct.unpack_from('<f',image.data,text+0x64)[0],.5)
            self.assertEqual(image.u32(text+0x5c),1)
            self.assertEqual(image.u32(text+0x60),1)
            self.assertEqual(image.data[text+0x68:text+0x6a],b'\x01\x00')
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
