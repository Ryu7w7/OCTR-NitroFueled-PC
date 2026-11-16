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
from matchmaking_timer import add_timer, timeout_return, preserve_menu_party


@unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local game dump')
class MatchmakingTimerTests(unittest.TestCase):
    def test_menu_back_preserves_party_and_explicit_offline_exit(self):
        game=Path(os.environ['BB_TEST_GAME'])
        tested=set()
        for archive in ('update2','juicedomain_onlinemenus'):
            pak=game/'archives'/(archive+'.pak')
            entries=iga.pak_entries(pak)
            for index,entry in enumerate(entries):
                if not any(entry[0].lower().endswith('/octane_lobby_'+name+'_root.igz')
                           for name in ('main','privategame','matchmaking','modeselect')):continue
                source=online_mode.current_entry(pak,entries,index,None)
                before=Igz(source);after=Igz(preserve_menu_party(source))
                methods=before.fixup('EXNM')
                native=[before.strings[a] for a,b in struct.iter_unpack('<II',methods[3][:methods[1]*8])
                        if not b&0x80000000]
                main='Scripts.Graph.Octane_Lobby_Main_Root' in before.strings
                if main:self.assertEqual(source,preserve_menu_party(source))
                stops=[o for o,k in before.objects() if k=='igVscCppActionNode' and native[before.u64(o+0x38)] in (
                    'CNetworkNodes::stopMatchMaking_VSC','CNetworkNodes::disconnectAll_VSC',
                    'CNetworkNodes::resetNetworSessionkManager_VSC')]
                quits=[o for o,k in before.objects() if k=='igVscCppActionNode'
                       and native[before.u64(o+0x38)]=='CNetworkNodes::quitParty_VSC']
                table=after.fixup('EXNM')
                patched=[after.strings[a] for a,b in struct.iter_unpack('<II',table[3][:table[1]*8])
                         if not b&0x80000000]
                allowed=set()
                for obj,kind in before.objects():
                    allowed.update(range(obj+8,obj+12))
                    if obj in stops and not main:
                        self.assertEqual(patched[after.u64(obj+0x38)],'CNetworkNodes::resetNetworkSessionManager_VSC')
                        self.assertEqual(after.u64(obj+0x28),before.u64(obj+0x28))
                        allowed.update(range(obj+56,obj+64))
                    if obj in quits and not main:
                        self.assertEqual(patched[after.u64(obj+0x38)],'CNetworkNodes::setOnlineLobbyIsOpened_VSC')
                        self.assertEqual(after.u32(obj+16),1)
                        boolean=after.u64(after.u64(obj+32))
                        self.assertEqual(after.data[boolean+14],0)
                        self.assertEqual(after.u64(obj+0x28),before.u64(obj+0x28))
                        allowed.update(range(obj+16,obj+40));allowed.update(range(obj+56,obj+64))
                    if kind=='igVscCppActionNode' and native[before.u64(obj+0x38)] in (
                            'CNetworkNodes::resetPartyManager_VSC','CNetworkNodes::resetNetworkSessionManager_VSC',
                            'CLoadMapNodes::quitToMainMenu2_VSC'):
                        size=before.sizes[before.u64(obj)]
                        self.assertEqual(after.data[obj+12:obj+size],before.data[obj+12:obj+size])
                self.assertTrue(all(a==b or i in allowed for i,(a,b) in enumerate(zip(before.data,after.data))))
                tested.add(entry[0].lower().rsplit('/',1)[1])
        self.assertEqual(len(tested),4)

    def test_timeout_cancels_search_without_quitting_party(self):
        pak=Path(os.environ['BB_TEST_GAME'])/'archives/update2.pak'
        entries=iga.pak_entries(pak)
        index=next(i for i,e in enumerate(entries) if e[0].lower().endswith('/octane_lobby_matchmaking_root.igz'))
        native=Igz(online_mode.current_entry(pak,entries,index,None))
        image=Igz(timeout_return(native.build()))
        self.assertEqual(image.u32(0x7c08+12),4)
        array=image.u64(0x7c08+32)
        self.assertEqual(image.u64(array),0x7c38)
        event=image.u64(array+8)
        leave_event=image.u64(array+16)
        guest_event=image.u64(array+24)
        guest_filters=image.u64(guest_event+0x18)
        guest_signal=image.u64(image.u64(guest_filters+32))
        self.assertEqual(image.u32(guest_signal+16),0x10002)
        guest_assign=image.u64(guest_event+0x30)
        self.assertEqual(image.u64(guest_assign+0x10),native.u64(0x8598+0x10))
        self.assertEqual(image.u64(guest_assign+0x18),native.u64(0x8598+0x18))
        self.assertNotEqual(image.u64(guest_assign+0x10),native.u64(0x3c30+0x10))
        self.assertEqual(image.data[image.u64(guest_assign+0x10)+14],1)
        waiting_menu=image.u64(guest_assign+0x20)
        self.assertEqual(image.u64(waiting_menu+0x38),native.u64(0x70e0+0x38))
        waiting_name=image.u64(image.u64(waiting_menu+32))
        self.assertEqual(dict(image.objects())[waiting_name],'igVscConstStringAccessor')
        self.assertEqual(image.string_at(waiting_name+16),'Octane_Lobby_WaitingRoom')
        self.assertEqual(image.u64(waiting_menu+0x28),image.u64(image.u64(event+0x30)+0x20))
        self.assertEqual(image.u64(leave_event+0x30),0x3c30)
        leave_filters=image.u64(leave_event+0x18)
        leave_signal=image.u64(image.u64(leave_filters+32))
        self.assertEqual(image.u32(leave_signal+16),0x10001)
        filters=image.u64(event+0x18)
        signal=image.u64(image.u64(filters+32))
        self.assertEqual(image.u32(signal+16),0x10000)
        self.assertEqual(native.u32(0x7140+16),9)
        assign=image.u64(event+0x30)
        self.assertEqual(image.u64(assign+0x10),native.u64(0x3c30+0x10))
        self.assertEqual(image.u64(assign+0x18),native.u64(0x3c30+0x18))
        stop=image.u64(assign+0x20)
        close_lobby=image.u64(stop+0x28)
        table=image.fixup('EXNM')
        methods=[image.strings[a] for a,b in struct.iter_unpack('<II',table[3][:table[1]*8]) if not b&0x80000000]
        self.assertEqual(methods[image.u64(stop+0x38)],'CNetworkNodes::resetNetworkSessionManager_VSC')
        self.assertEqual(methods[image.u64(close_lobby+0x38)],'CNetworkNodes::setOnlineLobbyIsOpened_VSC')
        self.assertEqual(image.u64(close_lobby+32),native.u64(0x3dd0+32))
        asynchronous=image.u64(close_lobby+0x28)
        self.assertEqual(image.u64(asynchronous+0x38),native.u64(0x3dd0+0x38))
        self.assertEqual(image.u64(asynchronous+0x28),0x3ea0)
        # Existing manual Back and explicit close retain their native party-leave behavior.
        for obj in (0x7c38,0x3c30,0x3d50,0x3d90,0x3dd0,0x3e60,0x8510):
            size=native.sizes[native.u64(obj)]
            self.assertEqual(image.data[obj+12:obj+size],native.data[obj+12:obj+size])
        counts=Counter(image.u64(at) for at in image.sites['ROFS'])
        for obj,kind in image.objects():
            if obj>=len(native.data):self.assertEqual(image.u32(obj+8)&0x7ffff,counts[obj])

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
