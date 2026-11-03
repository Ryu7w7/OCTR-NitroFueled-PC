"""Compile and exercise the real guest file read/stat/directory/write paths."""
from paths import ROOT
from pathlib import Path
import os,shutil,subprocess,tempfile,unittest
from mod_overlays import file_map

class RuntimeModFileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler=shutil.which(os.environ.get('CC','clang')) or shutil.which('gcc')
        if not compiler:raise unittest.SkipTest('C compiler unavailable')
        cls.temp=tempfile.TemporaryDirectory();cls.folder=Path(cls.temp.name)
        cls.binary=cls.folder/('runtime-files.exe' if os.name=='nt' else 'runtime-files')
        result=subprocess.run([compiler,'-std=gnu11','-ffunction-sections','-fdata-sections',
            str(ROOT/'tests/runtime_mod_files.c'),'-Wl,--gc-sections','-pthread',*(['-static'] if os.name=='nt' else []),'-o',str(cls.binary)],capture_output=True,text=True)
        if result.returncode:raise AssertionError(result.stderr)
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def test_intro_setting_and_graphics_archive_gate(self):
        for skip in (0,1):
            with tempfile.TemporaryDirectory(dir=self.folder) as tmp:
                root=Path(tmp);game=root/'game';game.mkdir();(game/'archives').mkdir()
                user=root/'user';user.mkdir()
                for name in ('octane_intro_logo_atvi.bk2','openinglogo.bk2'):
                    (game/name).write_text('INTRO')
                (game/'race.bk2').write_text('RACE')
                overlay=root/'menus.pak';overlay.write_text('MENUS')
                config=root/'bbport.ini';config.write_text(f'skip_intro={skip}\n')
                env=os.environ.copy()
                for key in ('BB_MOD_FILE_MAP','BB_MOD_FILE_DIRS','BB_ARCHIVE_PATCHES','BB_PRIVATE_ARCHIVE_PATCHES'):
                    env.pop(key,None)
                env.update(BB_CONFIG=str(config),BB_GRAPHICS_TAB_ARCHIVE=str(overlay))
                result=subprocess.run([str(self.binary),str(game),str(user),'menus-on' if skip else 'menus-off'],
                                      env=env,capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_large_archive_patch(self):
        """A mod pack's overlay: more ranges than the old 4096 and entries read from the file."""
        import archive_patch
        with tempfile.TemporaryDirectory(dir=self.folder) as tmp:
            root=Path(tmp);game=root/'game';(game/'archives').mkdir(parents=True)
            user=root/'user';user.mkdir()
            archive=game/'archives/update.pak';archive.write_bytes(bytes(range(256))*4096)
            size=archive.stat().st_size
            ranges=[(i*97,bytes([i&255])*16) for i in range(5000)]
            ranges+=[(size+i*300000,os.urandom(200000+i)) for i in range(6)]
            patch=root/'update.bbpatch';archive_patch.write(patch,archive,'/app0/archives/update.pak',ranges)
            expected=archive_patch.view(archive,patch,0,size+6*300000)
            env=os.environ.copy()
            for key in ('BB_MOD_FILE_MAP','BB_MOD_FILE_DIRS','BB_PRIVATE_ARCHIVE_PATCHES'):env.pop(key,None)
            env.update(BB_ARCHIVE_PATCHES=str(patch),BB_TEST_DUMP=str(root/'dump.bin'))
            result=subprocess.run([str(self.binary),str(game),str(user),'dump=/app0/archives/update.pak'],
                                  env=env,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('5006 ranges',result.stdout)
            self.assertEqual((root/'dump.bin').read_bytes(),expected)

    def test_itemless_race_after_lobby_scene_unloads(self):
        import struct
        with tempfile.TemporaryDirectory(dir=self.folder) as tmp:
            root=Path(tmp);game=root/'game';(game/'archives').mkdir(parents=True)
            user=root/'user';user.mkdir()
            (game/'archives/race.pak').write_bytes(b'SOURCE__')
            env=os.environ.copy()
            for key in ('BB_MOD_FILE_MAP','BB_MOD_FILE_DIRS'):
                env.pop(key,None)
            path=b'/app0/archives/race.pak'
            for key, data in (('BB_ARCHIVE_PATCHES',b'NORMAL__'),('BB_PRIVATE_ARCHIVE_PATCHES',b'ITEMLESS')):
                patch=root/key
                patch.write_bytes(b'BBPAK001'+struct.pack('<QI',8,len(path))+path+
                                  struct.pack('<IQI',1,0,len(data))+data)
                env[key]=str(patch)
            result=subprocess.run([str(self.binary),str(game),str(user),'itemless-race'],
                                  env=env,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_enabled_disabled_and_invalid_map_preserve_source(self):
        game=self.folder/'game';game.mkdir()
        user=self.folder/'user';user.mkdir()
        (game/'data.txt').write_text('SOURCE');(game/'unchanged.txt').write_text('UNCHANGED')
        mod=self.folder/'mod';mod.mkdir();(mod/'replacement').write_text('MOD');(mod/'extra').write_text('ADDED')
        mapping=self.folder/'map.bin';dirs=self.folder/'directories'
        file_map(game,mapping,dirs,[(Path('data.txt'),mod/'replacement'),(Path('new/nested.txt'),mod/'extra')])
        env=os.environ.copy();env.update(BB_MOD_FILE_MAP=str(mapping),BB_MOD_FILE_DIRS=str(dirs))
        for mode in ('enabled','disabled','broken'):
            current=env.copy()
            if mode=='disabled':current.pop('BB_MOD_FILE_MAP');current.pop('BB_MOD_FILE_DIRS')
            if mode=='broken':mapping.write_bytes(b'broken')
            result=subprocess.run([str(self.binary),str(game),str(user),mode],env=current,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertEqual((game/'data.txt').read_text(),'SOURCE')
        self.assertEqual(sorted(p.name for p in game.iterdir()),['data.txt','unchanged.txt'])
        self.assertEqual((mod/'replacement').read_text(),'MOD')
