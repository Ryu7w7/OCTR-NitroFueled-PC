"""Synthetic regional package regression tests; no game data."""
from paths import ROOT
from pathlib import Path
import tempfile, unittest, xml.etree.ElementTree as ET
import mod_library
from patches import external_patches

class RegionalCatalogTests(unittest.TestCase):
    def test_empty_display_catalog_is_region_neutral(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); mods=root/'mods'; mods.mkdir(); patches=root/'patches'; patches.mkdir()
            mod_library.scan(mods, patches)
            catalog=ET.parse(patches/mod_library.FILENAME).getroot()
            self.assertIsNone(catalog.find('TitleID'))

    def test_patches_filter_by_actual_edition(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            for title in ('CUSA13795','CUSA14876'):
                (root/(title+'.xml')).write_text('<Patch><TitleID><ID>'+title+'</ID></TitleID><Metadata Name="test" AppVer="01.21" AppElf="eboot.bin"><PatchList /></Metadata></Patch>')
            for title in ('CUSA13795','CUSA14876'):
                found=external_patches(root,'01.21',title)
                self.assertEqual([p[1].name for p in found],[title+'.xml'])


class RegionalPackageTests(unittest.TestCase):
    # Reuse fixture helpers, not the existing test methods (avoids duplicate discovery).
    import test_mod_packages as fixtures
    setUp=fixtures.PackageTests.setUp
    manifest=fixtures.PackageTests.manifest
    package=fixtures.PackageTests.package
    enable=fixtures.PackageTests.enable

    def configure(self, game_title, patch_title, version='01.21'):
        import json
        from test_game_check import param_sfo
        from test_patches import tiny_elf
        from mod_components import sha_bytes
        (self.game/'sce_sys/param.sfo').write_bytes(param_sfo({'TITLE_ID':game_title,'APP_VER':version}))
        import struct
        elf=bytearray(tiny_elf(bytes(128)))
        struct.pack_into('<16sHHIQQQIHHHHHH',elf,0,b'\x7fELF\x02\x01\x01'+bytes(9),3,62,1,0x1000,64,0,0,64,56,1,64,0,0)
        (self.game/'eboot.bin').write_bytes(elf)
        lines=''.join('<Line Type="bytes" Address="'+hex(0x401000+i*8)+'" Value="90" ExpectedSHA256="'+sha_bytes(b'\0')+'" />' for i in range(7))
        xml=('<Patch><TitleID><ID>'+patch_title+'</ID></TitleID>'
             '<Metadata Name="probe" AppVer="01.21" AppElf="eboot.bin" isEnabled="false"><PatchList>'+lines+'</PatchList></Metadata></Patch>')
        manifest=self.manifest()|{'requiresCapabilities':['xml-patches','code-signatures'],'patches':['logic.xml']}
        self.package(manifest=manifest,files={'logic.xml':xml.encode()})
        self.enable('sample')

    def actual_prepare(self):
        from contextlib import nullcontext
        from unittest.mock import patch
        import mod_packages as adapter
        self.environment={}
        with patch.object(adapter,'launch_lock',return_value=nullcontext()),patch('atexit.register'):
            return adapter.prepare(self.port,self.port,self.environment,self.game,self.ini,prepare_only=True)

    def check_success(self, title):
        import json
        self.configure(title,title)
        game,_=self.actual_prepare()
        self.assertEqual(game,self.game)
        xml=ET.parse(Path(self.environment['BB_PATCHES_DIR'])/'OCTR-Mod-Content.xml')
        self.assertEqual([n.text for n in xml.findall('./TitleID/ID')],[title])
        self.assertEqual(len(list(xml.iter('Line'))),7)
        import mod_packages as adapter
        launch=json.loads((adapter.ACTIVE[0]/'launch.json').read_text())
        self.assertEqual(launch['writes'],7)
        # The cached result is target-bound, not an old USA-labelled profile.
        ready=json.loads((Path(self.environment['BB_PATCHES_DIR']).parent/'ready.json').read_text())
        self.assertEqual(ready['identity']['target'],[title,'01.21'])

    def test_eu_all_seven_writes_survive_actual_adapter(self):
        self.check_success('CUSA14876')

    def test_usa_all_seven_writes_survive_actual_adapter(self):
        self.check_success('CUSA13795')

    def test_usa_patch_on_eu_is_rejected_without_partial_profile(self):
        self.configure('CUSA14876','CUSA13795')
        with self.assertRaisesRegex(ValueError,'different game edition/version'):
            self.actual_prepare()
        self.assertEqual(list((self.port/'mod-state').glob('extensions/overlays/*/ready.json')),[])

    def test_eu_patch_on_usa_is_rejected_without_partial_profile(self):
        self.configure('CUSA13795','CUSA14876')
        with self.assertRaisesRegex(ValueError,'different game edition/version'):
            self.actual_prepare()
        self.assertEqual(list((self.port/'mod-state').glob('extensions/overlays/*/ready.json')),[])

    def test_wrong_game_version_is_rejected(self):
        self.configure('CUSA14876','CUSA14876',version='01.00')
        with self.assertRaisesRegex(ValueError,'01.21'):
            self.actual_prepare()

    def test_missing_game_sfo_is_not_assumed_usa(self):
        self.configure('CUSA13795','CUSA13795')
        (self.game/'sce_sys/param.sfo').unlink()
        with self.assertRaisesRegex(ValueError,'unknown'):
            self.actual_prepare()

    def test_unsupported_game_without_enabled_package_starts(self):
        # Packages ship an empty mods folder: EU 1.00 must start as before.
        self.configure('CUSA14876','CUSA14876',version='01.00')
        self.enable()
        game,config=self.actual_prepare()
        self.assertEqual((game,config),(self.game,str(self.ini)))
        self.assertEqual(self.environment.get('BB_MODS_ENABLED'),'0')

    def test_unsupported_game_with_empty_mods_folder_starts(self):
        (self.game/'sce_sys/param.sfo').unlink()
        self.environment={}
        game,_=self.actual_prepare()
        self.assertEqual(game,self.game)
