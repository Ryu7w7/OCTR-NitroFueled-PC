import os
import struct
import unittest
from pathlib import Path
from archive_reader import Archive
from graphics_tab import show_tab, graphics_page, WIDGET_RESOURCES, fnv1a
from igz_file import Igz, unpack_offsets
from prepare import parse_self
from widescreen_startup import IGZ

@unittest.skipUnless(os.environ.get("BB_TEST_GAME"), "requires native USA/EU dump")
class NativeGraphics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.game = Path(os.environ["BB_TEST_GAME"])

    def test_graphics_assets(self):
        wanted = {"ui/octane_options_menu.igz", "ui/octane_options_languageselect.igz",
                  "ui/guisharedassets/settingselector.igz", "ui/guisharedassets/menu_buttonwithicon.igz", "ui/octane_lobbymembers.igz",
                  "ui/octane_options_menu_page.igz",
                  "ui/materialinstances/guisharedassets/juice_settingselector_materials.igz",
                  "ui/materialinstances/juice_octane_lobbymembers_materials.igz",
                  "ui/materialinstances/juice_octane_options_menu_page_materials.igz",
                  "ui/materialinstances/juice_octane_archetype_materials.igz"} | WIDGET_RESOURCES
        blobs = {}
        available = set()
        always_available = set()
        for name in ("update2.pak", "update.pak", "juicedomain_permanent.pak", "juicedomain_onlinemenus.pak"):
            a = Archive(self.game / "archives" / name)
            try:
                for entry in a.files:
                    key = entry["path"].lower()
                    available.add(key)
                    if name != "juicedomain_onlinemenus.pak": always_available.add(key)
                    if key in wanted and key not in blobs: blobs[key] = a.extract(entry)
            finally:
                a.data.close(); a._file.close()
        self.assertEqual(set(blobs), wanted)
        # Ready's material has dependencies beyond the icon itself. Every texture
        # must be available before any online/race domain has been loaded.
        missing = set()
        for key in wanted:
            if '/materialinstances/' not in key: continue
            material = Igz(blobs[key]); d = material.fixup('TDEP')
            paths = d[3].split(b'\0')[:d[1]*2][1::2]
            for path in paths:
                if not path.startswith(b'textures:/'): continue
                logical = path.decode().replace(':/','/').lower()
                self.assertIn(logical, available)
                if logical not in always_available: missing.add(logical)
        self.assertEqual(missing, WIDGET_RESOURCES)
        old = Igz(blobs["ui/octane_options_menu.igz"])
        new = Igz(show_tab(old.blob))
        xy = lambda g, p: struct.unpack_from("<2f", g.data, p+0x60)
        previous, last = xy(old, 0x15b0), xy(old, 0x1490)
        self.assertEqual(xy(new, 0x1370), tuple(2*b-a for a,b in zip(previous,last)))
        # Runtime identity uses the native Options group and sibling chain.
        self.assertEqual(xy(old, 0xaf0), (510, -9))
        current = 0x1370
        for expected in (0x1490, 0x15b0, 0xc70, 0xd90, 0xeb0, 0xaf0):
            self.assertEqual(new.u64(current+0x40), 0xaf0)
            self.assertEqual(new.u64(current+0x50), expected)
            current = expected
        # Options opening clones these VSC objects before Graphics is selected.
        # Preserve every behavior type, dynamic-memory pointer and parameter block.
        self.assertEqual(new.data[0x1fe8:0x2098], old.data[0x1fe8:0x2098])
        self.assertEqual(new.data[0x2730:0x2798], old.data[0x2730:0x2798])
        # Added in-race position track relocates its own list/keyframe only.
        self.assertGreater(len(new.data), len(old.data))
        for tag in ("RHND", "RNEX", "ROFS", "RVTB"):
            self.assertEqual([p for p in new.read_list(tag, unpack_offsets) if p < len(old.data)],
                             old.read_list(tag, unpack_offsets))
        # A cloned position track must resolve its metafield before playback.
        # Without RNEX it remains integer 11 and crashes native animation at +0xc.
        cloned_tracks = [o for o, kind in new.objects()
                         if o >= len(old.data) and kind == 'igGuiVec3Track']
        self.assertEqual(len(cloned_tracks), 1)
        clone = cloned_tracks[0]
        self.assertIn(clone+0x18, new.read_list('RNEX', unpack_offsets))
        self.assertEqual(new.u64(clone+0x18), old.u64(0x2870+0x18))
        for tag in ('RVTB', 'RSTT', 'ROFS', 'RPID', 'RNEX'):
            expected = [clone + s - 0x2870 for s in old.read_list(tag, unpack_offsets)
                        if 0x2870 <= s < 0x28f8]
            actual = [s for s in new.read_list(tag, unpack_offsets) if clone <= s < clone+0x88]
            self.assertEqual(actual, expected)
        buttons = (0xeb0, 0xd90, 0xc70, 0x15b0, 0x1490, 0x1370, 0x1250)
        def layout(name):
            visible = []
            for button in buttons:
                position, alpha = xy(new, button), struct.unpack_from('<f', new.data, button+0xe0)[0]
                table = new.u64(button+0xf0)
                for i in range(new.u32(table+0x10)//8):
                    animation = new.u64(new.u64(table+0x18)+8*i)
                    if not animation: continue
                    tag = new.u64(animation+0x10)
                    if new.string_at(tag+0x10) != name: continue
                    tracks = new.u64(animation+0x18)
                    for j in range(new.u32(tracks+0xc)):
                        track = new.u64(new.u64(tracks+0x20)+8*j)
                        keys = new.u64(track+0x10)
                        key = new.u64(new.u64(keys+0x20))
                        if new.types[new.u64(track)] == 'igGuiVec3Track':
                            position = struct.unpack_from('<2f', new.data, key+0x18)
                        else:
                            alpha = struct.unpack_from('<f', new.data, key+0x18)[0]
                if alpha: visible.append((button, position))
            return visible
        for name, count in ((None, 7), ('NXLayout', 7), ('NoExtraButton', 6),
                            ('NoExtraAndTutorialsButtons', 5)):
            with self.subTest(layout=name):
                visible = layout(name)
                self.assertEqual(len(visible), count)
                self.assertEqual([b for b, _ in visible][-2:], [0x1370, 0x1250])
                # Check Privacy both available and hidden by the native platform gate.
                for rows in (visible, visible[:-1]):
                    for (_, a), (_, b) in zip(rows, rows[1:]):
                        self.assertEqual(tuple(y-x for x, y in zip(a, b)), (12.0, 105.0))
        # All focus/navigation links and behavior memory remain native.
        for obj, kind in old.objects():
            if kind == 'CGuiBehaviorOctaneNavigationItem':
                self.assertEqual(new.data[obj:obj+0x220], old.data[obj:obj+0x220])
        self.assertEqual(new.fixup("EXNM"), old.fixup("EXNM"))
        page = Igz(graphics_page(blobs["ui/octane_options_languageselect.igz"]))
        self.assertEqual(struct.unpack_from("<f",page.data,0x8b0+0xe0)[0],0)
        self.assertTrue(page.u32(0x8b0+12)&0x400)
        descriptions = [p for p,t in page.objects() if t == 'igGuiPlaceable' and
                        p+0x28 in page.sites['RSTT'] and
                        page.string_at(p+0x28) == 'BbGraphicsDescription']
        self.assertEqual(len(descriptions), 1)
        description = descriptions[0]
        self.assertEqual(struct.unpack_from('<2f',page.data,description+0x60), (494,950))
        self.assertEqual(struct.unpack_from('<3f',page.data,description+0x78), (0,0,0))
        self.assertEqual(struct.unpack_from('<4f',page.data,description+0xc0), (1,1,1,1))
        self.assertNotIn('BbGraphicsHelpSeparator', page.strings)
        self.assertEqual(struct.unpack_from('<f',page.data,0x13d0+0x94)[0], 140)
        self.assertEqual(struct.unpack_from('<4f',page.data,0x1080+0x50), (150,0,0,0))
        original_page = Igz(blobs["ui/octane_options_languageselect.igz"])
        # Native behavior objects and all existing fixup targets remain intact.
        for at, kind in original_page.objects():
            if 'Behavior' in kind and at not in (0x4280, 0x42b8):
                size = original_page.sizes[original_page.u64(at)]
                self.assertEqual(page.data[at:at+size], original_page.data[at:at+size])
        # Do not bring back the cross-file object relocation that blanked v25/v26.
        self.assertEqual(page.types, original_page.types)
        for tag in ('TMET','MTSZ','ROOT','ONAM'):
            self.assertEqual(page.fixup(tag), original_page.fixup(tag))
        self.assertEqual(page.u64(0x1518), original_page.u64(0x1518))
        self.assertEqual(page.data[0x4280:0x4290], original_page.data[0x4280:0x4290])
        self.assertEqual(page.data[0x4298:0x42b8], original_page.data[0x4298:0x42b8])
        # Native component references, with no custom visual children.
        pairs = list(struct.iter_unpack('<II', page.fixup('EXNM')[3][:page.fixup('EXNM')[1]*8]))
        handles = [(page.strings[a], page.strings[b&0x7fffffff]) for a,b in pairs if b>>31]
        objects = [(page.strings[a], page.strings[b]) for a,b in pairs if not b>>31]
        for field, name in ((0x148,'Sprite_octane_ui_raceselect_ico_crashcove_tga'),
                            (0x150,'Sprite_octane_ui_raceselect_ico_crashcove_tga001'),
                            (0x160,'AnimationTag_BTN_Deselected_Blue'),
                            (0x168,'AnimationTag_BTN_LOCKED_Initialize')):
            self.assertEqual(objects[page.u64(0x42b8+field)], (name,'Menu_ButtonWithIcon'))
        self.assertEqual(page.u64(0x42b8+0x138), original_page.u64(0x42b8+0x138))
        selector = 0x1520
        root_table = page.u64(0x13d0+0x48)
        self.assertEqual(page.u32(root_table+0x10), 2)
        button = page.u64(page.u64(root_table+0x20)+8)
        self.assertEqual(page.u64(0x4280+0x10), selector)
        self.assertEqual(page.u64(0x13d0+0x50), selector)
        self.assertEqual(page.u64(selector+0x58), 0x13d0)
        self.assertEqual(page.u64(selector+0x50), button)
        self.assertEqual(page.u64(button+0x58), selector)
        self.assertEqual(page.u64(button+0x50), 0)
        for p, name, namespace in ((selector,'SettingSelector_asset','SettingSelector'),
                                    (button,'Menu_ButtonWithIcon_asset','Menu_ButtonWithIcon')):
            self.assertEqual(page.u64(p+0x48), 0)
            self.assertEqual(handles[page.u64(p+0x38)&0x7fffffff], (name,namespace))
        # Same component hashes as native Gameplay / Options use.
        native_page = Igz(blobs['ui/octane_options_menu_page.igz'])
        for p,name,namespace in ((0x1c90,'SettingSelector_asset','SettingSelector'),
                                  (0x8b50,'Menu_ButtonWithIcon_asset','Menu_ButtonWithIcon')):
            self.assertEqual(struct.unpack_from('<II',native_page.fixup('EXID')[3],native_page.u64(p+0x38)*8),
                             (fnv1a(name),fnv1a(namespace)))
        for at,tag,button_tag in ((0x47f8,'AnimationTag_Focus','AnimationTag_BTN_Selected_Blue'),
                                 (0x4760,'AnimationTag_Defocus','AnimationTag_BTN_Deselected_Blue')):
            self.assertEqual(page.u32(at+0x10),2)
            data = page.u64(at+0x20)
            for i,expected,target in ((0,(tag,'SettingSelector'),selector),
                                      (1,(button_tag,'Menu_ButtonWithIcon'),button)):
                action = page.u64(data+8*i)
                self.assertEqual(objects[page.u64(action+0x28)],expected)
                self.assertEqual(page.u64(action+0x30),target)
                # The clone reads all 0x68 bytes, including pointer fields after
                # +0x50. Truncation made those fields alias the next action table.
                source = original_page.u64(original_page.u64(at+0x20))
                size = original_page.sizes[original_page.u64(source)]
                self.assertEqual(size, 0x68)
                self.assertEqual(page.data[action+0x38:action+size],
                                 original_page.data[source+0x38:source+size])
                if i == 1:
                    self.assertLessEqual(action+size, data)
        for field in (0x158,0x178):
            self.assertEqual(struct.unpack_from('<II', page.fixup('EXID')[3],
                                                page.u64(0x42b8+field)*8),
                             (0x77c5b5ee, 0x3985f623))
            ready = Igz(blobs['ui/octane_lobbymembers.igz'])
            self.assertEqual(page.fixup('EXID')[3][-8:], ready.fixup('EXID')[3][32:40])
        for key, required in {
            "ui/guisharedassets/settingselector.igz": {"SettingSelector_asset", "Text_NameText", "Text_Element", "AnimationTag_Focus", "AnimationTag_Defocus"},
            "ui/guisharedassets/menu_buttonwithicon.igz": {"Menu_ButtonWithIcon_asset", "Text_LevelListName", "AnimationTag_BTN_Selected_Blue", "AnimationTag_BTN_Deselected_Blue", "Sprite_octane_ui_raceselect_ico_crashcove_tga", "Sprite_octane_ui_raceselect_ico_crashcove_tga001"},
            "ui/octane_lobbymembers.igz": {"octane_ui_ico_ready_tga"},
            "ui/octane_options_menu_page.igz": {"ModeDescription"},
        }.items():
            self.assertTrue(required <= {o.get('name') for o in IGZ(blobs[key]).objects})
        # Every serialized pointer is within the data section, every object has a known type.
        for at in page.sites['ROFS']:
            self.assertLess(page.u64(at), len(page.data))
        for at, _ in page.objects():
            self.assertLess(page.u64(at), len(page.types))
        # Use the native packed TDEP format, not the aligned TSTR helper.
        old_deps = original_page.fixup('TDEP')
        new_deps = page.fixup('TDEP')
        original_strings = old_deps[3].split(b'\0')[:old_deps[1]*2]
        dependency_strings = new_deps[3].split(b'\0')[:new_deps[1]*2]
        self.assertEqual(dependency_strings[:len(original_strings)], original_strings)
        prefix = b'\0'.join(original_strings) + b'\0'
        self.assertTrue(new_deps[3].startswith(prefix))
        self.assertTrue(all(dependency_strings))
        self.assertEqual(len(dependency_strings), new_deps[1]*2)
        dependencies = [s.decode('utf-8') for s in dependency_strings]
        for path in dependencies[1::2]:
            self.assertTrue(path.startswith(('ui:/', 'sc:/', 'vsc:/', '<build>')), path)
        self.assertIn('SettingSelector', dependencies)
        self.assertIn('Octane_LobbyMembers', dependencies)

    def test_native_hook_contracts(self):
        elf, _, segments, *_ = parse_self((self.game / "eboot.bin").read_bytes())
        def read(address, size):
            p = next(p for p in segments if p["type"] in (1,0x61000010) and
                     p["vaddr"] <= address < p["vaddr"]+p["filesz"])
            off = p["offset"]+address-p["vaddr"]
            return elf[off:off+size]
        shift = 0x10 if "CUSA14876" in str(self.game) else 0
        self.assertEqual(read(0x250b20,11), bytes.fromhex('55 48 89 e5 41 57 41 56 41 54 53'))
        for address, code in [
            (0x10de0c9,"e8 42 7f f5 ff"),
            (0x1109200,"55 48 89 e5 41 57 41 56 41 55 41 54"),
            (0x1276c70,"55 48 89 e5 41 57 41 56 41 55 41 54"),
            (0x12ae8a0,"55 48 89 e5 41 56 53 49 89 f6 48 85 ff"),
            (0x11345b0,"55 48 89 e5 41 57 41 56 53 48 83 ec 18"),
        ]:
            expected = bytes.fromhex(code)
            self.assertEqual(read(address-shift,len(expected)), expected, hex(address))
        # igGuiList margin is a vec4 at +0x40; scroll speed is not layout spacing.
        self.assertEqual(read(0x1c0d7a0-(0x40 if shift else 0)+4*2,2), b"\x40\x00")
        # igGuiListItem field offsets: _isFocusable shares the +0x18 bitfield.
        self.assertEqual(read(0x1c0d850-(0x40 if shift else 0)+5*2,2), b"\x18\x00")
