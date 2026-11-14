"""scripts/online_key_local.py: the data-folder patch that trusts the local server's key."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tools'))
from online_key_local import MARKER, local_xml, main  # noqa: E402
from make_online_key_patch import extract_modulus  # noqa: E402
from patches import all_external_patches  # noqa: E402

BUNDLED = ROOT / 'patches' / 'CrashTeamRacingOnline.xml'


class OnlineKeyLocalTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # A throwaway RSA pair stands in for the local server's ctr_ps4.key.
        cls.tmp = tempfile.TemporaryDirectory()
        cls.key = Path(cls.tmp.name) / 'ctr_ps4.key'
        subprocess.run(['openssl', 'genrsa', '-out', str(cls.key), '2048'],
                       check=True, capture_output=True)
        cls.expected = extract_modulus(Path(cls.tmp.name) / (cls.key.stem + '.pem')) if False else None

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_value_is_the_local_key(self):
        body = local_xml(BUNDLED, self.key).decode()
        pub = Path(self.tmp.name) / 'pub.pem'
        pub.write_text(subprocess.run(['openssl', 'rsa', '-in', str(self.key), '-pubout'],
                                      check=True, capture_output=True, text=True).stdout)
        want = extract_modulus(pub).hex().upper()
        self.assertIn(want, body)
        self.assertIn('0x1fecfa1', body)                    # the bundled write address, kept
        self.assertIn('custom Demonware key', body)         # the patch Name, kept (selection depends on it)
        self.assertNotIn(BUNDLED.read_text().split('Value="')[1][:32], body)  # not the production modulus
        self.assertIn(MARKER, body)

    def test_replace_rule_picks_the_data_folder_file(self):
        data = Path(self.tmp.name) / 'data'
        (data / 'patches').mkdir(parents=True)
        (data / 'patches' / 'CrashTeamRacingOnline.xml').write_bytes(local_xml(BUNDLED, self.key))
        found = all_external_patches([ROOT / 'patches', data / 'patches'])
        ours = [(key, path) for key, path, _ in found if path.name == 'CrashTeamRacingOnline.xml']
        self.assertEqual(len(ours), 1)
        self.assertEqual(ours[0][1].parent, data / 'patches')  # the later folder wins
        self.assertIn('custom Demonware key', ours[0][0])      # the selection key is unchanged

    def test_never_overwrites_a_foreign_file(self):
        data = Path(self.tmp.name) / 'data2'
        (data / 'patches').mkdir(parents=True)
        target = data / 'patches' / 'CrashTeamRacingOnline.xml'
        target.write_text('<?xml version="1.0"?><Patch></Patch>')
        with self.assertRaises(SystemExit):
            main_with(data, self.key)
        self.assertEqual(target.read_text(), '<?xml version="1.0"?><Patch></Patch>')

    def test_idempotent_over_our_own_file(self):
        data = Path(self.tmp.name) / 'data3'
        (data / 'patches').mkdir(parents=True)
        main_with(data, self.key)
        first = (data / 'patches' / 'CrashTeamRacingOnline.xml').read_bytes()
        main_with(data, self.key)
        self.assertEqual(first, (data / 'patches' / 'CrashTeamRacingOnline.xml').read_bytes())


def main_with(data: Path, key: Path) -> None:
    old = sys.argv
    try:
        sys.argv = ['online_key_local.py', '--data', str(data), '--key', str(key)]
        main()
    finally:
        sys.argv = old


if __name__ == '__main__':
    unittest.main()
