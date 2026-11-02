"""Synthetic package/network tests. No game assets, credentials or live lobby needed."""
from paths import ROOT
import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
import urllib.error
import zipfile

from mod_components import save_json, sha
from mod_session import launch_lock
import mod_library
from package_mod import build
from mod_sync.models import Offer, page_id, PAGE_FILE
from mod_sync.gamebanana import GameBanana
from mod_sync.packages import inspect_package, validate_package
from mod_sync.cache import Cache
from mod_sync.protocol import RyuNet, Snapshot
from mod_sync.service import Service, enabled_offers, start_for_runner
from mod_sync.transport import Cancelled, GameBananaRedirect, NoRedirect, Transport, gamebanana_url
from mod_sync import status

PAGE = 'https://gamebanana.com/mods/123456'


class FakeTransport:
    def __init__(self, content):
        self.content = content
        self.calls = []
        self.profile = {'_idRow': 123456, '_aGame': {'_idRow': 26138}, '_aFiles': [{
            '_idRow': 9999, '_sFile': 'fixture.zip', '_nFilesize': len(content),
            '_sDownloadUrl': 'https://gamebanana.com/dl/9999',
            '_sMd5Checksum': hashlib.md5(content).hexdigest(),
            '_sAvResult': 'clean', '_sAnalysisResult': 'ok'}]}

    def json(self, url, **kwargs):
        self.calls.append((url, kwargs))
        return copy.deepcopy(self.profile)

    def download(self, url, destination, expected, cancelled, progress=None):
        if cancelled.is_set():
            raise Cancelled()
        destination.write_bytes(self.content)
        if progress: progress(len(self.content), expected)
        return hashlib.sha256(self.content).hexdigest()


class SyncTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / 'source'
        self.source.mkdir()
        self.manifest = {'schemaVersion': 2, 'id': 'sync-fixture', 'name': 'Sync fixture',
            'version': '1.0.0', 'author': 'Test', 'description': 'Synthetic test only',
            'requiresCapabilities': [], 'compatibility': {'online': 'allowed'}, 'files': []}
        save_json(self.source / 'mod.json', self.manifest)
        (self.source / PAGE_FILE).write_text(PAGE + '\n')
        self.package = self.root / 'fixture.zip'
        build(self.source, self.package)
        self.transport = FakeTransport(self.package.read_bytes())
        self.api = GameBanana(self.transport)
        self.offer = self.api.pin(self.package)
        self.cache = Cache(self.root / 'cache', self.api)

    def stage(self):
        return self.cache.stage(self.offer, threading.Event())

    def test_pin_exact_zip_then_download_validate_and_disabled_import(self):
        target = self.stage()
        self.assertEqual(target.read_bytes(), self.package.read_bytes())
        data = self.root / 'portable'
        prefs = data / 'patches.json'
        save_json(prefs, {'enabled': [mod_library.PREFIX + 'sync-fixture', 'unrelated'], 'disabled': []})
        installed = self.cache.import_for_review(self.offer, data / 'mods', data / 'patches')
        self.assertEqual(installed.read_bytes(), self.package.read_bytes())
        self.assertEqual(json.loads(prefs.read_text())['enabled'], ['unrelated'])
        self.assertIn(mod_library.PREFIX + 'sync-fixture', json.loads(prefs.read_text())['disabled'])
        self.assertEqual(self.cache.import_for_review(self.offer, data / 'mods', data / 'patches'), installed)

    def test_no_implicit_latest_selection(self):
        self.transport.profile['_aFiles'][0]['_idRow'] = 10000
        self.transport.profile['_aFiles'][0]['_sDownloadUrl'] = 'https://gamebanana.com/dl/10000'
        with self.assertRaises(ValueError):
            self.api.resolve(self.offer)

    def test_same_sized_different_zip_rejected(self):
        self.transport.content = b'x' * self.offer.bytes
        with self.assertRaises(ValueError):
            self.stage()
        self.assertFalse(list(self.cache.root.glob('*.part')))
        self.assertFalse(list(self.cache.root.glob('*.zip')))

    def test_api_md5_checked_independently(self):
        self.transport.profile['_aFiles'][0]['_sMd5Checksum'] = 'a' * 32
        with self.assertRaises(ValueError):
            self.stage()

    def test_cancelled_download_is_not_installed(self):
        cancel = threading.Event()
        cancel.set()
        with self.assertRaises(Cancelled):
            self.cache.stage(self.offer, cancel)
        self.assertFalse(list(self.cache.root.glob('*.zip')))

    def test_quota_and_disk_space(self):
        with patch('mod_sync.cache.MAX_CACHE_BYTES', 1), self.assertRaises(ValueError):
            self.stage()
        with patch('mod_sync.cache.shutil.disk_usage') as space:
            space.return_value.free = 1
            with self.assertRaises(ValueError):
                self.stage()

    def test_cached_file_rehashed_before_use(self):
        target = self.stage()
        target.write_bytes(b'changed')
        self.assertEqual(self.stage().read_bytes(), self.package.read_bytes())

    def test_cached_download_does_not_bypass_withheld_status(self):
        self.stage()
        self.transport.profile['_bIsWithheld'] = True
        with self.assertRaises(ValueError): self.stage()

    def test_cache_modified_after_download_refuses_import(self):
        self.stage().write_bytes(b'changed')
        with self.assertRaises(ValueError):
            self.cache.import_for_review(self.offer, self.root / 'mods', self.root / 'patches')

    def test_active_game_session_refuses_import(self):
        self.stage()
        with launch_lock(self.root / 'mod-state'), self.assertRaises(ValueError):
            self.cache.import_for_review(self.offer, self.root / 'mods', self.root / 'patches')

    def test_active_vanilla_run_refuses_import_too(self):
        self.stage()
        with launch_lock(self.root / 'mod-state/sync-runtime'), self.assertRaises(ValueError):
            self.cache.import_for_review(self.offer, self.root / 'mods', self.root / 'patches')

    def test_different_existing_package_id_is_not_silently_upgraded(self):
        mods = self.root / 'mods'
        mods.mkdir()
        modified = dict(self.manifest, version='2.0.0')
        save_json(self.source / 'mod.json', modified)
        build(self.source, mods / 'newer.zip')
        self.stage()
        with self.assertRaises(ValueError):
            self.cache.import_for_review(self.offer, mods, self.root / 'patches')
        self.assertEqual([x.name for x in mods.iterdir()], ['newer.zip'])

    def test_unavailable_archived_scanning_or_wrong_game(self):
        for field, value in [('_bIsTrashed', True), ('_bIsWithheld', True), ('_aGame', {'_idRow': 1})]:
            with self.subTest(field=field):
                saved = copy.deepcopy(self.transport.profile)
                self.transport.profile[field] = value
                with self.assertRaises(ValueError): self.api.resolve(self.offer)
                self.transport.profile = saved
        for field, value in [('_bIsArchived', True), ('_sAvResult', 'pending'), ('_sAnalysisResult', 'failed')]:
            with self.subTest(field=field):
                saved = copy.deepcopy(self.transport.profile)
                self.transport.profile['_aFiles'][0][field] = value
                with self.assertRaises(ValueError): self.api.resolve(self.offer)
                self.transport.profile = saved

    def test_pin_repacked_or_ambiguous_zip_rejected(self):
        self.transport.profile['_aFiles'].append(copy.deepcopy(self.transport.profile['_aFiles'][0]))
        self.transport.profile['_aFiles'][1]['_idRow'] = 10000
        self.transport.profile['_aFiles'][1]['_sDownloadUrl'] = 'https://gamebanana.com/dl/10000'
        with self.assertRaises(ValueError): self.api.pin(self.package)

    def test_only_enabled_unchanged_packages_advertised(self):
        mods = self.root / 'mods'
        mods.mkdir()
        (mods / 'fixture.zip').write_bytes(self.package.read_bytes())
        mod_library.scan(mods, self.root / 'patches')
        selection = self.root / 'patches.json'
        save_json(selection, {'enabled': []})
        self.assertEqual(enabled_offers(mods, selection, self.api, lambda _: None), [])
        save_json(selection, {'enabled': [mod_library.PREFIX + 'sync-fixture']})
        self.assertEqual(enabled_offers(mods, selection, self.api, lambda _: None), [self.offer])
        (mods / 'fixture.zip').write_bytes(b'changed')
        self.assertEqual(enabled_offers(mods, selection, self.api, lambda _: None), [])

    def test_metadata_missing_duplicate_or_wrong_location(self):
        for entries in [{ 'a.txt': 'hello'}, {PAGE_FILE: PAGE, 'other/' + PAGE_FILE: PAGE}, {'nested/deep/' + PAGE_FILE: PAGE}]:
            with self.subTest(entries=entries):
                archive = self.root / 'bad.zip'
                with zipfile.ZipFile(archive, 'w') as out:
                    for name, value in entries.items(): out.writestr(name, value)
                with self.assertRaises(ValueError): inspect_package(archive)

    def test_paths_and_tools_are_rejected_before_extraction(self):
        for name in ['../escape.pak', '/absolute.pak', 'C:/escape.pak', 'a\\b.pak', 'CON.pak', 'run.exe', 'run.py', 'a/../b.pak']:
            with self.subTest(name=name):
                archive = self.root / 'bad.zip'
                with zipfile.ZipFile(archive, 'w') as out:
                    out.writestr(PAGE_FILE, PAGE)
                    entry = zipfile.ZipInfo('placeholder')
                    entry.filename = name  # retain hostile backslashes on Windows too
                    out.writestr(entry, b'fixture')
                with self.assertRaises(ValueError): inspect_package(archive)

    def test_offline_only_package_rejected(self):
        self.manifest['compatibility']['online'] = 'offline-only'
        save_json(self.source / 'mod.json', self.manifest)
        archive = self.root / 'offline.zip'
        build(self.source, archive)
        with self.assertRaises(ValueError): inspect_package(archive)

    def test_declared_checksum_failure(self):
        archive = self.root / 'tampered.zip'
        with zipfile.ZipFile(self.package) as src, zipfile.ZipFile(archive, 'w') as dest:
            for info in src.infolist():
                data = src.read(info)
                dest.writestr(info, data + b'\n' if info.filename == PAGE_FILE else data)
        with self.assertRaises(ValueError): validate_package(archive, self.root)

    def test_toggle_off_is_no_network_no_status_write(self):
        config = self.root / 'bbport.ini'
        config.write_text('online=1\nmod_sync=0\n')
        self.assertIsNone(start_for_runner(config, self.root, {'BB_ONLINE': '1'}, None, lambda _: None))
        self.assertFalse((self.root / 'mod-state').exists())

    def test_status_label_cannot_inject_fields(self):
        path = self.root / 'status.txt'
        status.publish(path, 'fallback', 'hello\nready\0', 1, 2, now=100)
        self.assertEqual(len(path.read_text().splitlines()), 6)

    def test_real_download_loop_checks_size_hash_and_byte_progress(self):
        class Reply(io.BytesIO):
            headers = {'Content-Length': '7'}
        class Opener:
            def open(self, request, **kwargs):
                self.request = request
                return Reply(b'payload')
        transport = Transport()
        transport.public = Opener()
        progress = []
        target = self.root / 'download.part'
        digest = transport.download('https://gamebanana.com/dl/9999', target, 7,
            threading.Event(), lambda done, total: progress.append((done, total)))
        self.assertEqual(digest, hashlib.sha256(b'payload').hexdigest())
        self.assertEqual(progress, [(7, 7)])
        self.assertFalse(transport.public.request.has_header('X-ryunet-proof'))
        with self.assertRaises(ValueError):
            transport.download('https://gamebanana.com/dl/9999', self.root / 'wrong.part', 6, threading.Event())

    def test_real_download_loop_rejects_missing_length_truncation_and_overrun(self):
        class Reply(io.BytesIO): headers = {}
        class Opener:
            def open(self, *args, **kwargs): return Reply(b'payload')
        transport = Transport()
        transport.public = Opener()
        for length in [6, 8]:
            with self.assertRaises(ValueError):
                transport.download('https://gamebanana.com/dl/9999', self.root / f'wrong-{length}.part', length, threading.Event())

    def test_real_json_reader_bounds_and_credential_separation(self):
        class Reply(io.BytesIO): headers = {'Content-Type': 'application/json'}
        class Opener:
            def open(self, *args, **kwargs): return Reply(b'{"valid":true}')
        transport = Transport()
        transport.public = Opener()
        self.assertEqual(transport.json(PAGE), {'valid': True})
        with self.assertRaises(ValueError): transport.json(PAGE, limit=2)
        with self.assertRaises(ValueError): transport.json(PAGE, headers={'X-RyuNET-Proof': 'secret'})

    def test_service_never_calls_cached_files_loaded_and_handles_lobby_entry(self):
        config = self.root / 'bbport.ini'
        config.write_text('mod_sync=1\n')
        offer = self.offer
        class Client:
            def __init__(self): self.reports = []
            def capabilities(self): return True
            def exchange(self, offers, report):
                self.reports.append(dict(report))
                if len(self.reports) == 1: return None
                if len(self.reports) > 6: config.write_text('mod_sync=0\n')
                return Snapshot('room', 'generation', (offer,), True)
        client = Client()
        service = Service(config, self.root / 'mods', self.root / 'patches.json', client,
                          cache=self.cache, poll_seconds=0.01, log=lambda _: None)
        with patch('mod_sync.service.enabled_offers', return_value=[]): service._loop()
        self.assertTrue((self.cache.root / offer.cache_name).exists())
        self.assertNotIn('ready', [r['phase'] for r in client.reports])
        self.assertIn('fallback', [r['phase'] for r in client.reports])
        self.assertTrue(all(not r['loaded'] for r in client.reports))

    def test_loaded_ack_is_separate_from_downloading(self):
        config = self.root / 'bbport.ini'
        config.write_text('mod_sync=1\n')
        offer = self.offer
        class Client:
            def __init__(self): self.reports = []
            def capabilities(self): return True
            def exchange(self, offers, report):
                self.reports.append(dict(report))
                if len(self.reports) > 2: config.write_text('mod_sync=0\n')
                return Snapshot('room', 'generation', (offer,), False)
        client = Client()
        service = Service(config, self.root / 'mods', self.root / 'patches.json', client,
                          cache=self.cache, loaded_provider=lambda snapshot: {offer.sha256},
                          poll_seconds=0.001, log=lambda _: None)
        with patch('mod_sync.service.enabled_offers', return_value=[]): service._loop()
        self.assertIn({'phase': 'ready', 'session': 'room', 'generation': 'generation', 'loaded': [offer.sha256]}, client.reports)
        self.assertFalse(self.cache.root.exists())

    def test_unsupported_server_does_not_download(self):
        class Client:
            def capabilities(self): return False
        service = Service(self.root / 'ini', self.root / 'mods', self.root / 'patches.json', Client(), cache=self.cache)
        service._loop()
        self.assertFalse(self.cache.root.exists())

    def test_network_failure_falls_back_without_propagating_to_runner(self):
        class Client:
            def capabilities(self): raise TimeoutError('fixture')
        service = Service(self.root / 'ini', self.root / 'mods', self.root / 'patches.json', Client(), cache=self.cache, log=lambda _: None)
        service._run()
        self.assertIn('\nfallback\n', service.status_path.read_text())


