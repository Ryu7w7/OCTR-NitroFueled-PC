"""Built-in mods: the library's built-in folders (mod_library.py), copy-delta payloads in format 2
packages (mod_components.py), the art rule (mod_packages.py) and tools/make_builtin_mods.py."""
import hashlib
import importlib.util
import json
import os
import shutil
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import paths  # noqa: F401
import mod_format as fmt
import mod_library
from mod_components import payload_bytes

spec = importlib.util.spec_from_file_location('make_builtin_mods', paths.ROOT / 'tools' / 'make_builtin_mods.py')
mbm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mbm)

ORIGINAL = bytes(range(256)) * 64
CHANGED = ORIGINAL[:5000] + b'the author bytes' + ORIGINAL[5000:9000] + ORIGINAL[100:400]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def package(folder, ident='race', version='1.0.0', delta=True):
    """A format 2 package changing one archive entry, its payload a copy delta or whole."""
    op = {'kind': 'archive_asset', 'archive': 'update2.pak', 'asset': 'ui/a.igz', 'index': 1,
          'original_sha256': sha(ORIGINAL), 'patched_sha256': sha(CHANGED)}
    if delta:
        files = {'payloads/a.delta.json': json.dumps(mbm.make_delta(ORIGINAL, CHANGED)).encode()}
    else:
        files = {'payloads/a.igz': CHANGED}
    op['payload'] = next(iter(files))
    manifest = {'schemaVersion': 2, 'id': ident, 'name': ident, 'version': version, 'author': 'Kyouko8',
                'description': 'test', 'requiresCapabilities': ['archive-assets'],
                'compatibility': {'online': 'allowed'}, 'operations': [op]}
    mbm.write_package(Path(folder), manifest, files)
    return manifest


class DeltaTests(unittest.TestCase):
    def test_a_delta_rebuilds_the_asset_and_keeps_only_new_bytes(self):
        delta = mbm.make_delta(ORIGINAL, CHANGED)
        literal = b''.join(bytes.fromhex(op[1]) for op in delta['operations'] if op[0] == 'bytes')
        self.assertEqual(literal, b'the author bytes')
        self.assertEqual([op[0] for op in delta['operations']], ['copy', 'bytes', 'copy', 'copy'])

    def test_a_package_s_delta_payload_is_applied_and_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            m = package(folder)
            op = m['operations'][0]
            self.assertEqual(payload_bytes(op, Path(folder), ORIGINAL), CHANGED)
            with self.assertRaisesRegex(ValueError, 'Original content checksum'):
                payload_bytes(op, Path(folder), ORIGINAL[:-1] + b'x')
            with self.assertRaisesRegex(ValueError, 'Patched content checksum'):
                payload_bytes({**op, 'patched_sha256': sha(b'x')}, Path(folder), ORIGINAL)


class SplitTests(unittest.TestCase):
    XML = '''<?xml version="1.0" encoding="utf-8"?>
<Patch>
    <TitleID><ID>CUSA13795</ID></TitleID>
    <Metadata Title="Crash Team Racing Nitro-Fueled" Name="Ultimate Customizer" Note="x" Author="Kyouko8"
              PatchVer="5.1.0" AppVer="01.21" AppElf="eboot.bin" isEnabled="false">
        <PatchList>
            <!-- Cup stars: the code for the added cups (their table, its file) -->
            <Line Type="bytes" Address="0x2738000" Value="90" Signature="90"/>
            <!-- Lap menu: lap counts with 3 digits -->
            <Line Type="bytes" Address="0x168d7a5" Value="67" Signature="66"/>
            <!-- Cup stars: an added cup reads its own table -->
            <Line Type="bytes" Address="0x13de1ac" Value="e9" Signature="0f"/>
            <!-- Random cup: Square on the focused cup button gives a random cup -->
            <Line Type="bytes" Address="0x167c950" Value="e9" Signature="74"/>
            <!-- Cup menu: find a cup by its first track among cup starts only (4 zones per cup) -->
            <Line Type="bytes" Address="0x167e750" Value="83" Signature="ff"/>
            <!-- Cup stars: the save data unmount reads and writes the table first -->
            <Line Type="bytes" Address="0x18d14c1" Value="ce" Signature="11"/>
        </PatchList>
    </Metadata>
</Patch>
'''

    def lines(self, xml):
        root = ET.fromstring(xml)
        meta = root.find('Metadata')
        return meta.get('Name'), meta.get('isEnabled'), [l.get('Address') for l in root.iter('Line')]

    def test_the_cup_lines_go_to_their_own_patch_and_the_shared_code_stays_in_the_base(self):
        base, cups = mbm.split_patch(self.XML)
        self.assertEqual(self.lines(base), ('Race and battle options', 'true', ['0x2738000', '0x168d7a5', '0x167c950']))
        self.assertEqual(self.lines(cups), ('Extra cups', 'true', ['0x13de1ac', '0x167e750', '0x18d14c1']))

    def test_a_line_without_its_comment_stops_the_split(self):
        with self.assertRaisesRegex(ValueError, 'every line needs its comment'):
            mbm.split_patch(self.XML.replace('<!-- Lap menu: lap counts with 3 digits -->', ''))


