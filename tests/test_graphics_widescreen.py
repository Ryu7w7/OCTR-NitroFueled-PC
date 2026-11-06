"""Generated menus must preserve widescreen assets when loaded after native archives."""
from paths import ROOT
import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest

import graphics_tab
import nitro_format
from nitro_components import payload_bytes
import widescreen


def entries(path, only=None):
    archive = graphics_tab.Archive(path)
    with archive.f:
        return {archive.logical(i).casefold(): archive.read(i) for i in range(len(archive.names))
                if only is None or archive.logical(i).casefold() == only}


class GraphicsWidescreenTests(unittest.TestCase):
    def test_disabled_graphics_does_not_create_an_overlay(self):
        with tempfile.TemporaryDirectory() as temp:
            self.assertEqual(widescreen.graphics_overlay(None, temp, {}, None, []), {})
            self.assertEqual(list(Path(temp).iterdir()), [])

    @unittest.skipUnless(os.environ.get('BB_TEST_GAME'), 'requires local USA 1.21 game dump')
    def test_generated_menu_composition_and_layout_cache_switching(self):
        game = Path(os.environ['BB_TEST_GAME'])
        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
            out = Path(temp)
            original = out / 'update3-original.pak'
            graphics_tab.build(game, original, out / 'dump')
            native = entries(original)
            env = {'BB_GRAPHICS_TAB_ARCHIVE': str(original)}
            targets = []
            for layout, output in (('32x9', '5120x1440'), ('21x9', '3440x1440'), ('21x9', '2560x1080')):
                folder = widescreen.DATA / ('ultrawide-' + layout)
                manifest = json.loads((folder / 'nitro-mod.json').read_text(encoding='utf-8'))
                _, flat = nitro_format.resolve(manifest, {'render_res': '1440', 'output_res': output})
                operations = [op for op in flat['operations'] if op['kind'] == 'archive_asset']
                result = widescreen.graphics_overlay(game, out, env, folder, operations)
                target = Path(result['BB_GRAPHICS_TAB_ARCHIVE'])
                composed = entries(target)
                targets.append(target)
                self.assertEqual(set(native), set(composed))
                changed = set()
                for op in operations:
                    logical = op['asset'].casefold()
                    if op['archive'] != 'update2.pak' or logical not in composed:
                        continue
                    raw = entries(game / 'archives' / op['archive'], logical)[logical]
                    transform = graphics_tab.CHANGES.get(logical, graphics_tab.OPTIONAL.get(logical))
                    if transform:
                        self.assertEqual(composed[logical], transform(payload_bytes(op, folder, raw)))
                        changed.add(logical)
                self.assertIn('ui/octane_mainmenu.igz', changed)
                self.assertNotEqual(native['ui/octane_mainmenu.igz'], composed['ui/octane_mainmenu.igz'])
                # All entries without a layout correction retain their exact original bytes.
                relevant = {op['asset'].casefold() for op in operations}
                for logical in set(native) - relevant:
                    self.assertEqual(native[logical], composed[logical])
                stamp = target.stat().st_mtime_ns
                self.assertEqual(widescreen.graphics_overlay(game, out, env, folder, operations), result)
                self.assertEqual(target.stat().st_mtime_ns, stamp)
            # Identical corrected assets may safely share a cache across outputs;
            # every selection above was checked against its own resolved manifest.
            self.assertTrue(targets)
            # Environment overrides belong to a layout plan, not the caller's baseline.
            self.assertEqual(env, {'BB_GRAPHICS_TAB_ARCHIVE': str(original)})
            self.assertEqual(entries(original), native)


if __name__ == '__main__':
    unittest.main()
