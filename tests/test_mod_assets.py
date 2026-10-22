"""Native asset mod composition using small synthetic IGA archives only."""
from paths import ROOT
from pathlib import Path
import json,struct,unittest,zipfile
from unittest.mock import patch
import test_mod_packages as fixtures
import mod_library as library
from mod_components import sha_bytes

def pak(payload=b'synthetic texture'):
    names=struct.pack('<I',4)+b'kind\0ui/example.igz\0'
    b=bytearray(2048);struct.pack_into('<5I',b,0,0x1a414749,13,0,1,2048)
    struct.pack_into('<QI',b,40,76,len(names));struct.pack_into('<I',b,56,0)
    struct.pack_into('<4I',b,60,2048,0,len(payload),0xffffffff)
    b[76:76+len(names)]=names
    return bytes(b)+payload

class AssetTests(unittest.TestCase):
    setUp=fixtures.PackageTests.setUp
    manifest=fixtures.PackageTests.manifest
    package=fixtures.PackageTests.package
    scan=fixtures.PackageTests.scan
    enable=fixtures.PackageTests.enable
    prepare=fixtures.PackageTests.prepare
    read_game=fixtures.PackageTests.read_game
    def raw(self,name='update3.pak',payload=b'modded'):
        p=self.mods/name;p.write_bytes(pak(payload));return p
    def test_pak_is_listed_disabled_by_default(self):
        self.raw();catalog=self.scan();self.assertEqual(len(catalog['entries']),1)
        self.assertEqual(self.prepare(),(self.game,str(self.ini)))
    def test_loose_pak_goes_to_archives_unchanged(self):
        p=self.raw();ident=next(iter(self.scan()['entries']));self.enable(ident)
        view,_=self.prepare();self.assertEqual(view,self.game)
        self.assertEqual(self.read_game('archives/update3.pak'),p.read_bytes())
        self.assertFalse((self.game/'archives').exists())
        self.assertFalse((self.game/'archives/update3.pak').exists())
    def test_single_pak_zip_requires_no_manifest(self):
        with zipfile.ZipFile(self.mods/'Driver.zip','w') as z:z.writestr('update3.pak',pak())
        ident=next(iter(self.scan()['entries']));self.enable(ident)
        view,_=self.prepare();self.assertEqual(self.read_game('archives/update3.pak'),pak())
    def test_wrapped_native_zip(self):
        with zipfile.ZipFile(self.mods/'Driver.zip','w') as z:
            z.writestr('Driver/archives/update3.pak',pak());z.writestr('Driver/README.txt','readme')
        self.assertEqual(len(self.scan()['entries']),1)
    def test_pak_folder_and_logic_zip_compose(self):
        p=self.mods/'Driver/archives';p.mkdir(parents=True);(p/'update3.pak').write_bytes(pak())
        m=self.manifest('hud')|{'requiresCapabilities':['game-files'],'operations':[
            {'kind':'game_file','target':'data.txt','payload':'payload.bin','original_sha256':sha_bytes(b'original'),'patched_sha256':sha_bytes(b'HUD')}]}
        self.package('hud',m,{'payload.bin':b'HUD'});self.enable(*self.scan()['entries'])
        view,_=self.prepare();self.assertEqual(self.read_game('data.txt'),b'HUD')
        self.assertEqual(self.read_game('archives/update3.pak'),pak())
    def test_distinct_paks_compose(self):
        self.raw('update3.pak');self.raw('juicedomain_race.pak');self.enable(*self.scan()['entries'])
        view,_=self.prepare();self.assertEqual(view,self.game)
        self.assertEqual(self.read_game('archives/update3.pak'),pak(b'modded'))
        self.assertEqual(self.read_game('archives/juicedomain_race.pak'),pak(b'modded'))
    def test_same_archive_blocks_rather_than_silently_overwriting(self):
        self.raw()
        with zipfile.ZipFile(self.mods/'Driver.zip','w') as z:z.writestr('update3.pak',pak(b'other'))
        self.enable(*self.scan()['entries'])
        with self.assertRaisesRegex(ValueError,'Mod conflicts'):self.prepare()
    def test_full_pak_conflicts_with_asset_edit(self):
        self.raw()
        m=self.manifest('edit')|{'requiresCapabilities':['archive-assets'],'operations':[
            {'kind':'archive_asset','archive':'update3.pak','asset':'ui/example.igz','index':0,
             'payload':'payload.bin','original_sha256':sha_bytes(b'modded'),'patched_sha256':sha_bytes(b'edit')}]}
        self.package('edit',m,{'payload.bin':b'edit'});self.enable(*self.scan()['entries'])
        with self.assertRaisesRegex(ValueError,'Mod conflicts'):self.prepare()
    def test_bad_pak_not_listed(self):
        (self.mods/'broken.pak').write_bytes(b'not pak');c=self.scan()
        self.assertFalse(c['entries']);self.assertTrue(c['messages'])
    def test_legacy_zip_rejects_executable_and_xml_without_manifest(self):
        for name in ('run.py','patch.xml'):
            with zipfile.ZipFile(self.mods/'bad.zip','w') as z:z.writestr(name,'bad');z.writestr('update3.pak',pak())
            self.assertFalse(self.scan()['entries'])
    def test_zip_path_traversal_rejected(self):
        with zipfile.ZipFile(self.mods/'bad.zip','w') as z:z.writestr('../update3.pak',pak())
        self.assertFalse(self.scan()['entries'])
    def test_cache_tampering_detected(self):
        self.raw();ident=next(iter(self.scan()['entries']))
        (library.folder(self.mods,ident)/'archives/update3.pak').write_bytes(pak(b'tampered'))
        self.assertFalse(self.scan()['entries'])
    def test_pak_update_preserves_id(self):
        p=self.raw();first=self.scan();ident=next(iter(first['entries']));self.enable(ident)
        p.write_bytes(pak(b'new version'));second=self.scan()
        self.assertIn(ident,second['entries']);self.assertNotEqual(first['entries'][ident]['sha256'],second['entries'][ident]['sha256'])
    def test_remove_pak_preserves_original_and_moves_source(self):
        p=self.raw();ident,item=next(iter(self.scan()['entries'].items()));self.enable(ident)
        moved=library.remove(self.mods,self.port/'patches',ident,item['sha256'])
        self.assertFalse(p.exists());self.assertEqual(moved.read_bytes(),pak(b'modded'))
        self.assertEqual((self.game/'eboot.bin').read_bytes(),b'synthetic executable')

if __name__=='__main__':unittest.main()