class LibraryTests(unittest.TestCase):
    def setUp(self):
        self.port = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.port, ignore_errors=True)
        self.builtin = self.port / 'builtin'
        self.mods = self.port / 'mods'
        self.mods.mkdir()
        self.patches = self.port / 'patches'
        old = os.environ.get('BB_BUILTIN_MODS')
        os.environ['BB_BUILTIN_MODS'] = str(self.builtin)
        self.addCleanup(lambda: os.environ.__setitem__('BB_BUILTIN_MODS', old if old is not None else ''))

    def catalog(self):
        root = ET.parse(self.patches / mod_library.FILENAME).getroot()
        return {m.get('Name'): m for m in root.iter('Metadata')}

    def test_a_built_in_mod_is_listed_on_by_default(self):
        package(self.builtin / 'race-options')
        found = mod_library.scan(self.mods, self.patches)
        self.assertEqual(found['messages'], [])
        item = found['entries']['race']
        self.assertEqual((item['source'], item['builtin']), ('builtin:race-options', True))
        meta = self.catalog()['Mod: race']
        self.assertEqual((meta.get('isEnabled'), meta.get('ModBuiltin')), ('true', '1'))
        fmt.validate_folder(mod_library.folder(self.mods, 'race'))

    def test_the_built_in_mod_wins_over_a_copy_in_the_mods_folder(self):
        package(self.builtin / 'race-options')
        package(self.mods / 'race-5.1.0', version='9.0.0', delta=False)  # even a newer one
        found = mod_library.scan(self.mods, self.patches)
        item = found['entries']['race']
        self.assertTrue(item['builtin'])
        self.assertIn('not loaded from: race-5.1.0 (9.0.0)', item['version_notice'])
        self.assertEqual(self.catalog()['Mod: race'].get('ModBuiltin'), '1')

    def test_a_mod_folder_s_own_mods_are_off_by_default(self):
        package(self.mods / 'other', ident='other')
        mod_library.scan(self.mods, self.patches)
        meta = self.catalog()['Mod: other']
        self.assertEqual((meta.get('isEnabled'), meta.get('ModBuiltin')), ('false', None))

    def test_a_built_in_mod_cannot_be_removed(self):
        package(self.builtin / 'race-options')
        item = mod_library.scan(self.mods, self.patches)['entries']['race']
        with self.assertRaisesRegex(ValueError, 'cannot be removed'):
            mod_library.remove(self.mods, self.patches, 'race', item['sha256'])

    def test_without_the_folder_there_are_no_built_in_mods(self):
        os.environ['BB_BUILTIN_MODS'] = ''
        self.assertEqual(mod_library.builtin_sources(), [])
        os.environ['BB_BUILTIN_MODS'] = str(self.port / 'missing')
        self.assertEqual(mod_library.builtin_sources(), [])


class ShippedTests(unittest.TestCase):
    """The folders in scripts/builtin_mods are valid packages, with no game file in them."""

    def test_the_shipped_built_in_mods_are_valid(self):
        folders = [p for p in mod_library.BUILTIN.iterdir() if p.is_dir()] if mod_library.BUILTIN.is_dir() else []
        if not folders:
            self.skipTest('no built-in mods in this tree')
        for folder in folders:
            with self.subTest(folder.name):
                m = fmt.validate_folder(folder)
                if folder.name == 'extra-cups-art':
                    continue  # not kept in the repository
                for op in [*m.get('operations', []), *(o for s in m.get('operationSets', {}).values() for o in s)]:
                    self.assertTrue(op['payload'].endswith('.delta.json'), op['payload'])

    def test_extra_cups_switch_the_cup_files_and_patch(self):
        folder = mod_library.BUILTIN / 'race-options'
        if not folder.is_dir():
            self.skipTest('no race-options in this tree')
        m = fmt.validate_folder(folder)
        settings = {'widescreen_layout': 'off'}
        self.assertIs(m['configuration']['extra-cups']['default'], False)
        on = fmt.resolve(m, settings, {'extra-cups': True})
        off = fmt.resolve(m, settings, {'extra-cups': False})
        self.assertEqual((on[0], off[0]), ('extra-cups', 'game-cups'))
        self.assertEqual(on[1]['patches'], ['patches/race-options.xml', 'patches/extra-cups.xml'])
        self.assertEqual(off[1]['patches'], ['patches/race-options.xml'])
        system = [o['payload'] for o in on[1]['operations'] if o['asset'] == mbm.SYSTEM]
        self.assertEqual(system, ['payloads/systemdata-battle-cups.delta.json'])
        system = [o['payload'] for o in off[1]['operations'] if o['asset'] == mbm.SYSTEM]
        self.assertEqual(system, ['payloads/systemdata-battle.delta.json'])
        self.assertFalse(any(o['asset'] in mbm.CUP_ASSETS for o in off[1]['operations']))


