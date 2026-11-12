"""Check the Ranked score asset against the shipped native menu/reward layouts."""
from collections import Counter
import os
import json
import struct
import unittest
import tempfile
from unittest.mock import patch
from pathlib import Path
import gcn_shaders as iga
from igz_file import Igz, unpack_offsets
from ranked_results import score_card, manual_navigation
from ranked_ui_assets import TIERS


@unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local game dump')
class RankedScoreTests(unittest.TestCase):
    def test_prepared_global_score_panel(self):
        import online_mode
        import archive_patch
        from graphics_tab import Archive
        game = Path(os.environ['BB_TEST_GAME'])
        pak = game / 'archives/update2.pak'
        entries = iga.pak_entries(pak)
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)
            elf = out / 'eboot.elf'
            elf.write_bytes(b'')
            from ui_texture import SINGLE_RACE_TEXTURE
            stale = out / 'online/cup-assets' / SINGLE_RACE_TEXTURE
            stale.parent.mkdir(parents=True)
            stale.write_bytes(b'old shared badge override')
            with patch.object(online_mode, 'hook_site', return_value=0x1277e00):
                patches, _ = online_mode.prepare(game, out / 'online', elf, [])
            base = next(p for p in patches if archive_patch.read(p)[1] == '/app0/archives/update2.pak')
            self.assertFalse(stale.exists())
            patch_by_guest = {archive_patch.read(p)[1]: p for p in patches}
            for name in ('update', 'update2', 'juicedomain_onlinemenus', 'juicedomain_startscreen',
                         'juicedomain_race', 'juicedomain_race_flareday', 'juicedomain_race_flarenight'):
                source = game / 'archives' / (name + '.pak')
                source_entries = iga.pak_entries(source)
                for index, entry in enumerate(source_entries):
                    if entry[0].lower().removeprefix(online_mode.PACKAGE_PREFIX) == SINGLE_RACE_TEXTURE.lower():
                        self.assertEqual(online_mode.current_entry(source, source_entries, index,
                            patch_by_guest.get('/app0/archives/' + name + '.pak')),
                            online_mode.current_entry(source, source_entries, index, None), name)
            custom = Archive(out / 'online/custom_ui_assets.pak')
            try:
                self.assertNotIn(SINGLE_RACE_TEXTURE.lower(),
                                 {custom.logical(i).lower() for i in range(len(custom.names))})
            finally:
                custom.f.close()
            def asset(name, base):
                index = next(i for i, entry in enumerate(entries)
                             if entry[0].lower().endswith('/ui/' + name + '.igz'))
                return online_mode.current_entry(pak, entries, index, base)
            score = asset('octane_race_ranking', base)
            self.assertIn('OCTR_RESULT_MMR', Igz(score).strings)
            self.assertEqual(asset('octane_endracemenu', base), asset('octane_endracemenu', None))
            for project,source_name in (('octane_cuprace_ranking','update2'),):
                source=game/'archives'/f'{source_name}.pak'
                source_entries=iga.pak_entries(source)
                index=next(i for i,e in enumerate(source_entries) if e[0].lower().endswith('/ui/'+project+'.igz'))
                changed=online_mode.current_entry(source,source_entries,index,patch_by_guest['/app0/archives/'+source.name])
                project_asset=Igz(changed)
                self.assertTrue(any(value.startswith('OCTR_NAME_ICON_') for value in project_asset.strings))
                fallback=Archive(out/'online'/(project+'.pak'))
                try:
                    files={fallback.logical(i).lower():fallback.read(i) for i in range(len(fallback.names))}
                    self.assertEqual(files['ui/'+project+'.igz'],changed)
                    self.assertIn('ui/materialinstances/juice_octane_lobbymembers_materials.igz',files)
                finally:fallback.f.close()
            cache=json.loads((out/'online/cache.json').read_text())
            self.assertFalse(any(Path(row[0]).name=='octane_online_podium.pak' for row in cache['files']))
            source=game/'archives/update.pak'
            source_entries=iga.pak_entries(source)
            index=next(i for i,e in enumerate(source_entries) if e[0].lower().endswith('/ui/octane_online_podium.igz'))
            self.assertEqual(online_mode.current_entry(source,source_entries,index,patch_by_guest['/app0/archives/update.pak']),
                             online_mode.current_entry(source,source_entries,index,None))

            archive = Archive(out / 'online/octane_race_ranking.pak')
            try:
                files = {archive.logical(i).lower(): archive.read(i) for i in range(len(archive.names))}
            finally:
                archive.f.close()
            self.assertEqual(files['ui/octane_race_ranking.igz'], score)
            self.assertIn('ui/octane_aar_wumpaonly.igz',set(files))
            self.assertIn('sounds/banks/ui_grandprix_bank.igz',set(files))
            bank = Igz(files['sounds/banks/ui_grandprix_bank.igz'])
            for event in ('UI_GrandPrix_Tier_Scroll_out','UI_GrandPrix_Tier_Scroll_in','UI_GrandPrix_Reward_Unlock','UI_GrandPrix_Challenge_Bonus_in'):
                self.assertIn(event,bank.strings)
            graph_path = 'vsc/ui/octane_race_ranking/octane_race_ranking_root.igz'
            self.assertEqual(Igz(files[graph_path]).u32(0x3418+0x10),2)
            material = online_mode.LOBBY_MEMBERS_MATERIALS_ENTRY.removeprefix(online_mode.PACKAGE_PREFIX)
            rows = online_mode.package_rows(Igz(files['packages/generated/ui/octane_race_ranking_pkg.igz']))[1]
            self.assertIn(('material_instances', material), rows)
            self.assertIn(material, files)
            self.assertTrue(all(path in files for _, path in rows))

    def test_manual_navigation_preserves_online_background_and_input_graph(self):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update2.pak'
        entry = next(e for e in iga.pak_entries(pak) if e[0].lower().endswith(
            '/vsc/ui/octane_race_ranking/octane_race_ranking_root.igz'))
        with pak.open('rb') as stream:
            old = Igz(iga.read_entry(stream, *entry[1:]))
        new = Igz(manual_navigation(old.build()))
        changed = {o for o,t in old.objects() if
                   old.data[o:o+old.sizes[old.u64(o)]] != new.data[o:o+old.sizes[old.u64(o)]]}
        self.assertEqual(changed, {0x3418,0x5d60})
        for node in changed:
            self.assertEqual(new.u32(node+0x10),2)
            self.assertEqual(new.data[node+0x28:node+0x40],old.data[node+0x28:node+0x40])
        for node in (0x3ea0,0x6850):
            self.assertEqual(new.u32(node+0x10),0)  # End_Race_Flow and input keep online branches.
        self.assertEqual(new.u32(0x58b8+0x10),0)
        self.assertEqual(new.strings,old.strings)

    def test_card_and_native_reverse_animation(self):
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/update2.pak'
        entries = iga.pak_entries(pak)
        with pak.open('rb') as f:
            def read(name):
                entry = next(e for e in entries if e[0].lower().endswith('/ui/'+name+'.igz'))
                return iga.read_entry(f, *entry[1:])
            native = read('octane_race_ranking')
            reward = read('octane_aar_wumpaonly')
            g = Igz(score_card(native, reward))
        rows = {g.string_at(o+0x28): o for o,t in g.objects()
                if t == 'igGuiPlaceable' and o+0x28 in g.sites['RSTT']
                and g.string_at(o+0x28).startswith('OCTR_RESULT_')}
        self.assertEqual(len(rows), len(TIERS)+7)
        for row in rows.values():
            self.assertEqual(g.u64(row+0xe8), 0)  # No input or graph ownership.
            self.assertEqual(struct.unpack_from('<f',g.data,row+0xe0)[0], 0)
        for row in rows.values():
            x,y = struct.unpack_from('<2f', g.data, row+0x60)
            width,height = struct.unpack_from('<2f', g.data, row+0x90)
            sx,sy = struct.unpack_from('<2f', g.data, row+0x84)
            self.assertGreaterEqual(x-width*sx/2, 1250)
            self.assertLessEqual(x+width*sx/2, 1900)
            self.assertGreaterEqual(y-height*sy/2, 750)
            self.assertLessEqual(y+height*sy/2, 980)
        panel=rows['OCTR_RESULT_PANEL']
        self.assertLess(g.u64(panel),len(g.types))
        # Separate visual roles; the hero is no longer the same style as supporting labels.
        exnm=g.fixup('EXNM')
        handles=[g.strings[a] for a,b in struct.iter_unpack('<II',exnm[3][:exnm[1]*8]) if b&0x80000000]
        exports=next(o for o,t in g.objects() if t=='igNameList')
        named={g.string_at(g.u64(exports+32)+i*16):g.u64(g.u64(32)+i*8) for i in range(g.u32(exports+12))}
        fonts={name:struct.unpack_from('<f',g.data,named[handles[g.u64(rows['OCTR_RESULT_'+name]+0x38)&0x7fffffff]]+0x64)[0]
               for name in ('TITLE','DELTA','TIER','MMR')}
        self.assertGreater(fonts['DELTA'],fonts['TIER']*1.7)
        self.assertGreater(fonts['TIER'],fonts['MMR'])
        self.assertLess(fonts['TITLE'],fonts['TIER'])
        # The native legend texture is transparent above its sloping top edge.
        # Keep text and the complete badge bounce inside its opaque content area.
        for role in ('TITLE','DELTA','TIER','MMR'):
            row = rows['OCTR_RESULT_'+role]
            x,y = struct.unpack_from('<2f',g.data,row+0x60)
            width,height = struct.unpack_from('<2f',g.data,row+0x90)
            self.assertGreaterEqual(x-width/2,1500)
            self.assertLessEqual(x+width/2,1870)
            self.assertGreaterEqual(y-height/2,810)
            self.assertLessEqual(y+height/2,960)
            asset = named[handles[g.u64(row+0x38)&0x7fffffff]]
            self.assertEqual(g.data[asset+0x68],1)  # Native forceFitInBox.
            self.assertEqual(g.data[asset+0x69],0)  # Never wrap labels into another row.
            self.assertEqual(g.u32(asset+0x60),1)  # Native vertical center.
            self.assertEqual(g.u64(asset+0x20),Igz(native).u64(0x7cf0+0x20))
            self.assertEqual(g.u64(asset+0x30),Igz(native).u64(0x7cf0+0x30))
        for a,b in (('TITLE','DELTA'),('DELTA','TIER'),('TIER','MMR')):
            ay,ah = (struct.unpack_from('<f',g.data,rows['OCTR_RESULT_'+a]+offset)[0]
                     for offset in (0x64,0x94))
            by,bh = (struct.unpack_from('<f',g.data,rows['OCTR_RESULT_'+b]+offset)[0]
                     for offset in (0x64,0x94))
            self.assertLess(ay+ah/2,by-bh/2)

        for name in ('FX_UP','FX_DOWN'):
            self.assertTrue(g.u32(rows['OCTR_RESULT_'+name]+12)&0x400000)
        self.assertIn('Octane_AAR_WumpaOnly',g.strings)
        native_reward=Igz(reward)
        name_list=next(o for o,t in native_reward.objects() if t=='igNameList')
        effects={native_reward.string_at(native_reward.u64(name_list+32)+i*16)
                 for i in range(native_reward.u32(name_list+12))
                 if native_reward.types[native_reward.u64(native_reward.u64(native_reward.u64(32)+i*8))]=='igGuiEffect'}
        effect_handles=[(g.strings[a],g.strings[b&0x7fffffff])
                        for a,b in struct.iter_unpack('<II',exnm[3][:exnm[1]*8]) if b&0x80000000]
        for tag in ('FX_UP','FX_DOWN'):
            asset,namespace=effect_handles[g.u64(rows['OCTR_RESULT_'+tag]+0x38)&0x7fffffff]
            self.assertEqual(namespace,'Octane_AAR_WumpaOnly')
            self.assertIn(asset,effects)
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
            from igz_file import unpack_offsets
            fields = [(g.strings[a], g.strings[b & 0x7fffffff])
                      for a,b in struct.iter_unpack('<II',exnm[3][:exnm[1]*8])
                      if not b & 0x80000000]
            self.assertIn(track+0x18, g.read_list('RNEX',unpack_offsets))
            self.assertLess(g.u64(track+0x18),len(fields))
            self.assertEqual(fields[g.u64(track+0x18)],('igGuiPlaceable::_scale','metafield'))
            keys = g.u64(track+0x10)
            values = [struct.unpack_from('<2f',g.data,g.u64(g.u64(keys+0x20)+8*k)+0x18)
                      for k in range(g.u32(keys+12))]
            scales.append(values)
        self.assertEqual(scales[0],list(reversed(scales[1])))
        peak = max(v for animation in scales for key in animation for v in key)
        x,y = struct.unpack_from('<2f',g.data,icon+0x60)
        self.assertGreaterEqual(x-50*peak,1290)
        self.assertLessEqual(x+50*peak,1505)
        self.assertGreaterEqual(y-50*peak,750)
        self.assertLessEqual(y+50*peak,1080)  # Badge bounce may extend beyond the card.
        self.assertEqual(scales[0][-1],(0,0))
        self.assertAlmostEqual(scales[1][-1][0],1.35,places=5)
        # Sample the shipped backing texture's actual DXT5 alpha, including its padding.
        from button_prompts import _objects, IMG_W, IMG_DATA, _morton
        pak = Path(os.environ['BB_TEST_GAME']) / 'archives/juicedomain_permanent.pak'
        entry = next(e for e in iga.pak_entries(pak)
                     if '!octane_ui_buttonlegend_bg`' in e[0].lower())
        with pak.open('rb') as f:
            texture = iga.read_entry(f,*entry[1:])
        objects,_,file_offset = _objects(texture)
        image = next(o for o,t in objects if t == 'igImage2')
        width,height = struct.unpack_from('<HH',texture,image+IMG_W)
        _,data = struct.unpack_from('<QQ',texture,image+IMG_DATA)
        raw = texture[file_offset(data):]
        px,py = struct.unpack_from('<2f',g.data,panel+0x60)
        pw,ph = struct.unpack_from('<2f',g.data,panel+0x90)
        def backing_alpha(x,y):
            tx = int((x-px+pw/2)/pw*width)
            ty = height-1-int((y-py+ph/2)/ph*height)
            self.assertTrue(0 <= tx < width and 0 <= ty < height)
            bx,by = tx//4,ty//4
            pitch = ((width+3)//4+7)//8
            at = (((by>>3)*pitch+(bx>>3))*64+_morton(bx&7,by&7))*16
            a,b = raw[at:at+2]
            values = [a,b]+([(a*(7-i)+b*i)//7 for i in range(1,7)] if a>b else
                            [(a*(5-i)+b*i)//5 for i in range(1,5)]+[0,255])
            bits = int.from_bytes(raw[at+2:at+8],'little')
            return values[(bits>>(3*((ty%4)*4+tx%4)))&7]
        boxes = [(x,y,135,135)]
        for role in ('TITLE','DELTA','TIER','MMR'):
            row = rows['OCTR_RESULT_'+role]
            boxes.append((*struct.unpack_from('<2f',g.data,row+0x60),
                          *struct.unpack_from('<2f',g.data,row+0x90)))
        for x,y,box_width,box_height in boxes:
            for cx in (x-box_width/2,x+box_width/2):
                for cy in (y-box_height/2,y+box_height/2):
                    self.assertGreaterEqual(backing_alpha(cx,cy),96,
                                            'content escapes visible native backing')

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
        self.assertEqual(g.strings[:len(Igz(native).strings)], Igz(native).strings)
