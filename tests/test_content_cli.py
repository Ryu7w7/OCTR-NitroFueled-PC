"""content_cli.py: what the launcher's "External content" section reads and does."""
import json
import shutil
import tempfile
import unittest
from pathlib import Path

import paths  # noqa: F401
import content_cli
import content_package as cp
from test_content_packages import ID, write_package, zip_folder


class CliTests(unittest.TestCase):
    def setUp(self):
        self.port = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.port, ignore_errors=True)
        self.root = self.port / cp.FOLDER
        self.root.mkdir()

    def test_the_listing_has_the_choices_packages_and_problems(self):
        write_package(self.port / 'work', manifest={'credits': 'Suzuka   by  CTRNFModding'})  # spaces folded
        zip_folder(self.port / 'work', self.root / 'suzuka.zip')
        write_package(self.root / 'nbc', ident='t929_neo_bowser_city', image=False, manifest={'online': 'private'})
        (self.root / 'broken.zip').write_bytes(b'not a zip')
        # The launcher writes the selection itself, in the same form.
        (cp.state(self.root)).mkdir(parents=True, exist_ok=True)
        (cp.state(self.root) / 'selection.json').write_text(
            json.dumps({'enabled': False, 'disabled': ['t929_neo_bowser_city'], 'page': 'bonus'}))
        lines = [line.split('\t') for line in content_cli.listing(self.port)]
        self.assertEqual(lines[0], ['selection', '0', 'bonus'])
        self.assertEqual(lines[1], ['package', ID, '1', 'track', 'Suzuka', '0.7.2', 'CTRNFModding', 'offline-only',
                                    'suzuka.zip', f'octane/{ID}/{ID}', 'Suzuka by CTRNFModding'])
        self.assertEqual(lines[2][:3], ['package', 't929_neo_bowser_city', '0'])
        self.assertEqual(lines[2][7], 'private')
        self.assertEqual([line[0] for line in lines[3:]], ['message'])

    def test_remove_moves_the_package_aside(self):
        write_package(self.root / 'suzuka')
        moved = content_cli.remove(self.port, ID)
        self.assertFalse((self.root / 'suzuka').exists())
        self.assertTrue((moved / cp.MANIFEST).is_file())
        self.assertEqual(content_cli.listing(self.port), ['selection\t1\town'])
        with self.assertRaisesRegex(ValueError, 'no package'):
            content_cli.remove(self.port, ID)

    def test_bad_commands_print_the_usage(self):
        self.assertEqual(content_cli.main([str(self.port), 'nothing']), 2)
        self.assertEqual(content_cli.main([str(self.port), 'remove', 'x']), 1)


if __name__ == '__main__':
    unittest.main()