if __name__ == '__main__':
    unittest.main()


class CupArtTests(unittest.TestCase):
    """cup_art.py: the extra cups' pictures, drawn from the game (Prepare)."""

    def test_tiling_round_trips_and_pads_to_eight_blocks(self):
        import numpy as np
        import cup_art
        self.assertEqual(cup_art.tiled_shape(1288, 1080), (328, 272))
        w, h = 40, 24
        pw, ph = cup_art.tiled_shape(w, h)
        grid = np.arange(pw * ph * 16, dtype=np.uint32).astype(np.uint8).reshape(ph, pw, 16)
        self.assertTrue((cup_art.untile_blocks(cup_art.tile_blocks(grid, w, h), w, h) == grid).all())

    def test_bc3_keeps_a_picture_close(self):
        import numpy as np
        import cup_art
        y, x = np.mgrid[0:32, 0:48]
        rgb = np.stack([x * 5, y * 7, (x + y) * 3], -1).astype(np.uint8)
        back = cup_art.decode_bc3(cup_art.encode_bc3(rgb))
        self.assertLess(np.abs(back.astype(int) - rgb).mean(), 4)
        flat = np.full((8, 8, 3), (231, 247, 255), np.uint8)
        self.assertLessEqual(np.abs(cup_art.decode_bc3(cup_art.encode_bc3(flat)).astype(int) - flat).max(), 4)

    def test_every_cup_has_four_track_pictures(self):
        import cup_art
        self.assertEqual(len(cup_art.CUPS), 25)
        self.assertTrue(all(len(names) == 4 for _, names in cup_art.CUPS))
        self.assertEqual(len({ident for ident, _ in cup_art.CUPS}), 25)

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'BB_TEST_GAME: a USA 1.21 game folder')
    def test_the_pictures_drawn_from_the_game_are_the_author_s(self):
        import cup_art
        op, data = cup_art.draw(os.environ['BB_TEST_GAME'], log=lambda _: None)
        # Ultimate Customizer 5.1.0's cupselect-imagenes.igz, byte for byte.
        self.assertEqual(op['patched_sha256'], '77646aee024c3c66500879c17e233ff8c1e9f785184c92af2fd58a0b3a89bc2e')
        with tempfile.TemporaryDirectory() as folder:
            m = fmt.validate_folder(cup_art.package(Path(folder) / 'extra-cups-art', op, data))
            self.assertEqual(m['id'], 'extra-cups-art')


class ExtraCupsOptionTests(unittest.TestCase):
    """The extra cups option is off by default and turns on only with the pictures prepared."""

    def setUp(self):
        self.port = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.port, ignore_errors=True)
        self.mods = self.port / 'mods'
        self.mods.mkdir()
        self.patches = self.port / 'patches'
        old = os.environ.get('BB_BUILTIN_MODS')
        os.environ['BB_BUILTIN_MODS'] = str(mod_library.BUILTIN)
        self.addCleanup(lambda: os.environ.__setitem__('BB_BUILTIN_MODS', old if old is not None else ''))
        if not (mod_library.BUILTIN / 'race-options').is_dir():
            self.skipTest('no race-options in this tree')

    def test_off_by_default(self):
        item = mod_library.scan(self.mods, self.patches)['entries'][mod_library.EXTRA_CUPS[0]]
        self.assertEqual(item['option_values'], {'extra-cups': False})

    def test_on_only_with_the_pictures(self):
        ident = mod_library.EXTRA_CUPS[0]
        item = mod_library.scan(self.mods, self.patches)['entries'][ident]
        with self.assertRaisesRegex(ValueError, 'Prepare the extra cups art first'):
            mod_library.configure(self.mods, self.patches, ident, item['sha256'], {'extra-cups': 'true'})
        package(mod_library.generated_builtin(self.mods) / 'extra-cups-art', ident='extra-cups-art')
        found = mod_library.scan(self.mods, self.patches)
        self.assertTrue(found['entries']['extra-cups-art']['builtin'])
        mod_library.configure(self.mods, self.patches, ident, item['sha256'], {'extra-cups': 'true'})
        self.assertEqual(mod_library.scan(self.mods, self.patches)['entries'][ident]['option_values'],
                         {'extra-cups': True})
