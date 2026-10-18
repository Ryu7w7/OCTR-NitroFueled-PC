"""ZIP package regression tests using synthetic bytes only; no game data."""
from paths import ROOT
from pathlib import Path
from contextlib import nullcontext
import copy,json,os,shutil,tempfile,unittest,zipfile
import xml.etree.ElementTree as ET
from unittest.mock import patch
import mod_packages as adapter
import nitro_library as library
import nitro_extensions as composer
import nitro_format as fmt
from nitro_components import sha,sha_bytes,save_json,apply_file
from package_mod_v2 import build
from nitro_variant_settings import choose

class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.port=self.root/'port';self.port.mkdir()
        self.mods=self.port/'mods';self.mods.mkdir();(self.port/'patches').mkdir()
        (self.port/'scripts').mkdir();(self.port/'bin').mkdir()
        (self.port/'bin/sacredfire.exe').write_bytes(b'synthetic engine')
        (self.port/'bin/VERSION').write_text('test')
        (self.port/'run.py').write_text('# synthetic runner')
        (self.port/'scripts/patches.py').write_text('# synthetic compiler')
        self.game=self.root/'game';self.game.mkdir();(self.game/'eboot.bin').write_bytes(b'synthetic executable')
        (self.game/'data.txt').write_bytes(b'original')
        self.ini=self.port/'bbport.ini';self.ini.write_text('output_res=1920x1080\nrender_res=1080\nonline=0\n')
        self.prefs=self.port/'patches.json';save_json(self.prefs,{'enabled':[],'disabled':[]})
        adapter.ACTIVE=None;self.count=0
        self.addCleanup(setattr,adapter,'ACTIVE',None)
    def manifest(self,ident='sample'):
        return {'schemaVersion':2,'id':ident,'name':ident,'version':'1.0.0','author':'tcosi',
            'description':'Synthetic fixture','requiresCapabilities':[],'files':[],
            'compatibility':{'runtime_versions':['test'],'runtime_sha256':[sha(self.port/'bin/sacredfire.exe')],
             'eboot_sha256':[sha(self.game/'eboot.bin')],'online':'offline-only'}}
    def package(self,ident='sample',manifest=None,files=None,filename=None):
        self.count+=1;source=self.root/('source'+str(self.count));source.mkdir()
        save_json(source/'nitro-mod.json',manifest or self.manifest(ident))
        for name,data in (files or {}).items():
            p=source/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
        dest=self.mods/(filename or ident+'.zip');build(source,dest);return dest
    def scan(self):return library.scan(self.mods,self.port/'patches')
    def enable(self,*names):save_json(self.prefs,{'enabled':[library.PREFIX+n for n in names],'disabled':[]})
    def prepare(self):
        with patch.object(adapter,'preflight',return_value=0),patch.object(adapter,'launch_lock',return_value=nullcontext()),patch('atexit.register'):
            return adapter.prepare(self.port,self.port,{},self.game,self.ini,prepare_only=True)
    def test_new_zip_listed_but_disabled(self):
        self.package();self.assertIn('sample',self.scan()['entries'])
        self.assertEqual(self.prepare(),(self.game,str(self.ini)))
    def test_two_packages_compose_and_defaults_precede_variant(self):
        uw=self.manifest('wide')|{'requiresCapabilities':['variant-settings','variants','settings'],
            'variantSettings':{'output_res':{'default':'5120x1440','allowed':['5120x1440','3840x1080']}},
            'variants':[{'id':'wide','when':{'output_res':['5120x1440','3840x1080']},'settings':{'temporal':'off'}}]}
        gauge=self.manifest('gauge')|{'requiresCapabilities':['game-files'],'operations':[
            {'kind':'game_file','target':'data.txt','original_sha256':sha_bytes(b'original'),'patched_sha256':sha_bytes(b'modified'),'payload':'payload.txt'}]}
        self.package('wide',uw);self.package('gauge',gauge,{'payload.txt':b'modified'});self.enable('wide','gauge')
        before=self.ini.read_bytes();game,config=self.prepare()
        self.assertEqual((game/'data.txt').read_bytes(),b'modified')
        self.assertIn('output_res=5120x1440',Path(config).read_text())
        self.assertEqual(self.ini.read_bytes(),before);self.assertEqual((self.game/'data.txt').read_bytes(),b'original')
        self.assertEqual((game/'data.txt').stat().st_nlink,1)
    def test_removing_zip_omits_cached_mod(self):
        package=self.package();self.enable('sample');self.prepare();package.unlink()
        self.assertEqual(self.prepare(),(self.game,str(self.ini)))
        self.assertEqual(self.scan()['entries'],{})
    def test_update_and_rename_keep_stable_checkbox_key(self):
        p=self.package();self.enable('sample');self.scan();p.rename(self.mods/'renamed.zip')
        self.assertEqual(set(self.scan()['entries']),{'sample'})
        p=self.mods/'renamed.zip';p.unlink();self.package(manifest=self.manifest()|{'version':'1.1.0'})
        self.assertEqual(self.scan()['entries']['sample']['version'],'1.1.0');self.prepare()
    def test_duplicate_ids_rejected(self):
        self.package();self.package(filename='duplicate.zip')
        result=self.scan();self.assertEqual(result['entries'],{});self.assertTrue(result['messages'])
    def test_catalog_provides_native_mod_name_and_zip_identity(self):
        package=self.package(manifest=self.manifest()|{'name':'Readable mod name'})
        self.scan()
        meta=ET.parse(self.port/'patches'/library.FILENAME).getroot().find('Metadata')
        self.assertEqual(meta.get('ModName'),'Readable mod name');self.assertEqual(meta.get('ModHash'),sha(package))
        self.assertEqual(meta.get('Name'),'ZIP mod: sample')
    def test_remove_keeps_zip_saves_and_unrelated_selection(self):
        package=self.package();digest=sha(package);self.enable('sample')
        prefs=json.loads(self.prefs.read_text());prefs['enabled'].append('other.xml/Other');prefs['custom']='keep';save_json(self.prefs,prefs)
        save=self.port/'mod-packages/user/savedata/progress';save.parent.mkdir(parents=True);save.write_bytes(b'keep save')
        removed=library.remove(self.mods,self.port/'patches','sample',digest)
        self.assertFalse(package.exists());self.assertEqual(sha(removed),digest)
        self.assertTrue(removed.is_relative_to(self.port/'mod-packages/removed'))
        prefs=json.loads(self.prefs.read_text())
        self.assertEqual(prefs['enabled'],['other.xml/Other']);self.assertEqual(prefs['custom'],'keep')
        self.assertIn(library.PREFIX+'sample',prefs['disabled']);self.assertEqual(save.read_bytes(),b'keep save')
        self.assertEqual(library.entries(self.mods),{})
    def test_removed_zip_restores_disabled_when_copied_back(self):
        package=self.package();self.enable('sample')
        removed=library.remove(self.mods,self.port/'patches','sample',sha(package))
        shutil.copy2(removed,package);self.scan()
        self.assertIn('sample',library.entries(self.mods));self.assertEqual(self.prepare(),(self.game,str(self.ini)))
    def test_remove_refuses_updated_zip_until_reload(self):
        package=self.package();digest=sha(package);package.unlink()
        self.package(manifest=self.manifest()|{'version':'1.1.0'})
        with self.assertRaisesRegex(ValueError,'changed'):library.remove(self.mods,self.port/'patches','sample',digest)
        self.assertTrue(package.exists())
    def test_remove_cannot_escape_mods_folder(self):
        package=self.package();digest=sha(package)
        with self.assertRaises(ValueError):library.remove(self.mods,self.port/'patches','../sample',digest)
        self.assertTrue(package.exists())
    def test_remove_refuses_invalid_selection_without_moving_zip(self):
        package=self.package();self.prefs.write_text('{"enabled": "wrong"}')
        with self.assertRaisesRegex(ValueError,'Invalid patch selection'):library.remove(self.mods,self.port/'patches','sample',sha(package))
        self.assertTrue(package.exists())
    def test_remove_blocked_by_active_mod_session(self):
        from mod_session import launch_lock
        package=self.package();digest=sha(package)
        with launch_lock(self.port/'mod-packages'):
            with self.assertRaisesRegex(ValueError,'already running'):library.remove(self.mods,self.port/'patches','sample',digest)
        self.assertTrue(package.exists())
    def test_failed_remove_restores_previous_selection(self):
        package=self.package();self.enable('sample');before=self.prefs.read_bytes()
        original=Path.rename
        def rename(path,target):
            if path==package:raise PermissionError('synthetic locked file')
            return original(path,target)
        with patch.object(Path,'rename',rename),self.assertRaises(PermissionError):
            library.remove(self.mods,self.port/'patches','sample',sha(package))
        self.assertEqual(self.prefs.read_bytes(),before);self.assertTrue(package.exists())
    def test_partial_zip_is_reported(self):
        (self.mods/'copying.zip').write_bytes(b'PK incomplete')
        self.assertTrue(self.scan()['messages'])
    def test_cached_payload_tampering_rejected(self):
        self.package(files={'data.txt':b'checked'});self.scan()
        (library.folder(self.mods,'sample')/'data.txt').write_bytes(b'changed')
        self.enable('sample')
        with self.assertRaisesRegex(ValueError,'could not be validated'):self.prepare()
    def test_traversal_zip_rejected(self):
        p=self.package()
        with zipfile.ZipFile(p,'a') as z:z.writestr('../escape.txt',b'bad')
        self.assertFalse(self.scan()['entries']);self.assertFalse((self.port/'escape.txt').exists())
    def test_offline_package_cannot_start_online(self):
        self.package();self.enable('sample');self.ini.write_text(self.ini.read_text().replace('online=0','online=1'))
        with self.assertRaisesRegex(ValueError,'offline-only'):self.prepare()
    def test_wrong_runtime_rejected(self):
        self.package();self.enable('sample');(self.port/'bin/sacredfire.exe').write_bytes(b'other engine')
        with self.assertRaisesRegex(ValueError,'unsupported runtime'):self.prepare()
    def test_dependency_and_conflict_checks(self):
        self.package('child',self.manifest('child')|{'requiresCapabilities':['dependencies'],'requires':[{'id':'base','version':'>=1.0.0'}]})
        self.enable('child')
        with self.assertRaisesRegex(ValueError,'dependency'):self.prepare()
    def test_overlapping_file_mods_rejected(self):
        for ident in ('one','two'):
            m=self.manifest(ident)|{'requiresCapabilities':['game-files'],'operations':[
                {'kind':'game_file','target':'new.txt','original_sha256':None,'patched_sha256':sha_bytes(b'x'),'payload':'payload.txt'}]}
            self.package(ident,m,{'payload.txt':b'x'})
        self.enable('one','two')
        with self.assertRaisesRegex(ValueError,'Mod conflicts'):self.prepare()
    def test_save_progress_only_commits_to_private_profile_after_success(self):
        self.package();self.enable('sample');self.prepare();session,state=adapter.ACTIVE
        save=session/'user/savedata/progress';save.parent.mkdir(parents=True);save.write_bytes(b'progress')
        adapter.finish(1);self.assertFalse((state/'user/savedata/progress').exists())
        adapter.finish(0);self.assertEqual((state/'user/savedata/progress').read_bytes(),b'progress')
        self.assertFalse((self.port/'user').exists())
    def test_compatible_resolution_preserved(self):
        m={'variantSettings':{'output_res':{'default':'5120x1440','allowed':['5120x1440','3840x1080']}}}
        settings,changes=choose([m],{'output_res':'3840x1080'})
        self.assertEqual(settings['output_res'],'3840x1080');self.assertEqual(changes,{})
    def test_incompatible_resolution_domains_fail(self):
        a={'variantSettings':{'output_res':{'default':'3440x1440','allowed':['3440x1440']}}}
        b={'variantSettings':{'output_res':{'default':'5120x1440','allowed':['5120x1440']}}}
        with self.assertRaises(ValueError):choose([a,b],{'output_res':'1920x1080'})
    def test_no_zip_preserves_custom_paths(self):
        env={'BB_CONFIG':str(self.ini)}
        self.assertEqual(adapter.prepare(self.port,self.port,env,self.game,self.ini),(self.game,str(self.ini)))
        self.assertEqual(env,{'BB_CONFIG':str(self.ini)})
    def test_global_mod_switch_keeps_zip_on_original_path(self):
        self.package();self.enable('sample');env={'BB_MODS_ENABLED':'0'}
        self.assertEqual(adapter.prepare(self.port,self.port,env,self.game,self.ini),(self.game,str(self.ini)))
    def test_cache_reset_is_private_and_not_repeated_after_shader_preparation(self):
        self.package();self.enable('sample')
        original=self.port/'user/cache/CUSA13795/test.spv';original.parent.mkdir(parents=True);original.write_bytes(b'vanilla')
        env={'BB_CLEAR_SHADER_CACHE':'1'}
        with patch.object(adapter,'preflight',return_value=0),patch.object(adapter,'launch_lock',return_value=nullcontext()),patch('atexit.register'):
            adapter.prepare(self.port,self.port,env,self.game,self.ini,prepare_only=True)
        session,state=adapter.ACTIVE
        self.assertEqual(env['BB_CLEAR_SHADER_CACHE'],'0')
        self.assertEqual(original.read_bytes(),b'vanilla')
        self.assertEqual((session/'cache-before-reset/CUSA13795/test.spv').read_bytes(),b'vanilla')
    def test_hardlinked_file_write_refused(self):
        source=self.root/'source';source.write_bytes(b'original');linked=self.root/'link';os.link(source,linked)
        payload=self.root/'replacement';payload.write_bytes(b'changed')
        op={'original_sha256':sha_bytes(b'original'),'patched_sha256':sha_bytes(b'changed'),'payload':'replacement'}
        with self.assertRaisesRegex(ValueError,'hardlink'):apply_file(linked,op,self.root)
        self.assertEqual(source.read_bytes(),b'original')

    def test_preflight_rejects_overlapping_real_xml_writes(self):
        from test_prepare import fixture
        profile=self.root/'profile';(profile/'game').mkdir(parents=True);(profile/'patches').mkdir()
        (profile/'game/eboot.bin').write_bytes(fixture())
        xml=ET.Element('Patch');ids=ET.SubElement(xml,'TitleID');ET.SubElement(ids,'ID').text='CUSA13795'
        for name in ('one','two'):
            meta=ET.SubElement(xml,'Metadata',Name=name,AppVer='01.21',AppElf='eboot.bin',isEnabled='true')
            lines=ET.SubElement(meta,'PatchList');ET.SubElement(lines,'Line',Type='bytes',Address='0x400002',Value='9090')
        ET.ElementTree(xml).write(profile/'patches/test.xml')
        with self.assertRaisesRegex(ValueError,'Overlapping executable patches'):adapter.preflight(profile,self.ini,self.prefs)

    def test_preflight_rejects_signature_skip(self):
        from test_prepare import fixture
        profile=self.root/'profile';(profile/'game').mkdir(parents=True);(profile/'patches').mkdir()
        (profile/'game/eboot.bin').write_bytes(fixture())
        (profile/'patches/test.xml').write_text('<Patch><TitleID><ID>CUSA13795</ID></TitleID><Metadata Name="missing" AppVer="01.21" AppElf="eboot.bin" isEnabled="true"><PatchList><Line Type="bytes" Address="0x400002" Value="90" Signature="fe ed fa ce" /></PatchList></Metadata></Patch>')
        with self.assertRaisesRegex(ValueError,'Incomplete executable patch'):adapter.preflight(profile,self.ini,self.prefs)

if __name__=='__main__':unittest.main()
