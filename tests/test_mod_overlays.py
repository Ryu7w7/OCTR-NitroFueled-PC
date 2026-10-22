"""Sparse overlays replace virtual archive entries without game copies."""
from paths import ROOT
from pathlib import Path
import tempfile,unittest,struct
from unittest.mock import patch
from test_mod_assets import pak
from mod_components import sha_bytes
from mod_overlays import compose,file_map
import archive_patch,mod_file_paths

class OverlayTests(unittest.TestCase):
    def test_builtin_archive_preparation_reads_replacement_pak(self):
        from archive_reader import Archive
        import gcn_shaders
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);game=root/'game';(game/'archives').mkdir(parents=True)
            original=game/'archives/update2.pak';original.write_bytes(pak(b'ORIGINAL'))
            replacement=root/'mod/update2.pak';replacement.parent.mkdir();replacement.write_bytes(pak(b'REPLACEMENT PAK'))
            mapping=root/'map';dirs=root/'dirs'
            file_map(game,mapping,dirs,[(Path('archives/update2.pak'),replacement)])
            env={'BB_MOD_FILE_MAP':str(mapping),'BB_MOD_SOURCE_GAME':str(game)}
            with patch.object(mod_file_paths,'_settings',env):
                self.assertEqual(mod_file_paths.archives(game/'archives'),[replacement])
                a=Archive(original)
                try:self.assertEqual(a.extract(a.files[0]),b'REPLACEMENT PAK')
                finally:a.data.close();a._file.close()
                self.assertEqual(gcn_shaders.pak_entries(original)[0][2],len(b'REPLACEMENT PAK'))
                output=root/'builtin.bbpatch'
                archive_patch.write_entry_patch(output,original,'/app0/archives/update2.pak',0,b'RESTYLED')
                self.assertEqual(archive_patch.read(output)[0],replacement.stat().st_size)
            self.assertEqual(original.read_bytes(),pak(b'ORIGINAL'))
    def test_sparse_archive_keeps_source_and_combines_builtin_ranges(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);game=root/'game';archives=game/'archives';archives.mkdir(parents=True)
            source=archives/'update2.pak';source.write_bytes(pak(b'ORIGINAL'))
            with source.open('r+b') as f:f.truncate(64*1024**2)
            folder=root/'mod';folder.mkdir();(folder/'payload').write_bytes(b'CUSTOM')
            op={'kind':'archive_asset','archive':'update2.pak','asset':'ui/example.igz','index':0,
                'original_sha256':sha_bytes(b'ORIGINAL'),'patched_sha256':sha_bytes(b'CUSTOM'),'payload':'payload'}
            builtin=root/'builtin.bbpatch';guest='/app0/archives/update2.pak'
            archive_patch.write(builtin,source,guest,[(200,b'BUILTIN')])
            before=source.stat().st_size;header=source.read_bytes()[:4096]
            with patch('shutil.copy2',side_effect=AssertionError('No game copy')):
                made=compose(game,root/'out',[(op,folder)],{guest:builtin})
            result=made[guest];self.assertLess(result.stat().st_size,4096)
            self.assertEqual(archive_patch.view(source,result,200,7),b'BUILTIN')
            record=archive_patch.view(source,result,60,16);offset,extra,length,mode=struct.unpack('<4I',record)
            self.assertEqual(archive_patch.view(source,result,offset|((extra&255)<<32),length),b'CUSTOM')
            self.assertEqual(source.stat().st_size,before);self.assertEqual(source.read_bytes()[:4096],header)
            with self.assertRaisesRegex(ValueError,'already replaced'):
                compose(game,root/'conflict',[(op,folder)],made)
    def test_file_map_redirects_host_reads_without_game_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);game=root/'game';game.mkdir();(game/'original').write_bytes(b'SOURCE')
            mod=root/'replacement';mod.write_bytes(b'MOD')
            mapping=root/'map';dirs=root/'dirs'
            file_map(game,mapping,dirs,[(Path('original'),mod)])
            with patch.object(mod_file_paths,'_settings',{'BB_MOD_FILE_MAP':str(mapping),'BB_MOD_SOURCE_GAME':str(game)}):
                self.assertEqual(mod_file_paths.resolve(game/'original'),mod)
                self.assertEqual(mod_file_paths.resolve(game/'absent'),game/'absent')
            self.assertEqual((game/'original').read_bytes(),b'SOURCE')
            self.assertEqual(list(dirs.iterdir()),[])
            with self.assertRaisesRegex(ValueError,'Conflicting file'):
                file_map(game,mapping,dirs,[(Path('x'),mod),(Path('x/y'),mod)])