class WireTests(unittest.TestCase):
    def test_page_url_validation(self):
        self.assertEqual(page_id(PAGE), 123456)
        for bad in ['http://gamebanana.com/mods/1', PAGE + '?redirect=x', 'https://gamebanana.com.evil/mods/1', 'https://user@gamebanana.com/mods/1', PAGE + '#x', 'https://gamebanana.com/mods/0']:
            with self.subTest(bad=bad), self.assertRaises(ValueError): page_id(bad)

    def test_download_redirect_allowlist(self):
        for good in [PAGE, 'https://filecache53.gamebanana.com/mods/foo.zip']:
            gamebanana_url(good)
        for bad in ['http://gamebanana.com/dl/1', 'https://gamebanana.com.evil/a', 'https://127.0.0.1/a', 'https://gamebanana.com:444/a', 'https://x@gamebanana.com/a']:
            with self.subTest(bad=bad), self.assertRaises(ValueError): gamebanana_url(bad)

    def test_offer_fields_are_strict(self):
        good = {'page_url': PAGE, 'file_id': 1, 'bytes': 2, 'sha256': 'a' * 64}
        for bad in [dict(good, path='../evil'), dict(good, bytes=True), dict(good, file_id=False), dict(good, sha256='../x')]:
            with self.assertRaises(ValueError): Offer.from_dict(bad)

    def test_expired_snapshot_and_duplicate_offers_rejected(self):
        value = {'protocol': 2, 'session': 'room', 'generation': '1', 'expires_at': 110, 'offers': [], 'hold': False}
        self.assertFalse(Snapshot.parse(value, 100).hold)
        for expiry in [99, 100, 161, float('nan'), True]:
            with self.assertRaises(ValueError): Snapshot.parse(dict(value, expires_at=expiry), 100)
        offer = {'page_url': PAGE, 'file_id': 1, 'bytes': 2, 'sha256': 'a' * 64}
        with self.assertRaises(ValueError): Snapshot.parse(dict(value, offers=[offer, offer]), 100)

    def test_signed_body_digest_and_no_cleartext_fallback(self):
        class Fake:
            def json(self, url, **kwargs):
                self.call = (url, kwargs)
                return {'protocol': 2, 'session': 'room', 'generation': '1', 'expires_at': 110, 'offers': [], 'hold': False}
        transport = Fake()
        proofs = []
        client = RyuNet('https://server.example', 'player', lambda m, t: proofs.append((m, t)) or 'proof', transport)
        with patch('mod_sync.protocol.time.time', return_value=100):
            client.exchange([], {'phase': 'preparing'})
        url, kw = transport.call
        self.assertIn(hashlib.sha256(kw['body']).hexdigest(), url)
        self.assertTrue(kw['private'])
        self.assertEqual(kw['headers']['X-RyuNET-Proof'], 'proof')
        self.assertEqual(proofs[0][0], 'POST')
        with self.assertRaises(ValueError): RyuNet('http://server.example', 'player', None)

    def test_no_lobby_is_not_fatal(self):
        class Fake:
            def json(self, url, **kwargs):
                raise urllib.error.HTTPError(url, 409, 'no lobby', {}, None)
        self.assertIsNone(RyuNet('https://server.example', 'player', lambda *_: 'proof', Fake()).exchange([], {}))

    def test_existing_server_clock_correction_applies_to_expiry(self):
        class Fake:
            def json(self, url, **kwargs):
                return {'protocol': 2, 'session': 'room', 'generation': '1', 'expires_at': 1010, 'offers': [], 'hold': False}
        client = RyuNet('https://server.example', 'player', lambda *_: 'proof', Fake(), clock_offset=900)
        with patch('mod_sync.protocol.time.time', return_value=100):
            self.assertEqual(client.exchange([], {}).session, 'room')

    def test_private_http_redirect_does_not_forward_proof(self):
        self.assertIsNone(NoRedirect().redirect_request(None, None, 302, '', {}, 'https://evil.example'))
        with self.assertRaises(ValueError):
            GameBananaRedirect().redirect_request(None, None, 302, '', {}, 'https://evil.example')


if __name__ == '__main__': unittest.main()
