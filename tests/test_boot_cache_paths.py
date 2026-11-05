"""Boot consumers, path forms and ISA reuse. Written for native Windows and Linux CI."""
from paths import ROOT
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import boot_cache
import isa_rewrite
import launch_lock
from mod_components import long_path
from test_boot_cache import game_fixture


class BootPathTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.game = game_fixture(self.root / 'game ü')
        self.out = self.root / 'data with spaces' / 'out'
        env = mock.patch.dict(os.environ, {'BB_SKIP_GAME_CHECK': '0'})
        env.start()
        self.addCleanup(env.stop)

    def ensure(self, game=None, out=None):
        with redirect_stdout(io.StringIO()):
            return boot_cache.ensure(game or self.game, out or self.out, log=lambda _: None)

    def link_directory(self, link, target):
        if os.name == 'nt':
            import _winapi
            _winapi.CreateJunction(str(target), str(link))
        else:
            link.symlink_to(target, target_is_directory=True)

    def test_game_symlink_or_junction_resolves_same_inputs(self):
        target = self.ensure()
        linked = self.root / 'linked game'
        self.link_directory(linked, self.game)
        try:
            self.assertEqual(self.ensure(game=linked), target)
        finally:
            if os.name == 'nt':
                linked.rmdir()
            else:
                linked.unlink()

    def test_cache_symlink_or_junction_never_deletes_external_files(self):
        external = self.root / 'external'
        external.mkdir()
        sentinel = external / 'keep.txt'
        sentinel.write_text('untouched')
        self.out.mkdir(parents=True)
        linked = self.out / 'boot-cache'
        self.link_directory(linked, external)
        try:
            with self.assertRaisesRegex(ValueError, 'symlink'):
                self.ensure()
            self.assertEqual(sentinel.read_text(), 'untouched')
        finally:
            if os.name == 'nt':
                linked.rmdir()
            else:
                linked.unlink()

    def test_output_directory_alias_uses_the_same_process_lock(self):
        self.out.mkdir(parents=True)
        alias = self.root / 'aliased data'
        alias.mkdir()
        linked = alias / 'out'
        self.link_directory(linked, self.out)
        try:
            self.assertEqual(launch_lock.lock_path(alias), launch_lock.lock_path(self.out.parent))
            code = ('import sys;sys.path.insert(0,sys.argv[1]);import launch_lock;\n'
                    'with launch_lock.session(sys.argv[2]): pass')
            with launch_lock.session(self.out.parent):
                result = subprocess.run([sys.executable, '-c', code, str(ROOT / 'scripts'), str(alias)],
                                        capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Another game launch', result.stderr)
        finally:
            if os.name == 'nt':
                linked.rmdir()
            else:
                linked.unlink()

    @unittest.skipUnless(os.name == 'nt', 'Windows extended-length paths')
    def test_cache_beyond_max_path_without_symlink_privilege(self):
        deep = long_path(self.root) / ('a' * 110) / ('b' * 110)
        try:
            target = self.ensure(out=deep / 'out')
            self.assertGreater(len(str(target)), 260)
            self.assertTrue((target / 'boot-linked.bin').is_file())
            self.assertEqual(self.ensure(out=deep / 'out'), target)
        finally:
            shutil.rmtree(long_path(self.root) / ('a' * 110), ignore_errors=True)

    def test_patch_cli_reads_selected_boot_and_writes_only_dynamic_output(self):
        boot = self.ensure()
        (self.out / 'eboot.elf').write_bytes(b'stale legacy ELF must not be used')
        config = self.root / 'bbport.ini'
        config.write_text('')
        patches = self.root / 'empty patches'
        patches.mkdir()
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/patches.py'),
                                 '--boot-dir', str(boot), '--out', str(self.out),
                                 '--patches-dir', str(patches), '--settings', str(config)],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.out / 'patches.bin').is_file())
        self.assertFalse((boot / 'patches.bin').exists())
        self.assertEqual((self.out / 'eboot.elf').read_bytes(), b'stale legacy ELF must not be used')

    @unittest.skipIf(isa_rewrite.iced_x86 is None, 'iced-x86 is required')
    def test_isa_cache_checks_generator_and_table_integrity(self):
        boot = self.ensure()
        out = boot / 'isa'
        args = [str(boot / 'boot-linked.bin'), '--out', str(out)]
        with redirect_stdout(io.StringIO()):
            self.assertEqual(isa_rewrite.main(args), 0)
            snapshot = {p.name: (p.stat().st_mtime_ns, p.read_bytes()) for p in out.iterdir()}
            with mock.patch.object(isa_rewrite, 'scan_segment', side_effect=AssertionError('ISA hit rescanned')):
                self.assertEqual(isa_rewrite.main(args), 0)
            self.assertEqual(snapshot, {p.name: (p.stat().st_mtime_ns, p.read_bytes()) for p in out.iterdir()})
            stamp = out / 'isa-rewrite.json'
            stale = json.loads(stamp.read_text())
            stale['generator'] = ['old generator']
            stamp.write_text(json.dumps(stale))
            with mock.patch.object(isa_rewrite, 'scan_segment', wraps=isa_rewrite.scan_segment) as scan:
                self.assertEqual(isa_rewrite.main(args), 0)
                self.assertTrue(scan.called)
            table = out / 'isa-rewrite.bin'
            table.write_bytes(table.read_bytes()[:-1])
            with mock.patch.object(isa_rewrite, 'scan_segment', wraps=isa_rewrite.scan_segment) as scan:
                self.assertEqual(isa_rewrite.main(args), 0)
                self.assertTrue(scan.called)


if __name__ == '__main__':
    unittest.main()
