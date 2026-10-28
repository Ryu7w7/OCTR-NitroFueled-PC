"""ZIP package regression tests using synthetic bytes only; no game data."""
from paths import ROOT
from pathlib import Path
from contextlib import nullcontext
import copy,json,os,shutil,tempfile,unittest,zipfile
import xml.etree.ElementTree as ET
from unittest.mock import patch
import mod_packages as adapter
import mod_library as library
import mod_content as composer
import mod_format as fmt
from mod_components import sha,sha_bytes,save_json,apply_file
from package_mod import build
from mod_variants import choose

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
        from test_game_check import param_sfo
        (self.game/'sce_sys').mkdir()
        (self.game/'sce_sys/param.sfo').write_bytes(param_sfo({'TITLE_ID':'CUSA13795','APP_VER':'01.21'}))
        self.ini=self.port/'bbport.ini';self.ini.write_text('output_res=1920x1080\nrender_res=1080\nonline=0\n')
        self.prefs=self.port/'patches.json';save_json(self.prefs,{'enabled':[],'disabled':[]})
        adapter.ACTIVE=None;self.count=0
        self.addCleanup(setattr,adapter,'ACTIVE',None)
        import mod_file_paths
        self.addCleanup(mod_file_paths.configure,None)
    def manifest(self,ident='sample'):
        return {'schemaVersion':2,'id':ident,'name':ident,'version':'1.0.0','author':'tcosi',
            'description':'Synthetic fixture','requiresCapabilities':[],'files':[],
            'compatibility':{'runtime_versions':['test'],'runtime_sha256':[sha(self.port/'bin/sacredfire.exe')],
             'eboot_sha256':[sha(self.game/'eboot.bin')],'online':'offline-only'}}
    def package(self,ident='sample',manifest=None,files=None,filename=None):
        self.count+=1;source=self.root/('source'+str(self.count));source.mkdir()
        save_json(source/'mod.json',manifest or self.manifest(ident))
        for name,data in (files or {}).items():
            p=source/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
        dest=self.mods/(filename or ident+'.zip');build(source,dest);return dest
    def scan(self):return library.scan(self.mods,self.port/'patches')
    def enable(self,*names):save_json(self.prefs,{'enabled':[library.PREFIX+n for n in names],'disabled':[]})
    def prepare(self):
        self.environment={}
        with patch.object(adapter,'preflight',return_value=0),patch.object(adapter,'engine_features',return_value={'file_overlays':1}),patch.object(adapter,'launch_lock',return_value=nullcontext()),patch('atexit.register'):
            return adapter.prepare(self.port,self.port,self.environment,self.game,self.ini,prepare_only=True)
    def read_game(self,relative):
        import mod_file_paths
        with patch.object(mod_file_paths,'_settings',self.environment):
            return mod_file_paths.resolve(self.game/relative).read_bytes()
    def test_new_zip_listed_but_disabled(self):
        self.package();self.assertIn('sample',self.scan()['entries'])
        self.assertEqual(self.prepare(),(self.game,str(self.ini)))
    def test_creator_description_survives_catalog_and_xml(self):
        text='Creator description: "warm" & curved.\nSecond line <plain text>.'
        self.package(manifest=self.manifest()|{'description':text})
        self.assertEqual(self.scan()['entries']['sample']['description'],text)
        meta=ET.parse(self.port/'patches'/library.FILENAME).find('Metadata')
        self.assertEqual(meta.get('ModDescription'),text)
    def test_profile_publication_retries_transient_windows_lock(self):
        source=self.root/'building';source.mkdir();(source/'ready').write_text('complete')
        destination=self.root/'prepared';real_replace=os.replace
        locked=PermissionError('temporarily locked');locked.winerror=5
        calls=[]
        def replace(a,b):
            calls.append((a,b))
            if len(calls)<3:raise locked
            return real_replace(a,b)
        with patch.object(composer.os,'replace',side_effect=replace),patch.object(composer.time,'sleep'):
            composer.publish_profile(source,destination,lambda _:None)
        self.assertEqual(len(calls),3)
        self.assertEqual((destination/'ready').read_text(),'complete')
    def test_profile_publication_preserves_existing_destination(self):
        source=self.root/'building';source.mkdir()
        destination=self.root/'prepared';destination.mkdir();(destination/'keep').write_text('existing')
        locked=PermissionError('destination exists');locked.winerror=5
        with patch.object(composer.os,'replace',side_effect=locked),patch.object(composer.time,'sleep') as sleep:
            with self.assertRaises(PermissionError):composer.publish_profile(source,destination,lambda _:None)
        sleep.assert_not_called()
        self.assertTrue(source.is_dir());self.assertEqual((destination/'keep').read_text(),'existing')
    def test_two_packages_compose_and_defaults_precede_variant(self):
        uw=self.manifest('wide')|{'requiresCapabilities':['variant-settings','variants','settings'],
            'variantSettings':{'output_res':{'default':'5120x1440','allowed':['5120x1440','3840x1080']}},
            'variants':[{'id':'wide','when':{'output_res':['5120x1440','3840x1080']},'settings':{'temporal':'off'}}]}
        gauge=self.manifest('gauge')|{'requiresCapabilities':['game-files'],'operations':[
            {'kind':'game_file','target':'data.txt','original_sha256':sha_bytes(b'original'),'patched_sha256':sha_bytes(b'modified'),'payload':'payload.txt'}]}
        self.package('wide',uw);self.package('gauge',gauge,{'payload.txt':b'modified'});self.enable('wide','gauge')
        before=self.ini.read_bytes();game,config=self.prepare()
        self.assertEqual(game,self.game)
        self.assertEqual(self.read_game('data.txt'),b'modified')
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
    def test_identical_duplicate_ids_listed_once(self):
        self.package();self.package(filename='duplicate.zip')
        result=self.scan();self.assertEqual(list(result['entries']),['sample'])
        self.assertIn('Other copies retained',result['entries']['sample']['version_notice'])

    def test_newest_version_wins_numeric_order_and_keeps_toggle(self):
        self.package(manifest=self.manifest()|{'version':'1.9.0'},filename='older.zip')
        self.package(manifest=self.manifest()|{'version':'1.10.0'},filename='newer.zip')
        self.enable('sample');before=self.prefs.read_bytes()
        result=self.scan();self.assertEqual(result['entries']['sample']['version'],'1.10.0')
        self.assertEqual(self.prefs.read_bytes(),before)

    def test_same_version_different_content_has_clear_conflict(self):
        self.package(filename='one.zip')
        self.package(manifest=self.manifest()|{'description':'different content'},filename='two.zip')
        result=self.scan();self.assertNotIn('sample',result['entries'])
        self.assertIn('Conflicting content',result['messages'][0])

    def test_newest_version_ignores_legacy_engine_allowlist(self):
        self.package(filename='older.zip')
        m=self.manifest()|{'version':'2.0.0'};m['compatibility']['runtime_versions']=['future']
        self.package(manifest=m,filename='future.zip')
        result=self.scan();self.assertEqual(result['entries']['sample']['version'],'2.0.0')

    def test_preparation_never_copies_or_links_source_game(self):
        self.package();self.enable('sample')
        real_copy=shutil.copy2
        def guarded(source,target,*args,**kwargs):
            self.assertFalse(Path(source).resolve().is_relative_to(self.game))
            return real_copy(source,target,*args,**kwargs)
        before={p.relative_to(self.game).as_posix():p.read_bytes() for p in self.game.rglob('*') if p.is_file()}
        with patch.object(shutil,'copy2',side_effect=guarded),patch('os.link',side_effect=AssertionError('No game links')):
            game,_=self.prepare();game2,_=self.prepare()
        self.assertEqual((game,game2),(self.game,self.game))
        self.assertFalse((self.port/'mod-state/extensions/profiles').exists())
        self.assertEqual(before,{p.relative_to(self.game).as_posix():p.read_bytes() for p in self.game.rglob('*') if p.is_file()})
    def test_catalog_provides_native_mod_name_and_zip_identity(self):
        package=self.package(manifest=self.manifest()|{'name':'Readable mod name'})
        self.scan()
        meta=ET.parse(self.port/'patches'/library.FILENAME).getroot().find('Metadata')
        self.assertEqual(meta.get('ModName'),'Readable mod name');self.assertEqual(meta.get('ModHash'),sha(package))
        self.assertEqual(meta.get('Name'),'Mod: sample')
    def test_remove_keeps_zip_saves_and_unrelated_selection(self):
        package=self.package();digest=sha(package);self.enable('sample')
        prefs=json.loads(self.prefs.read_text());prefs['enabled'].append('other.xml/Other');prefs['custom']='keep';save_json(self.prefs,prefs)
        save=self.port/'mod-state/user/savedata/progress';save.parent.mkdir(parents=True);save.write_bytes(b'keep save')
        removed=library.remove(self.mods,self.port/'patches','sample',digest)
        self.assertFalse(package.exists());self.assertEqual(sha(removed),digest)
        self.assertTrue(removed.is_relative_to(self.port/'mod-state/removed'))
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
        with launch_lock(self.port/'mod-state'):
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
    def test_old_finished_sessions_are_pruned(self):
        sessions=self.root/'mod-state/sessions'
        results={'a':0,'b':0,'c':0,'d':0,'e':139,'f':None}  # oldest first; e crashed, f unfinished
        for age,(name,code) in enumerate(results.items()):
            path=sessions/name;(path/'user/savedata').mkdir(parents=True)
            if code is not None:save_json(path/'result.json',{'exit_code':code})
            os.utime(path,(1000+age,1000+age))
        os.utime(sessions/'a',(1,1))
        deleted=adapter.prune_sessions(sessions,keep=3)
        # The three newest (d, e, f) stay; of the older a-c only finished ones go.
        self.assertEqual(sorted(p.name for p in deleted),['a','b','c'])
        self.assertEqual(sorted(p.name for p in sessions.iterdir()),['d','e','f'])
        # A crashed session past the latest three is kept: it may hold the only copy of its saves.
        for name in ('g','h','i'):
            (sessions/name).mkdir();save_json(sessions/name/'result.json',{'exit_code':0})
        self.assertEqual(adapter.prune_sessions(sessions,keep=3),[sessions/'d'])
        self.assertTrue((sessions/'e').exists() and (sessions/'f').exists())
    def test_offline_package_cannot_start_online(self):
        self.package();self.enable('sample');self.ini.write_text(self.ini.read_text().replace('online=0','online=1'))
        with self.assertRaisesRegex(ValueError,'offline-only'):self.prepare()
    def test_engine_update_does_not_require_repackaging(self):
        self.package();self.enable('sample');(self.port/'bin/sacredfire.exe').write_bytes(b'other engine')
        (self.port/'bin/VERSION').write_text('future-release')
        self.assertEqual(self.prepare()[0],self.game)

    def test_package_can_omit_engine_versions_and_hashes(self):
        m=self.manifest();m['compatibility'].pop('runtime_versions');m['compatibility'].pop('runtime_sha256')
        self.package(manifest=m);self.enable('sample')
        self.assertEqual(self.prepare()[0],self.game)

    def test_whole_executable_checksum_is_not_an_allowlist(self):
        self.package();self.enable('sample');(self.game/'eboot.bin').write_bytes(b'different executable wrapper')
        self.assertEqual(self.prepare()[0],self.game)

    def test_package_can_omit_all_executable_allowlists(self):
        m=self.manifest();m['compatibility']={'online':'offline-only'}
        self.package(manifest=m);self.enable('sample')
        self.assertEqual(self.prepare()[0],self.game)

    def test_engine_feature_negotiation_is_not_a_version_allowlist(self):
        adapter.engine_features.cache_clear()
        from types import SimpleNamespace
        with patch.object(adapter.subprocess,'run',return_value=SimpleNamespace(returncode=0,stdout='{"file_overlays":1}')):
            self.assertEqual(adapter.engine_features('any-future-engine','digest'),{'file_overlays':1})
        adapter.engine_features.cache_clear()
    def test_preflight_requires_proof_for_every_generated_mod_write(self):
        from test_prepare import fixture
        profile=self.root/'profile';(profile/'game').mkdir(parents=True);(profile/'patches').mkdir()
        (profile/'game/eboot.bin').write_bytes(fixture())
        (profile/'patches/OCTR-Mod-Content.xml').write_text('<Patch><Metadata Name="unsigned" AppVer="01.21" isEnabled="true"><PatchList><Line Type="bytes" Address="0x400002" Value="90" /></PatchList></Metadata></Patch>')
        with self.assertRaisesRegex(ValueError,'unverified executable write'):
            adapter.preflight(profile,self.ini,self.prefs)

    def test_preflight_expected_code_accepts_unrelated_self_changes(self):
        from test_prepare import fixture
        profile=self.root/'profile';(profile/'game').mkdir(parents=True);(profile/'patches').mkdir()
        executable=fixture();executable[-1]=99
        (profile/'game/eboot.bin').write_bytes(executable)
        (profile/'patches/OCTR-Mod-Content.xml').write_text('<Patch><Metadata Name="signed" AppVer="01.21" isEnabled="true"><PatchList><Line Type="bytes" Address="0x400002" Value="90" ExpectedSHA256="'+sha_bytes(bytes([2]))+'" /></PatchList></Metadata></Patch>')
        self.assertEqual(adapter.preflight(profile,self.ini,self.prefs),1)
        executable[0x202]=99;(profile/'game/eboot.bin').write_bytes(executable)
        with self.assertRaisesRegex(ValueError,'code signature mismatch'):
            adapter.preflight(profile,self.ini,self.prefs)

    def test_new_signature_capability_validates_all_writes(self):
        m=self.manifest();m['compatibility']={'online':'allowed'}
        m['requiresCapabilities']=['xml-patches','code-signatures'];m['patches']=['patches/p.xml']
        xml=b'<Patch><TitleID><ID>CUSA13795</ID></TitleID><Metadata Name="missing" AppVer="01.21" AppElf="eboot.bin"><PatchList><Line Type="bytes" Address="0x400002" Value="90" /></PatchList></Metadata></Patch>'
        with self.assertRaisesRegex(ValueError,'every write'):
            self.package(manifest=m,files={'patches/p.xml':xml})

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

    def test_preflight_takes_the_settings_path_as_text(self):
        # run.py without a widescreen layout: bbport.ini's path as mod_packages.prepare returns it.
        from test_prepare import fixture
        profile=self.root/'profile';(profile/'game').mkdir(parents=True);(profile/'patches').mkdir()
        (profile/'game/eboot.bin').write_bytes(fixture())
        self.assertEqual(adapter.preflight(profile,str(self.ini),self.prefs),0)

    def test_preflight_builtin_render_patches_without_xml_path(self):
        from test_patches import tiny_elf
        from patches import RENDER_SCALE_SIGNATURE,OUTPUT_DOUBLE_SIGNATURE,GPU_HEAP_SIGNATURE
        code=bytes.fromhex(OUTPUT_DOUBLE_SIGNATURE)+b'\0'*16
        code+=bytes.fromhex(RENDER_SCALE_SIGNATURE)+b'\0'*16+bytes.fromhex(GPU_HEAP_SIGNATURE)
        elf=tiny_elf(code)
        for resolution,count in [('480',3),('720',3),('1080',0),('1440',0),('1620',1),('2160',2)]:
            with self.subTest(resolution=resolution):
                self.ini.write_text('render_res='+resolution+'\nonline=0\n')
                with patch('prepare.parse_self',return_value=(elf,)),patch.dict(os.environ,{'BB_ONLINE':'0'}):
                    self.assertEqual(adapter.preflight(self.port,self.ini,self.prefs,game=self.game),count)

    def test_preflight_rejects_signature_skip(self):
        from test_prepare import fixture
        profile=self.root/'profile';(profile/'game').mkdir(parents=True);(profile/'patches').mkdir()
        (profile/'game/eboot.bin').write_bytes(fixture())
        (profile/'patches/test.xml').write_text('<Patch><TitleID><ID>CUSA13795</ID></TitleID><Metadata Name="missing" AppVer="01.21" AppElf="eboot.bin" isEnabled="true"><PatchList><Line Type="bytes" Address="0x400002" Value="90" Signature="fe ed fa ce" /></PatchList></Metadata></Patch>')
        with self.assertRaisesRegex(ValueError,'Incomplete executable patch'):adapter.preflight(profile,self.ini,self.prefs)

if __name__=='__main__':unittest.main()
