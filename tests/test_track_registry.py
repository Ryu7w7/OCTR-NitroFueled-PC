"""track_registry.py: tracks registered in the game's track menus. The files are built here (no game
data): IGZ files shaped like guisystemdata.igz, Octane_LevelSelect_Proto.igz, the streaming
packages and LevelSelect_materials.igz, with the fixups the registration reads and writes.
With BB_TEST_GAME set to a USA 1.21 game folder, the real files are registered too."""
import os
import struct
import unittest
from pathlib import Path

import paths  # noqa: F401
import test_racer_registry as trr
import track_registry as tr
from igz_merge import MASK, File

trr.SIZES.update({
    'CGuiOctaneAreaLevelList': 0x28, 'CGuiOctaneLevelData': 0x20, 'CGuiOctaneLevelSelectDataList': 0x28,
    'CGuiOctaneLevelSelectData': 0x38, 'igGuiAnimationTag': 0x10, 'igStreamingChunkInfo': 0x40,
    'igGraphicsMaterial': 0x68, 'igMemoryCommandStream': 0x58, 'igGraphicsObjectSet': 0x28,
    'igGraphicsTexture': 0x38, 'igGraphicsSamplerStateBundle': 0x10, 'CStandIn': 0x10})

# The game's pages: area list number, menu list name, maps. The menu list names one map in
# another case, as the game's files do.
PAGES = [(0, 'NSanityBeach', ['octane/t111_crash_cove/t111_crash_cove', 'octane/t112_roos_tubes/t112_roos_tubes']),
         (2, 'LostRuins', ['octane/t133_papus_pyramid/t133_papus_pyramid']),
         (7, '6', ['octane/t504_october_spooky/t504_october_spooky'])]
SUZUKA = 'octane/t922_suzuka/t922_suzuka'
NBC = 'octane/t929_neo_bowser_city/t929_neo_bowser_city'
TEXTURE = 'GuiStandard_diffuse,textures@!ui!Octane!RaceSelect!octane_ui_raceselect_bg_suzuka`tga,101'
PAPU = 'GuiStandard_diffuse,textures@!ui!Octane!RaceSelect!octane_ui_raceselect_bg_papu`tga,101'


def named_file(b, named):
    names = b.lst('igNameList', [n for n, _ in named], names=True)
    root = b.lst('igObjectList', [o for _, o in named])
    return b.build(root, names)


def gui(pages=PAGES):
    b = trr.Builder(['igObjectList', 'igNameList', 'CGuiOctaneAreaLevelList', 'CGuiOctaneLevelData'])
    named = []
    for area, _, maps in pages:
        levels = []
        for m in maps:
            o = b.obj('CGuiOctaneLevelData')
            b.string(o + 0x10, m)
            levels.append(o)
        named.append((tr.AREA + str(area), b.lst('CGuiOctaneAreaLevelList', levels)))
    return named_file(b, named)


def select(pages=PAGES, extra=False):
    """The track menu; `extra`: another mod's object in it (Custom Cup's thumbnails)."""
    b = trr.Builder(['igObjectList', 'igNameList', 'CGuiOctaneLevelSelectDataList', 'CGuiOctaneLevelSelectData',
                     'igGuiAnimationTag', 'CStandIn'])
    selected = b.obj('igGuiAnimationTag')
    deselected = b.obj('igGuiAnimationTag')
    named = [('AnimationTag_Selected', selected), ('AnimationTag_Deselected', deselected)]
    for _, page, maps in pages:
        records = []
        for m in maps:
            o = b.obj('CGuiOctaneLevelSelectData')
            b.string(o + 0x10, m.replace('october', 'October'))
            b.handle(o + 0x18, 'AnimationTag_BTN_Pressed', 'Menu_ButtonWithIcon', tag='RNEX')
            b.ref(o + 0x20, selected)
            b.ref(o + 0x28, deselected)
            records.append(o)
            named.append((f'{tr.POPULATOR}{page}_{len(records)}', o))
        named.append((tr.POPULATOR + page, b.lst('CGuiOctaneLevelSelectDataList', records)))
    named.append((tr.POPULATOR + 'Empty', b.lst('CGuiOctaneLevelSelectDataList', [])))
    if extra:
        named.append(('CustomCup_Thumbnail', b.obj('CStandIn')))
    return named_file(b, named)


