"""scripts/online_key_local.py: the data-folder patch that trusts the local server's key."""
import contextlib
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tools'))
from online_key_local import MARKER, extract_modulus_text, local_xml, main  # noqa: E402
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

    def test_restore_removes_only_our_own_patches(self):
        data = Path(self.tmp.name) / 'data4'
        patches = data / 'patches'
        patches.mkdir(parents=True)
        main_with(data, self.key)
        player = patches / 'CrashTeamRacingUnlockAll.xml'   # the player's own file, not ours
        player.write_text('<Patch/>')
        main_with(data, self.key, restore=True)
        self.assertEqual([p.name for p in patches.iterdir()], [player.name])

    def test_stored_key_for_the_server_is_used(self):
        data = Path(self.tmp.name) / 'data5'
        keys = data / 'ryunet_keys'
        keys.mkdir(parents=True)
        (keys / '127.0.0.1.pub').write_text(public_pem(self.key))
        main_with(data, server='127.0.0.1')
        body = (data / 'patches' / 'CrashTeamRacingOnline.xml').read_text()
        self.assertIn(extract_modulus_text(public_pem(self.key)).hex().upper(), body)

    def test_other_server_without_a_key_warns_and_restores(self):
        data = Path(self.tmp.name) / 'data6'
        (data / 'patches').mkdir(parents=True)
        main_with(data, self.key)                    # a local-key patch left behind
        with contextlib.redirect_stderr(io.StringIO()) as err:
            main_with(data, server='127.0.0.1')      # no key for it
        self.assertIn('no key for 127.0.0.1', err.getvalue())
        self.assertFalse((data / 'patches' / 'CrashTeamRacingOnline.xml').exists())

    def test_production_server_without_a_key_is_silent(self):
        data = Path(self.tmp.name) / 'data7'
        (data / 'patches').mkdir(parents=True)
        main_with(data, self.key)
        main_with(data, server='octr.ryu7w7.xyz')
        self.assertFalse((data / 'patches' / 'CrashTeamRacingOnline.xml').exists())


def public_pem(key: Path) -> str:
    return subprocess.run(['openssl', 'rsa', '-in', str(key), '-pubout'],
                          check=True, capture_output=True, text=True).stdout


def main_with(data: Path, key: Path | None = None, server: str | None = None,
              restore: bool = False) -> None:
    old = sys.argv
    try:
        sys.argv = ['online_key_local.py', '--data', str(data)]
        if key:
            sys.argv += ['--key', str(key)]
        if server:
            sys.argv += ['--server', server]
        if restore:
            sys.argv.append('--restore')
        main()
    finally:
        sys.argv = old


if __name__ == '__main__':
    unittest.main()
