"""Runtime restart exit without a game."""
import ast
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from paths import ROOT


class RestartTests(unittest.TestCase):
    def test_renderer_environment_is_rebuilt_after_settings_change(self):
        tree = ast.parse((ROOT / 'run.py').read_text(encoding='utf-8'))
        renderer = next(n for n in ast.walk(tree) if isinstance(n, ast.FunctionDef) and n.name == 'apply_renderer')
        sys.path.insert(0, str(ROOT / 'scripts'))
        import native_shaders
        settings = {'renderer': 'native', 'temporal': 'fsr3'}
        env = {'USER_OVERRIDE': 'kept'}
        scope = dict(env=env, renderer_changes={}, ini_value=lambda _, key: settings.get(key),
                     config=None, title_id='CUSA13795', app_version='01.21', data=Path('/data'), game=Path('/game'),
                     native_shaders=native_shaders, say=lambda _: None, out=Path('/out'))
        exec(compile(ast.Module(body=[renderer], type_ignores=[]), 'run', 'exec'), scope)
        with mock.patch.object(native_shaders, 'ready', return_value=True):
            scope['apply_renderer']()
            self.assertEqual(env['BB_TEMPORAL'], 'fsr1')
            settings['temporal'] = 'dlss'
            scope['apply_renderer']()
            self.assertNotIn('BB_TEMPORAL', env)
            self.assertEqual(env['BB_NATIVE_VK'], '1')
            settings['renderer'] = 'shadps4'
            scope['apply_renderer']()
            self.assertNotIn('BB_NATIVE_VK', env)
            self.assertNotIn('BB_VK_SHADERS', env)
            self.assertEqual(env, {'USER_OVERRIDE': 'kept'})
            env['BB_NATIVE_VK'] = '0'
            settings['renderer'] = 'native'
            scope['apply_renderer']()
            self.assertEqual(env['BB_NATIVE_VK'], '0')




    @unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc'), 'requires Linux C compiler')
    def test_runtime_restart_exits_to_supervisor(self):
        source = (ROOT / 'src/probe.c').read_text(encoding='utf-8')
        start = source.index('volatile int runtime_restarting;')
        end = source.index('\n}', source.index('void runtime_restart(void)', start)) + 2
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            c = root / 'restart.c'
            c.write_text('#define _GNU_SOURCE\n#include <stdio.h>\n#include <stdlib.h>\n'
                         '#include <unistd.h>\n#include <sys/syscall.h>\n' + source[start:end] +
                         '\nint main(void) { runtime_restart(); return 99; }\n')
            binary = root / 'restart'
            subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], cwd=root, env={**os.environ, 'BB_RESTART_EXIT_CODE': '75'},
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 75, result.stderr)

    @unittest.skipUnless(shutil.which('wine') and shutil.which('x86_64-w64-mingw32-g++'), 'requires Wine and MinGW')
    def test_windows_runtime_restart_exits_to_supervisor(self):
        source = (ROOT / 'src/probe.c').read_text(encoding='utf-8')
        start = source.index('volatile int runtime_restarting;')
        end = source.index('\n}', source.index('void runtime_restart(void)', start)) + 2
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            c = root / 'runtime.c'
            c.write_text('#include <stdio.h>\n#include <stdlib.h>\n#include <process.h>\n#include <windows.h>\n' +
                         source[start:end] + '\nint main(void) { runtime_restart(); return 99; }\n')
            binary = root / 'runtime.exe'
            subprocess.run(['x86_64-w64-mingw32-gcc', '-std=c11', str(c), '-o', str(binary)],
                           check=True, capture_output=True)
            result = subprocess.run(['wine', str(binary)],
                                    env={**os.environ, 'WINEDEBUG': '-all', 'BB_RESTART_EXIT_CODE': '75'},
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 75, result.stderr)

    @unittest.skipUnless(shutil.which('wine') and shutil.which('x86_64-w64-mingw32-g++'), 'requires Wine and MinGW')
    def test_windows_update_reopens_packaged_launcher(self):
        source = (ROOT / 'launcher/windows/platform.inc').read_text(encoding='utf-8')
        start = source.index('bool RestartLauncher(const std::wstring& args) {')
        end = source.index('\n}', start) + 2
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            cpp = root / 'restart.cpp'
            cpp.write_text('''#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
namespace fs = std::filesystem;
struct App { fs::path root; } g_app;
const wchar_t* kLauncherName = L"Crash™ Team Racing Nitro-Fueled";
std::wstring Quote(const std::wstring& s) { return L"\\\"" + s + L"\\\""; }
''' + source[start:end] + '''
int wmain(int argc, wchar_t** argv) {
    wchar_t self[4096]{};
    GetModuleFileNameW(nullptr, self, 4096);
    g_app.root = fs::path(self).parent_path();
    if (argc == 2 && std::wstring(argv[1]) == L"--updated") {
        std::ofstream(g_app.root / L"reopened.txt") << "updated";
        return 0;
    }
    fs::copy_file(self, g_app.root / (std::wstring(kLauncherName) + L".exe"));
    if (!RestartLauncher(L"--updated")) return 1;
    MSG message{};
    if (!PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) return 2;
    for (int i = 0; i < 100; ++i) {
        if (fs::exists(g_app.root / L"reopened.txt")) return 0;
        Sleep(50);
    }
    return 3;
}
''')
            binary = root / 'restart.exe'
            subprocess.run(['x86_64-w64-mingw32-g++', '-std=c++20', '-municode', '-static',
                            str(cpp), '-o', str(binary)], check=True, capture_output=True)
            result = subprocess.run(['wine', str(binary)], env={**os.environ, 'WINEDEBUG': '-all'},
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((root / 'reopened.txt').read_text(encoding='utf-8'), 'updated')


if __name__ == '__main__':
    unittest.main()
