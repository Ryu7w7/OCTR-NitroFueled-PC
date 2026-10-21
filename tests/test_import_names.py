"""SacredFire: tools/gen_import_names.py (import names for the prepared eboot)."""
from paths import ROOT
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(ROOT / 'tools'))
import gen_import_names


class ImportNameTests(unittest.TestCase):
    def test_lines_name_known_nids_and_mark_unknown(self):
        names = {'bzQExy189ZI': '_init_env', 'g8cM39EUZ6o': 'sceSysmoduleLoadModule'}
        lines = gen_import_names.generate(['bzQExy189ZI#libc', 'g8cM39EUZ6o#libSceSysmodule', 'cmvr0mOCX2Q#libSceNpToolkit2', 'plain'], names)
        self.assertEqual(lines, ['bzQExy189ZI libc _init_env', 'g8cM39EUZ6o libSceSysmodule sceSysmoduleLoadModule',
                                 'cmvr0mOCX2Q libSceNpToolkit2 ?', 'plain - ?'])

    def test_aerolib_parsing(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'aerolib.inl'
            path.write_text('// header\nSTUB("bzQExy189ZI", _init_env)\nSTUB("++-URPJNZ18", _ZN9InspectorE)\n')
            self.assertEqual(gen_import_names.aerolib_names(path), {'bzQExy189ZI': '_init_env', '++-URPJNZ18': '_ZN9InspectorE'})


if __name__ == '__main__':
    unittest.main()
