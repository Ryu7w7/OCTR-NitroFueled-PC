"""scripts/dev.py: the data folder and the local server it starts for a dev run."""
import os
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import dev  # noqa: E402


class DevTest(unittest.TestCase):
    def test_data_folder_is_run_shs(self):
        old = os.environ.pop('BB_DATA_DIR', None)
        try:
            self.assertEqual(dev.data_dir(), ROOT)                 # "." of run.sh
            os.environ['BB_DATA_DIR'] = 'out/dev-data'
            self.assertEqual(dev.data_dir(), ROOT / 'out' / 'dev-data')  # relative to the checkout
            os.environ['BB_DATA_DIR'] = str(ROOT / 'user')
            self.assertEqual(dev.data_dir(), ROOT / 'user')        # absolute kept
        finally:
            os.environ.pop('BB_DATA_DIR', None)
            if old is not None:
                os.environ['BB_DATA_DIR'] = old

    def test_server_binary_per_platform(self):
        name = 'server.exe' if os.name == 'nt' else 'server'
        self.assertEqual(dev.server_binary(Path('/r')), Path('/r') / 'bin' / name)

    def test_nothing_listens_on_a_closed_port(self):
        self.assertFalse(dev.listening('127.0.0.1', 1))

    def test_no_server_and_no_checkout_is_an_error(self):
        env = dict(os.environ)
        for name in ('BB_NO_SERVER', 'RYUNET_DIR', 'BB_ONLINE_HTTP_PORT'):
            env.pop(name, None)
        env['BB_ONLINE_HTTP_PORT'] = '1'      # nothing listens there
        with mock.patch.dict(os.environ, env, clear=True):
            self.assertEqual(dev.main(), 1)


if __name__ == '__main__':
    unittest.main()
