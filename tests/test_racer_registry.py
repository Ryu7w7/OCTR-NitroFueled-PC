"""racer_registry.py: racers of several mods merged into one registration. The files are built
here (no game data): IGZ files shaped like the game's systemData.igz, .lng and
characterlisting.igz, with the fixups the merge reads and writes."""
import struct
import tempfile
import unittest
from pathlib import Path

import paths  # noqa: F401
import racer_registry as rr
from graphics_tab import write_archive
from igz_file import MAGIC, pack_offsets, write_strings

SIZES = {'igObjectList': 0x28, 'igNameList': 0x28, 'igStringRefList': 0x28, 'CSharedUnlock': 0x10,
         'CRacerDriverDataList': 0x28, 'CRacerDriverData': 0xA8, 'CDriverSkinDataList': 0x28,
         'CDriverSkinData': 0x80, 'COutfitData': 0x40, 'COutfitMaterialPackageList': 0x28,
         'COutfitMaterialPackage': 0x18, 'igLocalizedStringDataList': 0x28, 'igLocalizedStringData': 0x28}
FIELD = 'CRacerBaseData::_displayName'


class Builder:
    """An IGZ v10 file put together object by object."""

    def __init__(self, types):
        self.types = list(types)
        self.data = bytearray()
        self.strings, self.named, self.depends = [], [], []
        self.sites = {t: [] for t in ('RVTB', 'RSTT', 'ROFS', 'RHND', 'RNEX')}

    def sidx(self, text):
        if text not in self.strings:
            self.strings.append(text)
        return self.strings.index(text)

    def raw(self, n):
        self.data += bytes(-len(self.data) % 8)
        at = len(self.data)
        self.data += bytes(n)
        return at

    def obj(self, kind):
        at = self.raw(SIZES[kind])
        struct.pack_into('<Q', self.data, at, self.types.index(kind))
        self.sites['RVTB'].append(at)
        return at

    def string(self, at, text):
        struct.pack_into('<Q', self.data, at, self.sidx(text))
        self.sites['RSTT'].append(at)

    def ref(self, at, target):
        struct.pack_into('<Q', self.data, at, target)
        self.sites['ROFS'].append(at)

    def handle(self, at, name, space, named=True, tag='RHND'):
        flag = 0x80000000 if named else 0
        kind = [e for e in self.named if (e[1] & 0x80000000) == flag]
        entry = [self.sidx(name), self.sidx(space) | flag]
        index = kind.index(entry) if entry in kind else len(kind)
        if entry not in kind:
            self.named.append(entry)
        struct.pack_into('<I', self.data, at, index | flag)
        self.sites[tag].append(at)

    def lst(self, kind, items, names=False):
        at = self.obj(kind)
        size = 16 if names else 8
        block = self.raw(size * len(items))
        for i, item in enumerate(items):
            if names:
                self.string(block + 16 * i, item)
                struct.pack_into('<I', self.data, block + 16 * i + 8, rr.fnv1a(item))
            elif kind == 'igStringRefList':
                self.string(block + 8 * i, item)
            else:
                self.ref(block + 8 * i, item)
        struct.pack_into('<II', self.data, at + 0xC, len(items), len(items))
        struct.pack_into('<Q', self.data, at + 0x18, size * len(items))
        self.ref(at + 0x20, block)
        return at

    def build(self, root=None, names=None):
        blocks = []
        if self.depends:
            body = b''.join(s.encode() + b'\0' for s in self.depends)
            blocks.append(('TDEP', len(self.depends) // 2, body + bytes(4 - len(body) % 4)))
        blocks += [('TSTR', len(self.strings), write_strings(self.strings)),
                   ('TMET', len(self.types), write_strings(self.types)),
                   ('MTSZ', len(self.types), b''.join(struct.pack('<I', SIZES[t]) for t in self.types))]
        if self.named:
            blocks.append(('EXNM', len(self.named), b''.join(struct.pack('<II', *e) for e in self.named)))
        for tag, sites in self.sites.items():
            if sites:
                blocks.append((tag, len(sites), pack_offsets(sorted(sites))))
        if root is not None:
            blocks += [('ROOT', 1, struct.pack('<I', root)), ('ONAM', 1, struct.pack('<I', names))]
        fixups = bytearray()
        for tag, count, body in blocks:
            body += bytes(-len(body) % 4)
            fixups += struct.pack('<4sIII', tag.encode(), count, 16 + len(body), 16) + body
        head = bytearray(0x800)
        struct.pack_into('<5I', head, 0, MAGIC, 10, 0, 0, len(blocks))
        struct.pack_into('<4I', head, 0x14, 0, 0x800, len(fixups), 16)
        struct.pack_into('<4I', head, 0x24, 0, 0x800 + len(fixups), len(self.data), 16)
        return bytes(head + fixups + self.data)


def system(racers):
    """A systemData.igz: racers [(Name, [(skin display name, outfit)])], every racer pointing at
    one shared object, as the game's do."""
    b = Builder(['igObjectList', 'igNameList', 'CSharedUnlock', 'CRacerDriverDataList', 'CRacerDriverData',
                 'CDriverSkinDataList', 'CDriverSkinData', 'COutfitData', 'COutfitMaterialPackageList',
                 'COutfitMaterialPackage'])
    named = []
    shared = b.obj('CSharedUnlock')
    named.append(('SharedUnlock', shared))
    drivers = []
    for name, skins in racers:
        d = b.obj('CRacerDriverData')
        b.string(d + 0x10, name)
        b.ref(d + 0x30, shared)
        b.string(d + 0x58, 'Driver' + name)
        b.handle(d + 0x70, f'Driver{name}_Racer_Icon', f'Driver{name}_HUD_materials')
        b.string(d + 0x80, name)
        b.depends += [f'Driver{name}_HUD_materials', f'materials:/Drivers/Driver{name}_HUD_materials.igz']
        named.append(('SystemData_availableDrivers_' + name, d))
        objects = []
        for display, outfit in skins:
            s = b.obj('CDriverSkinData')
            b.string(s + 0x10, display)
            b.handle(s + 0x58, f'Driver{name}_Racer_Icon', f'Driver{name}_HUD_materials')
            o = b.obj('COutfitData')
            b.string(o + 0x10, f'Driver{name}_Outfit_{outfit}_skins')
            b.string(o + 0x18, f'Driver{name}_Outfit_{outfit}_Cutscene_skins')
            p = b.obj('COutfitMaterialPackage')
            b.string(p + 0x10, f'Driver{name}_Outfit_{outfit}_materials')
            b.ref(o + 0x28, b.lst('COutfitMaterialPackageList', [p]))
            b.ref(s + 0x68, o)
            objects.append(s)
            named += [(f'Skin_{name}_{outfit}', s), (f'Driver{name}_outfits_{outfit}', o)]
        b.ref(d + 0x98, b.lst('CDriverSkinDataList', objects))
        drivers.append(d)
    named.insert(0, ('SystemData_availableDrivers', b.lst('CRacerDriverDataList', drivers)))
    names = b.lst('igNameList', [n for n, _ in named], names=True)
    root = b.lst('igObjectList', [o for _, o in named])
    return b.build(root, names)


def language(racers):
    """A systemData_<language>.lng with each racer's name and skin names."""
    b = Builder(['igObjectList', 'igNameList', 'igLocalizedStringDataList', 'igLocalizedStringData'])
    entries = []
    for name, skins in racers:
        for target, text in [('SystemData_availableDrivers_' + name, name)] + \
                [(f'Skin_{name}_{outfit}', display) for display, outfit in skins]:
            e = b.obj('igLocalizedStringData')
            b.string(e + 0x10, text)
            b.handle(e + 0x18, target, 'systemData')
            b.handle(e + 0x20, FIELD, 'metafield', named=False, tag='RNEX')
            entries.append(e)
    strings = b.lst('igLocalizedStringDataList', entries)
    names = b.lst('igNameList', ['igLocalizedStringDataList0'], names=True)
    return b.build(b.lst('igObjectList', [strings]), names)


def listing(racers):
    b = Builder(['igStringRefList'])
    b.lst('igStringRefList', ['Driver' + n for n, _ in racers])
    return b.build()


def registration(racers, extra=None):
    entries = {rr.SYSTEM: system(racers), rr.LISTING: listing(racers),
               'looseData/systemData_en.lng': language(racers), 'looseData/systemData_fr.lng': language(racers)}
    entries.update(extra or {})
    return entries


GAME = [('Crash', [('Crash', 'Default')]), ('Coco', [('Coco', 'Default')])]
MIKU = ('Miku', [('Miku', 'Default'), ('Miku Project Diva', 'Diva')])
OLD_MIKU = ('Miku', [('Miku', 'Default')])  # a copy another mod carried, before the second outfit
GWEN = ('Gwen', [('Gwen', 'Default')])
HUD = 'materialinstances/Drivers/DriverMiku_HUD_materials.igz'
ICONS = 'materialinstances/UI/CustomizationIcons_materials.igz'


def skins(entries, racer):
    z = rr.File(entries[rr.SYSTEM])
    d = next(o for o in z.items(z.drivers()) if z.string_at(o + 0x58) == racer)
    return [z.string_at(s + 0x10) for s in z.items(z.ref(d + 0x98))]


def texts(entries, path):
    z = rr.File(entries[path])
    return sorted((z.handle(z.u32(o + 0x18))[0], z.handle(z.u32(o + 0x20))[0], z.string_at(o + 0x10))
                  for o in z.items(z.find('igLocalizedStringDataList')))


def listed(entries):
    z = rr.File(entries[rr.LISTING])
    lst = z.find('igStringRefList')
    return [z.string_at(z.ref(lst + 0x20) + 8 * i) for i in range(z.u32(lst + 0xC))]


class MergeTests(unittest.TestCase):
    def setUp(self):
        self.miku = registration(GAME + [MIKU], {HUD: b'hud of Miku 2', ICONS: b'icons with Racer_Skin_Miku_Diva'})
        self.gwen = registration(GAME + [OLD_MIKU, GWEN], {HUD: b'hud of Miku 1', ICONS: b'icons with Miku 1',
                                                           'materialinstances/Drivers/DriverGwen_HUD_materials.igz': b'gwen'})
        self.mods = [('miku', self.miku, ['archives/drivermiku.pak', 'archives/update4.pak']),
                     ('gwen', self.gwen, ['archives/drivergwen.pak', 'archives/update5.pak'])]
        self.game = registration(GAME)

    def test_file_round_trip(self):
        for blob in self.miku.values():
            if blob[:4] == struct.pack('<I', MAGIC):
                self.assertEqual(rr.File(blob).build(), blob)

    def test_racers_of_both_mods_from_their_own_mod(self):
        merged, added = rr.merge(self.game, self.mods, log=lambda _: None)
        self.assertEqual(added, [('DriverMiku', 'miku'), ('DriverGwen', 'gwen')])
        self.assertEqual(rr.driver_names(merged[rr.SYSTEM]), ['DriverCrash', 'DriverCoco', 'DriverMiku', 'DriverGwen'])
        # Miku comes from her own mod (two outfits), not from the copy the later Gwen mod carries.
        self.assertEqual(skins(merged, 'DriverMiku'), ['Miku', 'Miku Project Diva'])
        self.assertEqual(skins(merged, 'DriverGwen'), ['Gwen'])
        self.assertEqual(listed(merged), ['DriverCrash', 'DriverCoco', 'DriverMiku', 'DriverGwen'])
        self.assertEqual(texts(merged, 'looseData/systemData_en.lng'),
                         texts(registration(GAME + [MIKU, GWEN]), 'looseData/systemData_en.lng'))
        # Their own entries: Miku's from the Miku mod even though the Gwen mod loads later.
        self.assertEqual(merged[HUD], b'hud of Miku 2')
        self.assertEqual(merged[ICONS], b'icons with Racer_Skin_Miku_Diva')
        self.assertEqual(merged['materialinstances/Drivers/DriverGwen_HUD_materials.igz'], b'gwen')

    def test_copied_racer_has_its_handles_names_and_dependencies(self):
        merged, _ = rr.merge(self.game, self.mods, log=lambda _: None)
        z = rr.File(merged[rr.SYSTEM])
        roots = dict(z.roots()[2])
        for name in ('Miku', 'Gwen'):
            d = roots['SystemData_availableDrivers_' + name]
            self.assertEqual(z.handle(z.u32(d + 0x70)), (f'Driver{name}_Racer_Icon', f'Driver{name}_HUD_materials'))
            self.assertIn(f'Driver{name}_HUD_materials', z.depends[0::2])
            # The shared object is referenced, not copied.
            self.assertEqual(z.ref(d + 0x30), roots['SharedUnlock'])
        self.assertIn('Skin_Miku_Diva', roots)
        self.assertEqual(len([o for o, t in z.typed.items() if t == 'CSharedUnlock']), 1)
        names = z.roots()[1]
        data = z.ref(names + 0x20)
        for i in range(z.u32(names + 0xC)):  # the name list keeps each name's hash
            self.assertEqual(z.u32(data + 16 * i + 8), rr.fnv1a(z.string_at(data + 16 * i)))
        # The merged file reads back the same (its fixups are consistent).
        for blob in merged.values():
            if blob[:4] == struct.pack('<I', MAGIC):
                self.assertEqual(rr.File(blob).build(), blob)

    def test_a_racer_nobody_ships_comes_from_the_last_registration(self):
        mods = [('pack', registration(GAME + [OLD_MIKU]), ['archives/update4.pak']),
                ('newer pack', registration(GAME + [MIKU]), ['archives/update5.pak'])]
        merged, added = rr.merge(self.game, mods, log=lambda _: None)
        self.assertEqual(added, [('DriverMiku', 'newer pack')])
        self.assertEqual(skins(merged, 'DriverMiku'), ['Miku', 'Miku Project Diva'])

    def test_game_racers_stay_as_the_game_has_them(self):
        merged, added = rr.merge(self.game, [('a', registration(GAME), []), ('b', registration(GAME + [GWEN]), [])],
                                 log=lambda _: None)
        self.assertEqual(added, [('DriverGwen', 'b')])
        self.assertEqual(rr.driver_names(merged[rr.SYSTEM]), ['DriverCrash', 'DriverCoco', 'DriverGwen'])


class PrepareTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.game = self.root / 'game'
        (self.game / 'archives').mkdir(parents=True)
        self.pak(self.game / 'archives/update2.pak', registration(GAME))
        self.files = self.root / 'files'

    def tearDown(self):
        self.tmp.cleanup()

    def pak(self, path, entries):
        class Template:
            flags = 1
        path.parent.mkdir(parents=True, exist_ok=True)
        write_archive(path, [(rr.PREFIX + p, p, i << 8, d) for i, (p, d) in enumerate(entries.items())], Template)
        return path

    def test_one_registration_is_left_alone(self):
        miku = self.pak(self.root / 'miku/update4.pak', registration(GAME + [MIKU]))
        other = self.pak(self.root / 'other/update9.pak', {'something.igz': b'x'})
        self.assertIsNone(rr.prepare(self.game, self.files, [
            ('miku', 'archives/update4.pak', miku, ['archives/update4.pak']),
            ('other', 'archives/update9.pak', other, ['archives/update9.pak'])], log=lambda _: None))
        self.assertFalse(self.files.exists())

    def test_two_registrations_become_the_highest_archive(self):
        miku = self.pak(self.root / 'miku/update4.pak', registration(GAME + [MIKU]))
        gwen = self.pak(self.root / 'gwen/update5.pak', registration(GAME + [OLD_MIKU, GWEN]))
        logs = []
        target = rr.prepare(self.game, self.files, [
            ('gwen', 'archives/update5.pak', gwen, ['archives/drivergwen.pak', 'archives/update5.pak']),
            ('miku', 'archives/update4.pak', miku, ['archives/drivermiku.pak', 'archives/update4.pak'])],
            log=logs.append)
        self.assertEqual(target, 'archives/update5.pak')
        merged = rr.read_archive(self.files / 'archives/update5.pak')
        self.assertEqual(rr.driver_names(merged[rr.SYSTEM]), ['DriverCrash', 'DriverCoco', 'DriverMiku', 'DriverGwen'])
        self.assertEqual(skins(merged, 'DriverMiku'), ['Miku', 'Miku Project Diva'])
        self.assertEqual(logs, ['Racer registration: Miku (miku), Gwen (gwen) merged into one'])

    def test_a_racer_that_cannot_be_copied_names_the_mods(self):
        broken = registration(GAME + [GWEN])
        z = rr.File(broken[rr.SYSTEM])
        d = dict(z.roots()[2])['SystemData_availableDrivers_Gwen']
        z.set_u64(d + 0x30, z.u64(d + 0x30) + 8)  # a reference into the middle of an object
        broken[rr.SYSTEM] = z.build()
        miku = self.pak(self.root / 'miku/update4.pak', registration(GAME + [MIKU]))
        gwen = self.pak(self.root / 'gwen/update5.pak', broken)
        with self.assertRaisesRegex(ValueError, 'Racer mods miku, gwen .* could not be merged'):
            rr.prepare(self.game, self.files, [('miku', 'archives/update4.pak', miku, []),
                                               ('gwen', 'archives/update5.pak', gwen, [])], log=lambda _: None)


if __name__ == '__main__':
    unittest.main()
