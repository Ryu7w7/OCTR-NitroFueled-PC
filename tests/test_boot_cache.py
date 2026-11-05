"""Boot cache contracts using generated ELF/SFO files, never proprietary game assets."""
from paths import ROOT
from contextlib import redirect_stdout
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import boot_cache
import launch_lock
import link_libc
import link_modules
import prepare
from test_game_check import param_sfo


def executable(library='libc'):
    """Small loadable ELF with dynamic symbols, procparam and TLS for the real pipeline."""
    data = bytearray(0x2400)
    struct.pack_into('<16sHHIQQQIHHHHHH', data, 0,
                     b'\x7fELF\x02\x01\x01\x09', 0xfe10, 62, 1, 0x400, 64, 0, 0, 64, 56, 5, 0, 0, 0)
    tags = [(0x61000035, 0), (0x61000037, 64), (0x61000039, 0x100), (0x6100003f, 48),
            (0x61000029, 0x200), (0x6100002d, 0), (0x6100002f, 0x200), (0x61000031, 0),
            (0x61000013, (1 << 32) | 32), (0x6100000d, (1 << 32) | 32), (12, 0x400), (0, 0)]
    headers = [(1, 7, 0, 0, 0, len(data), 0x3000, 0x1000),
               (2, 4, 0x800, 0x800, 0, len(tags) * 16, len(tags) * 16, 8),
               (0x61000000, 4, 0x2000, 0x2000, 0, 0x400, 0x400, 8),
               (0x61000001, 4, 0x700, 0x700, 0, 32, 32, 8),
               (7, 4, 0x600, 0x600, 0, 4, 16, 16)]
    for i, header in enumerate(headers):
        struct.pack_into('<IIQQQQQQ', data, 64 + i * 56, *header)
    for i, tag in enumerate(tags):
        struct.pack_into('<QQ', data, 0x800 + i * 16, *tag)
    name = (prepare.nid('_init_env') + '#A#A\0').encode()
    data[0x2001:0x2001 + len(name)] = name
    library_name = library.encode() + b'\0'
    data[0x2020:0x2020 + len(library_name)] = library_name
    struct.pack_into('<IBBHQQ', data, 0x2118, 1, 0x12, 0, 1, 0x400, 1)
    data[0x400] = 0xc3
    return bytes(data)


def game_fixture(root, title='CUSA13795', version='01.21'):
    root.mkdir(parents=True)
    (root / 'sce_module').mkdir()
    (root / 'sce_sys').mkdir()
    (root / 'eboot.bin').write_bytes(executable())
    for name in boot_cache.MODULES:
        (root / 'sce_module' / name).write_bytes(executable(Path(name).stem))
    (root / 'sce_sys/param.sfo').write_bytes(param_sfo(dict(TITLE='Fixture', TITLE_ID=title, APP_VER=version)))
    return root


class BootCacheTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.game = game_fixture(self.root / 'game with spaces ü')
        self.out = self.root / 'data ü' / 'out'
        self.environment = mock.patch.dict(os.environ, {'BB_SKIP_GAME_CHECK': '0'})
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def ensure(self, game=None):
        with redirect_stdout(io.StringIO()):
            return boot_cache.ensure(game or self.game, self.out, log=lambda _: None)

    def test_real_pipeline_equivalence_and_hit_without_writes_or_build(self):
        baseline = self.root / 'baseline'
        with redirect_stdout(io.StringIO()):
            prepare.prepare(self.game, baseline)
            link_libc.link(self.game, baseline)
            link_modules.link(self.game, baseline, boot_cache.MODULES)
        target = self.ensure()
        for name in boot_cache.OUTPUTS:
            if name != 'analysis.json':
                self.assertEqual((target / name).read_bytes(), (baseline / name).read_bytes(), name)
        original = json.loads((baseline / 'analysis.json').read_text(encoding='utf-8'))
        report = json.loads((target / 'analysis.json').read_text(encoding='utf-8'))
        self.assertIn('Snapshot', report.pop('resource_inventory_scope'))
        self.assertEqual(report, original)
        snapshot = {p.name: (p.stat().st_mtime_ns, p.read_bytes()) for p in target.iterdir()}
        with mock.patch.object(boot_cache, 'build', side_effect=AssertionError('cache hit rebuilt')):
            self.assertEqual(self.ensure(), target)
        self.assertEqual(snapshot, {p.name: (p.stat().st_mtime_ns, p.read_bytes()) for p in target.iterdir()})

    def test_usa_eu_usa_reuses_separate_entries(self):
        usa = self.ensure()
        eu_game = game_fixture(self.root / 'EU', 'CUSA14876')
        eu = self.ensure(eu_game)
        self.assertNotEqual(usa, eu)
        self.assertEqual(usa.parent.name, 'CUSA13795')
        self.assertEqual(eu.parent.name, 'CUSA14876')
        with mock.patch.object(boot_cache, 'build', side_effect=AssertionError('region rebuilt')):
            self.assertEqual(self.ensure(), usa)
            self.assertEqual(self.ensure(eu_game), eu)

    def test_same_content_in_new_game_path_reuses_cache(self):
        target = self.ensure()
        moved = self.root / 'another game'
        shutil.copytree(self.game, moved)
        with mock.patch.object(boot_cache, 'build', side_effect=AssertionError('path caused rebuild')):
            self.assertEqual(self.ensure(moved), target)

    def test_each_input_change_with_same_size_and_timestamp_invalidates(self):
        old_key, _ = boot_cache.fingerprint(self.game)
        inputs = ['eboot.bin', 'sce_sys/param.sfo', *('sce_module/' + n for n in boot_cache.MODULES)]
        for name in inputs:
            with self.subTest(name=name):
                path = self.game / name
                old = path.read_bytes()
                stat = path.stat()
                path.write_bytes(old[:-1] + bytes([old[-1] ^ 1]))
                os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
                self.assertNotEqual(boot_cache.fingerprint(self.game)[0], old_key)
                path.write_bytes(old)

    def test_generator_schema_module_order_and_policy_invalidate(self):
        original = boot_cache.fingerprint(self.game)[0]
        with mock.patch.object(boot_cache, 'SCHEMA', boot_cache.SCHEMA + 1):
            self.assertNotEqual(boot_cache.fingerprint(self.game)[0], original)
        with mock.patch.object(boot_cache, 'MODULES', tuple(reversed(boot_cache.MODULES))):
            self.assertNotEqual(boot_cache.fingerprint(self.game)[0], original)
        with mock.patch.dict(os.environ, {'BB_SKIP_GAME_CHECK': '1'}):
            self.assertNotEqual(boot_cache.fingerprint(self.game)[0], original)
        real_digest = boot_cache.digest
        for generator in boot_cache.GENERATORS:
            with self.subTest(generator=generator):
                def changed(path):
                    return '0' * 64 if Path(path).name == generator else real_digest(path)
                with mock.patch.object(boot_cache, 'digest', side_effect=changed):
                    self.assertNotEqual(boot_cache.fingerprint(self.game)[0], original)

    def test_cache_does_not_bypass_restored_game_validation(self):
        other = game_fixture(self.root / 'unsupported', 'CUSA00000')
        with mock.patch.dict(os.environ, {'BB_SKIP_GAME_CHECK': '1'}):
            self.ensure(other)
        with self.assertRaises(prepare.GameCheckError):
            self.ensure(other)

    def test_missing_corrupt_and_malformed_outputs_rebuild(self):
        target = self.ensure()
        for damage in ('missing', 'same-size', 'manifest'):
            with self.subTest(damage=damage):
                if damage == 'missing':
                    (target / 'eboot.elf').unlink()
                elif damage == 'same-size':
                    path = target / 'boot-linked.bin'
                    raw = path.read_bytes()
                    path.write_bytes(raw[:-1] + bytes([raw[-1] ^ 1]))
                else:
                    (target / 'manifest.json').write_text('[]')
                with mock.patch.object(boot_cache, 'build', wraps=boot_cache.build) as build:
                    self.assertEqual(self.ensure(), target)
                    self.assertEqual(build.call_count, 1)

    def test_failed_rebuild_never_falls_back_or_publishes_partial_results(self):
        previous = self.ensure()
        path = self.game / 'eboot.bin'
        path.write_bytes(path.read_bytes() + b'updated wrapper')
        with mock.patch.object(boot_cache, 'build', side_effect=OSError('disk full')):
            with self.assertRaisesRegex(OSError, 'disk full'):
                self.ensure()
        self.assertTrue(previous.is_dir())
        self.assertFalse(list(previous.parent.glob('.building-*')))
        self.assertNotEqual(self.ensure(), previous)

    def test_inputs_changing_during_build_are_not_published(self):
        real_build = boot_cache.build
        def changing(game, out):
            real_build(game, out)
            with (game / 'eboot.bin').open('ab') as stream:
                stream.write(b'changed during build')
        with mock.patch.object(boot_cache, 'build', side_effect=changing):
            with self.assertRaisesRegex(ValueError, 'changed during boot preparation'):
                self.ensure()
        self.assertFalse(list(self.out.rglob('manifest.json')))

    def test_publication_denied_then_recovers(self):
        with mock.patch.object(Path, 'rename', side_effect=PermissionError('file is locked')):
            with self.assertRaises(PermissionError):
                self.ensure()
        self.assertFalse(list(self.out.rglob('manifest.json')))
        self.assertTrue(self.ensure().is_dir())

    def test_prune_keeps_other_region_and_unmanaged_files(self):
        eu = self.ensure(game_fixture(self.root / 'EU', 'CUSA14876'))
        target = self.ensure()
        unmanaged = target.parent / 'notes'
        unmanaged.mkdir()
        (unmanaged / 'keep.txt').write_text('user file')
        for _ in range(3):
            with (self.game / 'eboot.bin').open('ab') as stream:
                stream.write(b'new dump wrapper')
            target = self.ensure()
        managed = [p for p in target.parent.iterdir() if boot_cache.KEY.fullmatch(p.name)]
        self.assertEqual(len(managed), 2)
        self.assertTrue(eu.is_dir())
        self.assertEqual((unmanaged / 'keep.txt').read_text(), 'user file')

    def test_damaged_archives_are_not_hidden_by_cache_hit(self):
        self.ensure()
        archives = self.game / 'archives'
        archives.mkdir()
        data = bytearray(0x48)
        data[:4] = b'IGA\x1a'
        struct.pack_into('<I', data, 0x28, 0x38)
        struct.pack_into('<I', data, 0x30, 16)
        (archives / 'bad.pak').write_bytes(data)
        with self.assertRaises(prepare.GameCheckError):
            self.ensure()


class LaunchLockTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def command(self, code, *args):
        return [sys.executable, '-c', 'import sys;sys.path.insert(0,sys.argv.pop(1));' + code,
                str(ROOT / 'scripts'), *map(str, args)]

    def test_second_process_is_rejected_then_lock_is_reusable(self):
        code = 'import launch_lock;\nwith launch_lock.session(sys.argv[1]): print("acquired")'
        with launch_lock.session(self.root):
            result = subprocess.run(self.command(code, self.root), capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Another game launch', result.stderr)
            with launch_lock.session(self.root):
                pass
        result = subprocess.run(self.command(code, self.root), capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_process_death_releases_lock(self):
        code = ('import launch_lock,time;\nwith launch_lock.session(sys.argv[1]):\n'
                ' print("locked",flush=True)\n time.sleep(30)')
        with subprocess.Popen(self.command(code, self.root), stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True) as child:
            try:
                self.assertEqual(child.stdout.readline().strip(), 'locked')
            finally:
                child.kill()
                child.communicate(timeout=10)
        with launch_lock.session(self.root):
            pass

    @unittest.skipIf(os.name == 'nt', 'POSIX run.sh supervisor')
    def test_supervisor_preserves_lock_through_restart(self):
        script = self.root / 'child.py'
        script.write_text('''import os,sys
from pathlib import Path
sys.path.insert(0,sys.argv[1])
import launch_lock
data=Path(sys.argv[2])
assert launch_lock.inherited(launch_lock.lock_path(data))
assert os.environ['BB_RESTART_EXIT_CODE']=='75'
assert 'BB_RESTART_COMMAND' not in os.environ
marker=data/'restarted'
if not marker.exists():
    marker.write_text('yes')
    sys.exit(75)
sys.exit(0)
''')
        result = subprocess.run([sys.executable, str(ROOT / 'scripts/launch_lock.py'), '--run', str(self.root),
                                 sys.executable, str(script), str(ROOT / 'scripts'), str(self.root)],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.root / 'restarted').exists())

    @unittest.skipIf(os.name == 'nt', 'POSIX run.sh supervisor')
    def test_stop_supervisor_releases_session_after_stopping_child(self):
        code = 'import time;print("ready",flush=True);time.sleep(30)'
        command = [sys.executable, str(ROOT / 'scripts/launch_lock.py'), '--run', str(self.root),
                   sys.executable, '-c', code]
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as process:
            try:
                self.assertEqual(process.stdout.readline().strip(), 'ready')
                process.terminate()
                process.communicate(timeout=10)
                self.assertEqual(process.returncode, 143)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=10)
        with launch_lock.session(self.root):
            pass


if __name__ == '__main__':
    unittest.main()
