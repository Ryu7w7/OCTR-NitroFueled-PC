"""The public selector must not inherit a stale or unsupported matchmaking capability."""
from paths import ROOT
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
import run

class PublicItemlessLaunch(unittest.TestCase):
    def test_supported_version_only_and_offline_clear(self):
        for online, announcement, expected in (
                ('1', {'itemless_matchmaking': 1}, '1'),
                ('1', {'itemless_matchmaking': 2}, '0'),
                ('1', {}, '0'), ('0', {'itemless_matchmaking': 1}, '0')):
            with self.subTest(online=online, announcement=announcement), tempfile.TemporaryDirectory() as tmp:
                data=Path(tmp)
                env={'BB_ONLINE':online,'BB_ONLINE_NAME':'LocalHost','BB_ONLINE_SERVER':'127.0.0.1',
                     'BB_PUBLIC_ITEMLESS':'1'}
                with patch.object(run,'server_settings',return_value=announcement), \
                     patch.object(run,'online_key',return_value=None), patch.object(run,'say'):
                    run.setup_online(env,data/'bbport.ini',data)
                self.assertEqual(env['BB_PUBLIC_ITEMLESS'],expected)