def package(entries):
    """A streaming package: igStreamingChunkInfo with (kind, path) entries."""
    b = trr.Builder(['igObjectList', 'igNameList', 'igStreamingChunkInfo'])
    info = b.obj('igStreamingChunkInfo')
    block = b.raw(16 * len(entries))
    for i, (kind, path) in enumerate(entries):
        b.string(block + 16 * i, kind)
        b.string(block + 16 * i + 8, path)
    struct.pack_into('<IIQ', b.data, info + 0x28, len(entries), 0, 0x0110000000000000 | 16 * len(entries))
    b.ref(info + 0x38, block)
    return named_file(b, [('chunk_info', info)])


def materials(pictures=('PapusPyramid', 'CrashCove')):
    """Picture materials as the game's: material -> two command streams (one through a data
    block), an object set whose data block holds the texture and shared settings."""
    b = trr.Builder(['igObjectList', 'igNameList', 'igGraphicsMaterial', 'igMemoryCommandStream',
                     'igGraphicsObjectSet', 'igGraphicsTexture', 'igGraphicsSamplerStateBundle'])
    sampler = b.obj('igGraphicsSamplerStateBundle')
    named = [('Sampler', sampler)]
    for picture in pictures:
        texture_name = PAPU if picture == 'PapusPyramid' else PAPU.replace('papu', picture.lower())
        material = b.obj('igGraphicsMaterial')
        b.string(material + 0x60, 'CGuiMaterial')
        b.handle(material + 0x28, 'graphics_effect', 'GuiStandard', tag='RHND')
        stream = b.obj('igMemoryCommandStream')
        commands = b.raw(0xA0)
        struct.pack_into('<Q', b.data, stream + 0x40, 0x01200000000000A0)
        b.ref(stream + 0x48, commands)
        b.ref(material + 0x30, stream)
        second = b.obj('igMemoryCommandStream')
        holder = b.raw(8)
        b.ref(holder, second)
        struct.pack_into('<Q', b.data, material + 0x40, 0x0110000000000008)
        b.ref(material + 0x48, holder)
        texture = b.obj('igGraphicsTexture')
        b.handle(texture + 0x28, 'image', texture_name)
        b.depends += [texture_name, f'textures:/{texture_name}.igz']
        objects = b.obj('igGraphicsObjectSet')
        items = b.raw(16)
        b.ref(items, texture)
        b.ref(items + 8, sampler)
        struct.pack_into('<Q', b.data, objects + 0x18, 0x0110000000000010)
        b.ref(objects + 0x20, items)
        b.ref(material + 0x58, objects)
        named.append((tr.IMAGE + picture, material))
    return named_file(b, named)


def game():
    menu = [('vsc', 'vsc/ui/menu.igz'), ('texture', 'textures/a.igz'),
            ('material_instances', 'materialinstances/ui/levelselect_materials.igz')]
    return {'loosedata/guisystemdata.igz': gui(), 'ui/Octane_LevelSelect_Proto.igz': select(),
            tr.CHUNKS: package([('igx_file', 'maps/octane/t111_crash_cove/t111_crash_cove_zoneinfo.igz')]),
            tr.MATERIALS: materials(), tr.DOMAINS[0]: package(menu), tr.DOMAINS[1]: package(menu)}


def page_maps(entries):
    return [(p[0], p[1], p[2]) for p in tr.pages(entries[tr.GUI], entries[tr.SELECT])]


def chunks(blob):
    return tr.chunk_entries(File(blob))[1]


