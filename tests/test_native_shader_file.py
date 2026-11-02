"""Native shader loading preserves words and rejects damaged files before Vulkan."""
from paths import ROOT
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

COMPILER = shutil.which('c++')


@unittest.skipUnless(COMPILER, 'requires a C++ compiler')
class NativeShaderFileTests(unittest.TestCase):
    def test_file_loading(self):
        with tempfile.TemporaryDirectory(prefix='native-shader-test-') as tmp:
            exe = Path(tmp) / 'test'
            subprocess.run([
                COMPILER, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-UNDEBUG',
                '-I' + str(ROOT / 'gpu/shim'),
                str(ROOT / 'tests/test_native_shader_file.cpp'),
                str(ROOT / 'gpu/shim/bbport_native_vk_spirv.cpp'), '-o', str(exe),
            ], check=True, capture_output=True, text=True)
            subprocess.run([str(exe), str(Path(tmp) / 'shader.spv')], check=True)


if __name__ == '__main__':
    unittest.main()
