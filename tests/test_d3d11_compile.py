"""tools/shaders/compile_d3d11.py: the post process compiled with the optimizer, older folders redone."""
import importlib.util
import os
import re
import tempfile
import unittest
from pathlib import Path

from paths import ROOT


def load_tool():
    spec = importlib.util.spec_from_file_location('compile_d3d11', ROOT / 'tools' / 'shaders' / 'compile_d3d11.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class VersionTests(unittest.TestCase):
    def test_run_py_knows_the_tool_version(self):
        tool = (ROOT / 'tools' / 'shaders' / 'compile_d3d11.py').read_text(encoding='utf-8')
        run = (ROOT / 'run.py').read_text(encoding='utf-8')
        self.assertEqual(re.search(r"^VERSION = '(\w+)'", tool, re.M)[1],
                         re.search(r"^D3D11_COMPILED_VERSION = '(\w+)'", run, re.M)[1])


@unittest.skipUnless(os.name == 'nt', 'd3dcompiler_47.dll is a Windows library')
class CompileTests(unittest.TestCase):
    def test_pixel_shaders_with_the_optimizer(self):
        tool = load_tool()
        self.assertEqual(tool.flags('ComposeScene_04fda25a_ps'), tool.OPTIMIZED_FLAGS)
        self.assertEqual(tool.flags('LutGenerator_63dcf8f7_ps'), tool.OPTIMIZED_FLAGS)
        self.assertEqual(tool.flags('Standard_3d9fd8d7_ps'), tool.OPTIMIZED_FLAGS)
        self.assertEqual(tool.flags('Standard_cc6438d2_vs'), tool.FLAGS)

    def test_an_older_folder_compiles_its_pixel_shaders_again(self):
        tool = load_tool()
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            for name in ('ComposeScene_1_ps', 'Standard_2_vs'):
                (folder / f'{name}.hlsl').write_text('float4 main() : SV_Target { return 1; }')
                (folder / f'{name}.dxbc').write_bytes(b'old')
            self.assertEqual(tool.forget_outdated(folder), 1)
            self.assertFalse((folder / 'ComposeScene_1_ps.dxbc').exists())
            self.assertTrue((folder / 'Standard_2_vs.dxbc').exists())
            tool.main(folder, 1)
            self.assertEqual((folder / 'compiled.txt').read_text().strip(), tool.VERSION)
            self.assertTrue((folder / 'ComposeScene_1_ps.dxbc').read_bytes().startswith(b'DXBC'))
            self.assertEqual(tool.forget_outdated(folder), 0)  # current now: nothing removed


class OrderTests(unittest.TestCase):
    def test_the_start_section_is_the_menus_and_names_real_shaders(self):
        order = ROOT / 'tools' / 'shaders' / 'd3d11_order.txt'
        lines = [l.strip() for l in order.read_text(encoding='utf-8').splitlines()]
        self.assertIn('[start]', lines)
        self.assertIn('[later]', lines)
        start = lines[lines.index('[start]') + 1:lines.index('[later]')]
        if os.name == 'nt':  # the tool loads d3dcompiler_47.dll
            self.assertEqual(start, load_tool().start_names(order))
        self.assertGreater(len(start), 50)
        for name in start:
            self.assertRegex(name, r'^[A-Za-z0-9]+_[0-9a-f]{8,16}_(vs|ps|es|gs)$')


if __name__ == '__main__':
    unittest.main()
