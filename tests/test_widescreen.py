"""Built-in widescreen selection and projection conflict regression checks."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import widescreen
import nitro_format as fmt
from nitro_components import copy_delta
from patches import render_patches


class WidescreenTests(unittest.TestCase):
    def test_resized_asset_reuses_original_bytes(self):
        delta = {'format': 'copy-delta-v1', 'source_bytes': 6, 'result_bytes': 8,
                 'operations': [['copy', 0, 3], ['bytes', '7879'], ['copy', 3, 3]]}
        self.assertEqual(copy_delta(b'abcdef', delta), b'abcxydef')

    def test_copy_delta_rejects_bad_ranges_and_sizes(self):
        for operations, result_size in (([['copy', 4, 3]], 3),
                                        ([['copy', -1, 3]], 3),
                                        ([['copy', 0, 6]], 5),
                                        ([['copy', 0, 4]], 5),
                                        ([['unknown', 0, 4]], 4)):
            with self.subTest(operations=operations, result_size=result_size):
                with self.assertRaises(ValueError):
                    copy_delta(b'abcdef', {'format': 'copy-delta-v1', 'source_bytes': 6,
                               'result_bytes': result_size, 'operations': operations})

    def test_disabled_does_not_select_assets(self):
        self.assertIsNone(widescreen.selection({'ultrawide': '0', 'ultrawide_size': '5120x1440'}))

    def test_disabled_path_preserves_environment_and_files(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ini = root / 'bbport.ini'
            ini.write_text('ultrawide=0\n')
            env = {'BB_USER_DIR': 'original-user'}
            before = sorted(root.iterdir())
            game, config = widescreen.prepare(root, root, env, root / 'game', ini)
            self.assertEqual(game, root / 'game')
            self.assertEqual(config, str(ini))
            self.assertEqual(env, {'BB_USER_DIR': 'original-user'})
            self.assertEqual(before, sorted(root.iterdir()))

    def test_upstream_settings_select_matching_corrected_profile(self):
        for layout, sizes in widescreen.OUTPUTS.items():
            for size in sizes:
                with self.subTest(size=size):
                    selected, settings = widescreen.selection({'ultrawide': '1', 'ultrawide_size': size})
                    self.assertEqual(selected, layout)
                    self.assertEqual(settings['output_res'], size)

    def test_layout_and_size_cannot_disagree(self):
        with self.assertRaises(ValueError):
            widescreen.selection({'ultrawide': '1', 'widescreen_layout': '21x9', 'ultrawide_size': '5120x1440'})

    def test_unknown_aspect_rejected(self):
        with self.assertRaises(ValueError):
            widescreen.selection({'ultrawide': '1', 'ultrawide_size': '3840x1600'})

    def test_all_output_internal_combinations_resolve(self):
        for layout, sizes in widescreen.OUTPUTS.items():
            manifest = fmt.validate_folder(widescreen.DATA / ('ultrawide-' + layout))
            for size in sizes:
                for internal in ('480', '720', '1080', '1440', '1620', '2160'):
                    with self.subTest(layout=layout, size=size, internal=internal):
                        _, settings = widescreen.selection(dict(ultrawide='1', ultrawide_size=size, render_res=internal))
                        variant, flat = fmt.resolve(manifest, settings)
                        self.assertTrue(variant)
                        self.assertEqual(sum(o['kind'] == 'archive_asset' for o in flat['operations']), 224)
                        self.assertEqual(sum(o['kind'] == 'shader_position_scale' for o in flat['operations']), 2)
                        effective = dict(settings, **flat['settings'])
                        effective['ultrawide'] = '0'
                        self.assertEqual(render_patches(effective), [])

    def test_unsupported_internal_resolution_rejected(self):
        with self.assertRaises(ValueError):
            widescreen.selection(dict(ultrawide='1', ultrawide_size='3440x1440', render_res='900'))

    def test_in_game_render_change_requires_full_relaunch(self):
        import tempfile
        previous = widescreen.FROZEN_RENDER
        try:
            widescreen.FROZEN_RENDER = {'render_res': '1080', 'ultrawide': '0'}
            with tempfile.TemporaryDirectory() as tmp:
                config = Path(tmp) / 'bbport.ini'
                config.write_text('render_res=1440\nultrawide=0\n')
                with self.assertRaisesRegex(ValueError, 'select.*launcher'):
                    widescreen.validate_restart(config)
                config.write_text('render_res=1080\nultrawide=0\nfps_limit=90\n')
                widescreen.validate_restart(config)
        finally:
            widescreen.FROZEN_RENDER = previous


if __name__ == '__main__':
    unittest.main()
