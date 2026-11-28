"""Skin portrait registry patch on a USA 1.21 game dump."""
from paths import ROOT  # noqa: F401
import os
import unittest
from pathlib import Path

import skin_portraits
from archive_reader import Archive
from racer_registry import File

GAME = os.environ.get('BB_TEST_GAME', '')


@unittest.skipUnless(GAME and Path(GAME, 'archives', 'update2.pak').is_file(), 'BB_TEST_GAME not set')
class SkinPortraitTests(unittest.TestCase):
    def test_every_portrait_material_is_connected_to_one_skin(self):
        archive = Archive(Path(GAME, 'archives', 'update2.pak'))
        try:
            entry = next(e for e in archive.files if e['path'].lower() == skin_portraits.ENTRY)
            material_entry = next(e for e in archive.files if e['path'].replace('\\', '/').lower()
                                  == skin_portraits.MATERIAL_ENTRY)
            original = bytes(archive.extract(entry))
            original_materials = bytes(archive.extract(material_entry))
        finally:
            archive.data.close()
            archive._file.close()
        sources = skin_portraits.portrait_files(GAME)
        portraits = {suffix: values[:3] for suffix, values in sources.items()}
        before = File(original)
        base_handles = {skin: before.u32(skin + 0x70) for skin in before.typed
                        if before.typed[skin] == 'CDriverSkinData'}
        patched, count = skin_portraits.apply(original, portraits)
        self.assertEqual(count, len(portraits))
        system = File(patched)
        handles = [system.handle(system.u32(skin + 0x78)) for skin in system.typed
                   if system.typed[skin] == 'CDriverSkinData' and skin + 0x78 in system.sites['RHND']]
        self.assertEqual(len(handles), len(portraits))
        self.assertIn(('RacerHud_Pura_Musketeer', skin_portraits.MATERIAL_NAMESPACE), handles)
        self.assertTrue(all(system.u32(skin + 0x70) == value for skin, value in base_handles.items()))
        merged, material_count = skin_portraits.merge_materials(original_materials, sources)
        self.assertEqual(material_count, len(portraits) * 2)
        material = File(merged)
        roots = dict(material.roots()[2])
        self.assertIn('RacerHud_Pura_Musketeer', roots)
        self.assertIn('Minimap_Pura_Musketeer', roots)
