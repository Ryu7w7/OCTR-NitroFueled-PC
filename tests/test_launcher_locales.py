"""The Linux launcher's translations (launcher/locales/*.json): every file is valid JSON, every key
is a text the launcher translates, and each translation keeps its {} placeholders and line breaks."""
from paths import ROOT
import ast
import json
import unittest

LAUNCHER = ROOT / 'launcher' / 'sacredfire_launcher.py'
LOCALES = ROOT / 'launcher' / 'locales'
# Choice lists whose labels the launcher passes through tr() (combo_row and the bindings pages).
LISTS = {'FPS_LIMITS', 'RENDER_RESOLUTIONS', 'WIDESCREEN_LAYOUTS', 'PRESENT_MODES', 'LANGUAGES', 'LANGUAGES_EU', 'UPSCALERS', 'INVENTORIES',
         'KEY_INPUTS', 'PAD_INPUTS'}


def translated_texts():
    tree = ast.parse(LAUNCHER.read_text(encoding='utf-8'))
    found = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and getattr(node.func, 'id', None) == 'tr' and node.args:
            arg = node.args[0]
            if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
                found.add(arg.value)
    for node in tree.body:
        if isinstance(node, ast.Assign) and getattr(node.targets[0], 'id', '') in LISTS:
            index = 1 if node.targets[0].id in ('KEY_INPUTS', 'PAD_INPUTS') else 0
            for element in node.value.elts:
                item = element.elts[index]
                if isinstance(item, ast.Constant):
                    found.add(item.value)
    return found


class LocaleTests(unittest.TestCase):
    def test_locales(self):
        texts = translated_texts()
        files = sorted(LOCALES.glob('*.json'))
        self.assertGreaterEqual(len(files), 8)
        for path in files:
            table = json.loads(path.read_text(encoding='utf-8'))
            for key, value in table.items():
                with self.subTest(locale=path.stem, text=key[:40]):
                    self.assertIn(key, texts, 'not a text the launcher translates')
                    self.assertEqual(key.count('{}'), value.count('{}'), 'placeholders differ')
                    self.assertEqual(key.count('\n'), value.count('\n'), 'line breaks differ')

    def test_languages_offered(self):
        import sys
        sys.path.insert(0, str(ROOT / 'launcher'))
        import launcher_i18n
        codes = {code for _label, code in launcher_i18n.UI_LANGUAGES if code}
        self.assertEqual(codes - {'en'}, {p.stem for p in LOCALES.glob('*.json')})
        launcher_i18n.set_language('es')
        self.assertEqual(launcher_i18n.tr('Play'), 'Jugar')
        self.assertEqual(launcher_i18n.tr('Español'), 'Español')
        self.assertEqual(launcher_i18n.menu_language(), 'en')
        launcher_i18n.set_language('ru')
        self.assertEqual(launcher_i18n.menu_language(), 'ru')
        launcher_i18n.set_language('en')


if __name__ == '__main__':
    unittest.main()
