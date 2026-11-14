"""Obsolete generated caches are removed without following external links."""
from paths import ROOT
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import cache_cleanup
import native_shaders


class CacheCleanupTests(unittest.TestCase):
    def test_windows_reparse_point_detection_without_path_is_junction(self):
        import stat
        from types import SimpleNamespace
        attributes=SimpleNamespace(st_mode=stat.S_IFDIR,st_file_attributes=0x400)
        with patch.object(Path,'lstat',return_value=attributes):
            self.assertTrue(cache_cleanup.linked(Path('junction')))

    def test_prune_keeps_current_unknown_and_linked_folders(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            current, old, unknown, outside = [root/name for name in ('aaaa','bbbb','notes','outside')]
            for folder in (current,old,unknown,outside):
                folder.mkdir(); (folder/'stamp.json').write_text('{}')
            link = root/'cccc'
            try:
                link.symlink_to(outside,target_is_directory=True)
            except OSError:
                link = None
            cache_cleanup.prune(current,'stamp.json',r'[a-f]{4}')
            self.assertFalse(old.exists())
            self.assertTrue(current.is_dir() and unknown.is_dir() and outside.is_dir())
            if link is not None:
                self.assertTrue(link.is_symlink())

    def test_deferred_windows_delete_is_retried(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            for name in ('aaaa','bbbb'):
                (root/name).mkdir(); (root/name/'stamp.json').write_text('{}')
            messages=[]
            with patch.object(Path,'iterdir',side_effect=PermissionError('directory in use')):
                cache_cleanup.prune(root/'aaaa','stamp.json',r'[a-f]{4}',messages.append)
            self.assertTrue((root/'bbbb').exists())
            with patch.object(cache_cleanup.shutil,'rmtree',side_effect=PermissionError('file in use')):
                cache_cleanup.prune(root/'aaaa','stamp.json',r'[a-f]{4}',messages.append)
            self.assertTrue(messages and (root/'bbbb').exists())
            cache_cleanup.prune(root/'aaaa','stamp.json',r'[a-f]{4}')
            self.assertFalse((root/'bbbb').exists())

    def test_discard_rejects_external_files_and_junctions(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); cache=root/'cache';cache.mkdir()
            old=cache/'old.bbpatch';old.write_bytes(b'old')
            outside=root/'save.dat';outside.write_bytes(b'save')
            cache_cleanup.discard_files(cache,[old,outside])
            self.assertFalse(old.exists());self.assertEqual(outside.read_bytes(),b'save')
            old.write_bytes(b'old')
            real=cache_cleanup.linked
            with patch.object(cache_cleanup,'linked',side_effect=lambda path: Path(path)==cache or real(path)):
                cache_cleanup.discard_files(cache,[old])
            self.assertTrue(old.exists())

    def test_graphics_update_same_size_rebuilds_and_deletes_old_archive(self):
        import contextlib
        import io
        import os
        import sys
        import graphics_tab
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);game=root/'game';(game/'archives').mkdir(parents=True)
            archive=game/'archives/update2.pak';archive.write_bytes(b'AAA')
            out=root/'out'
            def build(game,dest,dump):
                dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(b'generated')
            with (patch.object(sys,'argv',['graphics_tab.py',str(game),'--out',str(out)]),
                  patch.object(graphics_tab,'enabled',return_value=True),
                  patch.object(graphics_tab,'build',side_effect=build) as compile,
                  contextlib.redirect_stdout(io.StringIO())):
                graphics_tab.main();graphics_tab.main()
                self.assertEqual(compile.call_count,1)
                old=next((out/'graphics-tab').glob('*.pak'))
                before=archive.stat();archive.write_bytes(b'BBB')
                os.utime(archive,ns=(before.st_atime_ns,before.st_mtime_ns+1_000_000_000))
                graphics_tab.main()
                self.assertEqual(compile.call_count,2)
                self.assertFalse(old.exists())
                self.assertEqual(len(list((out/'graphics-tab').glob('*.pak'))),1)

    def test_disabled_item_rules_delete_only_generated_cache(self):
        import item_rules
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);cache=root/'item_rules';cache.mkdir()
            for name in ('update2.bbpatch','update2.stamp.json','notes.txt'):
                (cache/name).write_text('old')
            self.assertEqual(item_rules.prepare(root,root,None,{}),{})
            self.assertEqual([p.name for p in cache.iterdir()],['notes.txt'])

    def test_old_shader_reset_backups_are_deleted_without_touching_saves(self):
        import mod_packages
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);session=root/'session'
            backup=session/'cache-before-reset';backup.mkdir(parents=True)
            (backup/'old.spv').write_bytes(b'cache')
            save=session/'user/savedata/player';save.parent.mkdir(parents=True)
            save.write_bytes(b'player progress')
            mod_packages.prune_sessions(root)
            self.assertFalse(backup.exists())
            self.assertEqual(save.read_bytes(),b'player progress')

    def test_native_shader_cache_tracks_code_game_and_removes_old_generation(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'scripts/native_shaders.py';source.parent.mkdir()
            source.write_text('generator')
            game=root/'game';(game/'archives').mkdir(parents=True)
            (game/'eboot.bin').write_bytes(b'boot')
            archive=game/'archives/shaders_ps4.pak';archive.write_bytes(b'shaders')
            current=root/f'CUSA13795-01.21-g{native_shaders.GENERATION}'
            old=root/'CUSA13795-01.21-g0'
            for folder in (current,old):folder.mkdir()
            (old/'complete.json').write_text('{}')
            with patch.object(native_shaders,'__file__',str(source)):
                kept={'generation':native_shaders.GENERATION,'sources':native_shaders.source_stamp(game)}
                (current/'complete.json').write_text(json.dumps(kept))
                self.assertTrue(native_shaders.ready(current,game))
                self.assertFalse(old.exists())
                archive.write_bytes(b'updated')
                self.assertFalse(native_shaders.ready(current,game))
                kept['sources']=native_shaders.source_stamp(game)
                (current/'complete.json').write_text(json.dumps(kept))
                source.write_text('updated generator')
                self.assertFalse(native_shaders.ready(current,game))