class TrackRegistryTests(unittest.TestCase):
    def register(self, tracks, entries=None):
        entries = dict(entries or game())
        out = tr.register(entries, tracks, log=lambda _: None)
        for blob in out.values():
            self.assertEqual(File(blob).build(), blob)  # the files read back as they were written
        return entries, {**entries, **out}

    def test_pages_pair_each_area_with_its_menu_list(self):
        self.assertEqual([(a, p) for a, p, _ in page_maps(game())],
                         [(tr.AREA + '0', tr.POPULATOR + 'NSanityBeach'), (tr.AREA + '2', tr.POPULATOR + 'LostRuins'),
                          (tr.AREA + '7', tr.POPULATOR + '6')])

    def test_a_track_goes_to_the_end_of_its_page_and_the_others_stay(self):
        before, after = self.register([tr.Track(NBC, page=tr.AREA + '0')])
        old, new = page_maps(before), page_maps(after)
        self.assertEqual(new[0][2], PAGES[0][2] + [NBC])
        self.assertEqual(new[1:], old[1:])
        z = File(after[tr.SELECT])
        record = z.items(tr.named(z)[tr.POPULATOR + 'NSanityBeach'])[-1]
        tags = {name for name, o in tr.named(z).items() if name.startswith('AnimationTag')}
        self.assertEqual(z.string_at(record + 0x10), NBC)
        # The button's animations are the game's own objects, not copies.
        self.assertEqual({o for n, o in tr.named(z).items() if n in tags},
                         {z.ref(record + 0x20), z.ref(record + 0x28)})
        self.assertEqual(z.handle(z.u32(record + 0x18)), ('AnimationTag_BTN_Pressed', 'Menu_ButtonWithIcon'))

    def test_without_a_page_the_track_goes_to_the_last_page(self):
        _, after = self.register([tr.Track(SUZUKA)])
        self.assertEqual(page_maps(after)[-1][2], PAGES[-1][2] + [SUZUKA])

    def test_tracks_keep_the_order_they_are_given(self):
        _, after = self.register([tr.Track(SUZUKA), tr.Track(NBC)])
        self.assertEqual(page_maps(after)[-1][2][-2:], [SUZUKA, NBC])

    def test_other_changes_to_the_menu_stay(self):
        entries = game()
        entries[tr.SELECT] = select(extra=True)
        _, after = self.register([tr.Track(SUZUKA)], entries)
        self.assertIn('CustomCup_Thumbnail', tr.named(File(after[tr.SELECT])))

    def test_a_picture_is_a_copy_of_the_game_s_with_the_track_s_texture(self):
        before, after = self.register([tr.Track(SUZUKA, image='LevelImage_Suzuka', texture=TEXTURE)])
        old, z = File(before[tr.MATERIALS]), File(after[tr.MATERIALS])
        image = tr.named(z)['LevelImage_Suzuka']
        textures = [z.handle(z.u32(site)) for o in tr.owned(z, image) if z.typed[o] == 'igGraphicsTexture'
                    for tag, site in z.sites_in(o, z.size(o)) if tag == 'RHND']
        self.assertEqual(textures, [('image', TEXTURE)])
        self.assertIn(TEXTURE, z.depends)
        self.assertEqual(z.depends[z.depends.index(TEXTURE) + 1], f'textures:/{TEXTURE}.igz')
        # Its own material, streams, object set and texture; the shared sampler is referenced.
        self.assertEqual(len(z.typed) - len(old.typed), 5)
        sampler = tr.named(z)['Sampler']
        objects = z.ref(image + 0x58)
        self.assertEqual(z.ref(z.ref(objects + 0x20) + 8), sampler)
        # The picture it was copied from is unchanged.
        papu = tr.named(z)['LevelImage_PapusPyramid']
        self.assertEqual(z.data[papu:papu + 0x68], old.data[papu:papu + 0x68])
        self.assertIn(PAPU, z.depends)

    def test_the_picture_s_texture_goes_before_the_menu_materials_in_the_menus(self):
        _, after = self.register([tr.Track(SUZUKA, image='LevelImage_Suzuka', texture=TEXTURE)])
        for domain in tr.DOMAINS:
            self.assertEqual([p for _, p in chunks(after[domain])],
                             ['vsc/ui/menu.igz', 'textures/a.igz', tr.texture_file(TEXTURE).casefold(),
                              'materialinstances/ui/levelselect_materials.igz'])

    def test_a_track_s_own_files_stream_in(self):
        zone = ('igx_file', 'maps/octane/t922_suzuka/t922_suzuka_zoneinfo.igz')
        before, after = self.register([tr.Track(SUZUKA, files=[zone, ('igx_file', 'Maps/Octane/T111_Crash_Cove/'
                                                                       't111_crash_cove_zoneinfo.igz')])])
        self.assertEqual(chunks(after[tr.CHUNKS]), chunks(before[tr.CHUNKS]) + [zone])

    def test_a_track_without_its_own_picture_changes_no_material(self):
        before, after = self.register([tr.Track(NBC)])
        self.assertEqual(after[tr.MATERIALS], before[tr.MATERIALS])
        self.assertEqual(after[tr.DOMAINS[0]], before[tr.DOMAINS[0]])

    def test_no_tracks_change_nothing(self):
        self.assertEqual(tr.register(game(), [], log=lambda _: None), {})

    def test_a_map_registered_twice_is_refused(self):
        for tracks in ([tr.Track(PAGES[0][2][0])], [tr.Track(SUZUKA), tr.Track(SUZUKA.upper())]):
            with self.assertRaisesRegex(ValueError, 'already a track'):
                tr.register(game(), tracks, log=lambda _: None)

    def test_an_unknown_page_or_a_picture_without_texture_is_refused(self):
        with self.assertRaisesRegex(ValueError, 'no menu page'):
            tr.register(game(), [tr.Track(SUZUKA, page=tr.AREA + '9')], log=lambda _: None)
        with self.assertRaisesRegex(ValueError, 'without its texture'):
            tr.register(game(), [tr.Track(SUZUKA, image='LevelImage_Suzuka')], log=lambda _: None)
        with self.assertRaisesRegex(ValueError, 'already in LevelSelect_materials'):
            tr.register(game(), [tr.Track(SUZUKA, image='LevelImage_CrashCove', texture=TEXTURE)], log=lambda _: None)

    def test_keys_keep_the_case_the_entries_have(self):
        entries = {k.upper(): v for k, v in game().items()}
        out = tr.register(entries, [tr.Track(SUZUKA)], log=lambda _: None)
        self.assertTrue(out and set(out) <= set(entries))


@unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'BB_TEST_GAME: a USA 1.21 game folder')
class RealGameTests(unittest.TestCase):
    def test_suzuka_and_neo_bowser_city_register_in_the_game_s_files(self):
        from content_delta import Game
        game_files = Game(Path(os.environ['BB_TEST_GAME']))
        try:
            entries = {p: game_files.newest(p) for p in tr.FILES}
        finally:
            game_files.close()
        out = tr.register(entries, [
            tr.Track(SUZUKA, image='LevelImage_Suzuka', texture=TEXTURE,
                     files=[('igx_file', 'maps/octane/t922_suzuka/t922_suzuka_zoneinfo.igz')]),
            tr.Track(NBC, page=tr.AREA + '0')], log=lambda _: None)
        merged = {**entries, **out}
        layout = page_maps(merged)
        self.assertEqual(layout[0][2][-1], NBC)
        self.assertEqual(layout[-1][2][-1], SUZUKA)
        for blob in out.values():
            self.assertEqual(File(blob).build(), blob)
        z = File(merged[tr.SELECT])
        record = z.items(tr.named(z)[layout[-1][1]])[-1]
        self.assertEqual(z.typed[z.u64(record + 0x20) & MASK], 'igGuiAnimationTag')


if __name__ == '__main__':
    unittest.main()
