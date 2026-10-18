from paths import ROOT
"""Cold-cache setup, normal closure, failure and reuse regressions."""
from pathlib import Path
import tempfile,unittest
from unittest.mock import patch,Mock
import nitro_shader_setup as setup
from nitro_components import sha_bytes

class ShaderSetup(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=Path(__file__).parent);self.root=Path(self.tmp.name)
        self.user=self.root/'user';self.user.mkdir();self.op={'kind':'shader_bytes','target':'cache/CUSA13795/test.spv','original_sha256':sha_bytes(b'original'),'patched_sha256':sha_bytes(b'modded!!'),'runs':[[0,b'original'.hex(),b'modded!!'.hex()]]}
        self.ops=[(self.op,self.root)];self.status=Mock()
    def tearDown(self):self.tmp.cleanup()
    def ready(self,user=None):
        cache=(user or self.user)/'cache/CUSA13795';cache.mkdir(parents=True,exist_ok=True)
        for name,data in [('profile.bin',b'profile'),('test.meta',b'meta'),('test.key',b'key'),('test.spv',b'original')]:
            (cache/name).write_bytes(data)
    def plan(self):return {'operations':self.ops,'engine':'engine-hash','packages':['shader-mod']}
    def test_required_shader_missing_even_when_metadata_exists(self):
        self.ready();(self.user/self.op['target']).unlink()
        missing,wrong=setup.requirements(self.user,self.ops)
        self.assertEqual(missing,['test.spv']);self.assertEqual(wrong,[])
    def test_incompatible_existing_shader_is_not_treated_as_missing(self):
        self.ready();(self.user/self.op['target']).write_bytes(b'wrong')
        with patch.object(setup.subprocess,'Popen') as start,self.assertRaisesRegex(ValueError,'Unsupported'):
            setup.ensure_shaders(self.root,self.root,self.root,self.user,self.plan(),{},self.status)
        start.assert_not_called()
    def test_warm_cache_skips_game_setup(self):
        self.ready()
        with patch.object(setup.subprocess,'Popen') as start:
            self.assertFalse(setup.ensure_shaders(self.root,self.root,self.root,self.user,self.plan(),{},self.status))
        start.assert_not_called()
    def test_prepare_only_never_starts_game(self):
        with patch.object(setup.subprocess,'Popen') as start,self.assertRaisesRegex(ValueError,'Press Play'):
            setup.ensure_shaders(self.root,self.root,self.root,self.user,self.plan(),{},self.status,True)
        start.assert_not_called()
    def test_ready_inputs_request_normal_close_once(self):
        self.ready();process=Mock(pid=123);closed=[False];now=[0.0]
        process.poll.side_effect=lambda:0 if closed[0] else None;process.wait.return_value=0
        def closer(pid,exe):self.assertEqual(pid,123);closed[0]=True;return True
        close=Mock(side_effect=closer)
        setup.wait_for_setup(process,self.user,self.ops,self.root/'engine',self.status,
            clock=lambda:now[0],sleep=lambda n:now.__setitem__(0,now[0]+n),closer=close)
        close.assert_called_once();self.assertGreaterEqual(now[0],3)
        process.terminate.assert_not_called();process.kill.assert_not_called()
    def test_premature_exit_does_not_restart_or_close_another_game(self):
        process=Mock(pid=123);process.poll.return_value=0;process.wait.return_value=0;close=Mock()
        with self.assertRaisesRegex(ValueError,'before the required'):
            setup.wait_for_setup(process,self.user,self.ops,self.root/'engine',self.status,closer=close)
        close.assert_not_called()
    def test_failed_setup_does_not_accept_even_complete_cache(self):
        self.ready();process=Mock();process.poll.return_value=1;process.wait.return_value=1
        with self.assertRaisesRegex(ValueError,'unsuccessfully'):
            setup.wait_for_setup(process,self.user,self.ops,self.root/'engine',self.status)
    def test_private_setup_omits_mods_promotes_clean_cache_and_keeps_saves(self):
        state=self.root/'state';state.mkdir();(state/'bbport.ini').write_text('output_res=3440x1440\nrender_res=720\nonline=0\n')
        baseline=self.root/'baseline';(baseline/'game').mkdir(parents=True)
        captured={}
        def start(*args,**kw):
            captured.update(kw['env']);u=Path(kw['env']['BB_USER_DIR']);self.ready(u)
            save=u/'savedata/1/CUSA13795/save';save.parent.mkdir(parents=True);save.write_bytes(b'accepted agreement')
            return Mock()
        with patch('nitro_extensions.prepare',return_value={'profile':baseline}) as prepare,patch.object(setup.subprocess,'Popen',side_effect=start),patch.object(setup,'wait_for_setup'):
            self.assertTrue(setup.ensure_shaders(self.root,state,self.root,self.user,self.plan(),{'BB_NEO':'1','BB_MODS_ENABLED':'1'},self.status))
        self.assertEqual(captured['BB_MODS_ENABLED'],'0');self.assertEqual(captured['BB_ONLINE'],'0');self.assertEqual(captured['BB_NEO'],'0')
        self.assertEqual(Path(captured['BB_GAME_DIR']),baseline/'game')
        self.assertEqual((self.user/self.op['target']).read_bytes(),b'original')
        self.assertEqual((state/'extensions/vanilla-cache/engine-hash/CUSA13795/test.spv').read_bytes(),b'original')
        self.assertEqual((state/'user/savedata/1/CUSA13795/save').read_bytes(),b'accepted agreement')
        self.assertEqual((self.user/'savedata/1/CUSA13795/save').read_bytes(),b'accepted agreement')
        self.assertIn('3440x1440',(state/'bbport.ini').read_text())
        self.assertEqual(list(prepare.call_args.args[3].iterdir()),[])

if __name__=='__main__':unittest.main(verbosity=2)
