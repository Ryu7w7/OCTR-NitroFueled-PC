"""Online result badge assets preserve native names and local ownership."""
import os
import struct
import unittest
from pathlib import Path
from collections import Counter
import gcn_shaders as iga
from igz_file import Igz
from ranked_name_badges import add_badges


@unittest.skipUnless(os.environ.get('BB_TEST_GAME'),'requires local game dump')
class RankedNameBadges(unittest.TestCase):
    def test_native_result_variants(self):
        game=Path(os.environ['BB_TEST_GAME'])
        for project in ('Octane_Race_Ranking','Octane_CupRace_Ranking',
                        'Octane_BattleTeam_Ranking'):
            with self.subTest(project=project):
                blob=None
                for archive in ('update2','update'):
                    pak=game/'archives'/f'{archive}.pak'
                    entry=next((e for e in iga.pak_entries(pak) if e[0].lower().endswith(
                        '/ui/'+project.lower()+'.igz')),None)
                    if entry:
                        with pak.open('rb') as f:blob=iga.read_entry(f,*entry[1:])
                        break
                self.assertIsNotNone(blob)
                old=Igz(blob);g=Igz(add_badges(blob,project))
                self.assertEqual(g.types,old.types)
                self.assertEqual(g.sizes,old.sizes)
                names=[o for o,t in old.objects() if t=='igGuiPlaceable' and o+0x28 in old.sites['RSTT']
                       and old.string_at(o+0x28)=='[PlayerTagText]']
                icons=[o for o,t in g.objects() if t=='igGuiPlaceable' and o+0x28 in g.sites['RSTT']
                       and g.string_at(o+0x28).startswith('OCTR_NAME_ICON_')]
                self.assertEqual(len(icons),len(names)*16)
                for name in names:
                    self.assertEqual(g.string_at(name+0x28),old.string_at(name+0x28))
                    for field,size in ((0x40,8),(0x60,48),(0x90,8),(0xe8,16)):
                        self.assertEqual(g.data[name+field:name+field+size],old.data[name+field:name+field+size])
                    parent=g.u64(name+0x40);table=g.u64(parent+0x48)
                    children={g.u64(g.u64(table+32)+8*i) for i in range(g.u32(table+12))}
                    own={icon for icon in icons if g.u64(icon+0x40)==parent}
                    self.assertEqual(len(own),16)
                    self.assertTrue(own.issubset(children))
                    self.assertTrue(all(g.u64(icon+0xe8)==0 and g.u64(icon+0xf0)==0 for icon in own))
                references=Counter(g.u64(at) for at in g.sites['ROFS'])
                for obj,kind in g.objects():
                    if obj>=len(old.data) and kind=='igGuiSprite':
                        self.assertEqual(g.u32(obj+8)&0x7ffff,references[obj])
                        self.assertEqual(g.u32(obj+0x30),0)
                for at in g.sites['ROFS']:
                    self.assertTrue(0<g.u64(at)<len(g.data))
                handles=[(g.strings[a],g.strings[b&0x7fffffff]) for a,b in struct.iter_unpack(
                    '<II',g.fixup('EXNM')[3][:g.fixup('EXNM')[1]*8]) if b&0x80000000]
                for icon in icons:
                    export,namespace=handles[g.u64(icon+0x38)&0x7fffffff]
                    self.assertEqual(namespace,project)
                    self.assertTrue(export.startswith('OCTR_NAME_RANK_'))
                native_handles=[(old.strings[a],old.strings[b&0x7fffffff]) for a,b in struct.iter_unpack(
                    '<II',old.fixup('EXNM')[3][:old.fixup('EXNM')[1]*8]) if b&0x80000000]
                nl=next(o for o,t in g.objects() if t=='igNameList')
                exported={g.string_at(g.u64(nl+32)+16*i):g.u64(g.u64(32)+8*i) for i in range(g.u32(nl+12))}
                for name in names:
                    text=exported[native_handles[old.u64(name+0x38)&0x7fffffff][0]]
                    self.assertEqual(g.u32(text+0x60),old.u32(text+0x60))
                self.assertNotIn('Octane_LobbyMembers',g.strings)
