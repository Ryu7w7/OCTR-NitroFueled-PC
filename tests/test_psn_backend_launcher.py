"""Optional PSN selection must not claim a RyuNET identity or consume its invitations."""
import ast
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest import mock

from paths import ROOT

sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'launcher'))
import run
import discord_ipc
import ryunet_http


class SetupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.data = Path(self.temp.name)
        self.config = self.data / 'bbport.ini'
        self.library = self.data / 'backend.dll'
        self.library.write_bytes(b'not loaded by the launcher')
        self.config.write_text('online=1\nonline_name=RyuPC\nonline_server=127.0.0.1:8090\n'
                               'online_account_id=100\nonline_friends=OldFriend\n', encoding='utf-8')
        self.env = {'BB_PSN_BACKEND': str(self.library), 'BB_PSN_BACKEND_CONFIG': 'opaque=one two',
                    'BB_ONLINE_KEYHASH': 'previous', 'BB_ONLINE_KEY': 'previous', 'BB_ONLINE_TUNNEL': '1'}

    def setup_external(self):
        with mock.patch.multiple(run, online_key=mock.Mock(side_effect=AssertionError('key access')),
                                 claim_nick=mock.Mock(side_effect=AssertionError('nickname claim')),
                                 season_theme=mock.Mock(side_effect=AssertionError('theme query'))), \
                contextlib.redirect_stdout(io.StringIO()):
            run.setup_online(self.env, self.config, self.data)

    def test_external_identity_is_independent_of_publisher(self):
        before = self.config.read_bytes()
        self.setup_external()
        self.assertEqual(self.config.read_bytes(), before)
        self.assertEqual(self.env['BB_PSN_BACKEND_CONFIG'], 'opaque=one two')
        self.assertEqual(self.env['BB_ONLINE_SERVER'], '127.0.0.1')
        self.assertEqual(self.env['BB_ONLINE_TUNNEL'], '0')
        for name in ('NAME', 'ACCOUNT_ID', 'FRIENDS', 'KEY', 'KEYHASH'):
            self.assertEqual(self.env['BB_ONLINE_' + name], '')
        routing = json.loads((self.data / 'host_overrides.json').read_text())
        self.assertEqual(routing['lavender-ps4-auth3.prod.demonware.net'], 'http://127.0.0.1:8090')
        self.assertFalse((self.data / 'ryunet_online.key').exists())

    def test_no_implicit_publisher_or_overwrite_without_selection(self):
        self.config.write_text('online=1\n', encoding='utf-8')
        self.setup_external()
        self.assertNotIn('BB_ONLINE_SERVER', self.env)
        self.assertFalse((self.data / 'host_overrides.json').exists())
        existing = self.data / 'host_overrides.json'
        existing.write_text('{"custom": "127.0.0.1"}', encoding='utf-8')
        before = existing.read_bytes()
        self.setup_external()
        self.assertEqual(existing.read_bytes(), before)

    def test_invalid_explicit_backend_fails_without_legacy_fallback(self):
        for value in ('backend.dll', str(self.data / 'missing.dll'), str(self.data)):
            with self.subTest(value=value), contextlib.redirect_stderr(io.StringIO()):
                self.env['BB_PSN_BACKEND'] = value
                with self.assertRaises(SystemExit):
                    self.setup_external()
        self.assertFalse((self.data / 'host_overrides.json').exists())

    def test_empty_selector_keeps_legacy_nickname_validation(self):
        self.env['BB_PSN_BACKEND'] = ''
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            run.setup_online(self.env, self.config, self.data)

    def test_legacy_claim_theme_and_routing_unchanged(self):
        self.config.write_text('online=1\nonline_name=PlayerOne\nonline_server=127.0.0.1:8090\n', encoding='utf-8')
        env = {}
        with mock.patch.object(run, 'online_key', return_value='ab' * 32) as key, \
                mock.patch.object(run, 'claim_nick') as claim, \
                mock.patch.object(run, 'season_theme', return_value='winter') as theme, \
                contextlib.redirect_stdout(io.StringIO()):
            run.setup_online(env, self.config, self.data)
        key.assert_called_once_with(self.data)
        claim.assert_called_once_with('127.0.0.1', '8090', 'PlayerOne', env['BB_ONLINE_KEYHASH'])
        theme.assert_called_once_with('127.0.0.1', '8090')
        self.assertEqual(env['BB_ONLINE_NAME'], 'PlayerOne')
        self.assertEqual(env['BB_THEME'], 'winter')
        self.assertNotIn('BB_ONLINE_TUNNEL', env)

    def test_shell_export_clears_inherited_identity_and_preserves_config(self):
        output = self.data / 'environment'
        result = subprocess.run([sys.executable, str(ROOT / 'scripts' / 'online_config.py'),
                                 '--config', str(self.config), '--data', str(self.data), '--env-file', str(output)],
                                env={**os.environ, **self.env}, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = dict(record.decode('utf-8').split('=', 1) for record in output.read_bytes().split(b'\0') if record)
        self.assertEqual(entries['BB_ONLINE_KEYHASH'], '')
        self.assertEqual(entries['BB_ONLINE_ACCOUNT_ID'], '')
        self.assertEqual(entries['BB_ONLINE_TUNNEL'], '0')
        self.assertEqual(entries['BB_PSN_BACKEND_CONFIG'], 'opaque=one two')

    def test_shell_setup_error_does_not_continue_with_partial_settings(self):
        (self.data / 'host_overrides.json').mkdir()
        output = self.data / 'environment'
        result = subprocess.run([sys.executable, str(ROOT / 'scripts' / 'online_config.py'),
                                 '--config', str(self.config), '--data', str(self.data), '--env-file', str(output)],
                                env={**os.environ, **self.env}, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())


class DiscordIsolationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Execute the production helpers without importing GTK or opening a launcher window.
        source = ROOT / 'launcher' / 'sacredfire_launcher.py'
        tree = ast.parse(source.read_text(encoding='utf-8'))
        names = {'nick_ok', 'discord_activity_for', 'apply_join'}
        selected = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names]
        window = next(node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == 'LauncherWindow')
        methods = {'external_backend', 'on_discord_join', 'accept_join', 'poll_party', 'poll_announcement',
                   'start_discord_link', 'confirm_discord_unlink'}
        selected += [node for node in window.body if isinstance(node, ast.FunctionDef) and node.name in methods]
        cls.namespace = {'external_psn_backend': run.external_psn_backend, 'discord_ipc': discord_ipc,
                         'ryunet_http': ryunet_http, 'tr': lambda text: text, 'time': time,
                         'ONLINE_SERVER': 'octr.ryu7w7.xyz', 'RESERVED_NICKS': run.RESERVED_NICKS}
        exec(compile(ast.Module(body=selected, type_ignores=[]), str(source), 'exec'), cls.namespace)

    def test_external_presence_has_no_legacy_nickname_or_join_secret(self):
        ini = {'online': '1', 'online_name': 'OldNick', 'online_server': 'server.example'}
        activity = self.namespace['discord_activity_for'](ini, 100, (1, 8), True)
        self.assertFalse(activity.join_secret)
        self.assertFalse(activity.party_id)
        self.assertNotIn('OldNick', activity.state)
        legacy = self.namespace['discord_activity_for'](ini, 100, (1, 8), False)
        self.assertEqual(legacy.join_secret, 'ctr1|server.example|OldNick')

    def test_external_invite_does_not_read_or_mutate_profile(self):
        read = mock.Mock(side_effect=AssertionError('read profile'))
        with mock.patch.dict(self.namespace, {'load_ini': read}):
            self.assertIsNone(self.namespace['apply_join']('other.example', 'Host', True))
        read.assert_not_called()

    def test_gtk_callbacks_stop_before_profile_changes_or_legacy_queries(self):
        window = SimpleNamespace(external_backend=lambda: True)
        # These callbacks must return before touching any other window or profile state.
        self.namespace['on_discord_join'](window, 'ctr1|server.example|Host')
        self.namespace['accept_join'](window, 'server.example', 'Host')
        self.namespace['poll_party'](window)
        self.namespace['poll_announcement'](window)
        self.namespace['start_discord_link'](window, 'link')
        self.namespace['confirm_discord_unlink'](window)

    def test_gtk_routing_stays_with_running_session(self):
        window = SimpleNamespace(process=object(), running_external_backend=True,
                                 environment=lambda: {})
        self.assertTrue(self.namespace['external_backend'](window))
        window.running_external_backend = False
        window.environment = lambda: {'BB_PSN_BACKEND': '/future/backend.so'}
        self.assertFalse(self.namespace['external_backend'](window))
        window.process = None
        self.assertTrue(self.namespace['external_backend'](window))

    def test_legacy_invite_still_updates_friends_and_system_event_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            ini = Path(tmp) / 'bbport.ini'
            save = mock.Mock()
            with mock.patch.dict(self.namespace, {'load_ini': lambda: ({'online_friends': 'Friend'}, []),
                                                 'save_ini': save, 'ini_path': lambda: ini}):
                self.assertEqual(self.namespace['apply_join']('server.example', 'Host', False), 'Friend,Host')
            self.assertEqual(save.call_args.args[0]['online_server'], 'server.example')
            self.assertIn('session=cusa13795-session-Host\nhost=Host\n', (ini.parent / 'ryunet_join.txt').read_text())


if __name__ == '__main__':
    unittest.main()
