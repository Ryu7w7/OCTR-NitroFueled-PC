"""Update handoff and the runtime's real restart exit, without GTK or a game."""
import ast
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

from paths import ROOT


class RestartTests(unittest.TestCase):
    def launcher_scope(self):
        tree = ast.parse((ROOT / 'launcher/sacredfire_launcher.py').read_text())
        app = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == 'LauncherApp')
        window = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == 'LauncherWindow')
        restart = next(n for n in window.body if isinstance(n, ast.FunctionDef) and n.name == 'restart_into')
        base = mock.Mock()
        class Application:
            def __init__(self, **kwargs):
                base(**kwargs)
        scope = {'Adw': types.SimpleNamespace(Application=Application), 'APP_ID': 'test',
                 'Gio': types.SimpleNamespace(ApplicationFlags=types.SimpleNamespace(DEFAULT_FLAGS=0, NON_UNIQUE=1)),
                 'sys': sys, 'subprocess': mock.Mock(DEVNULL=subprocess.DEVNULL),
                 'host_environment': lambda: {'PATH': '/usr/bin'}, 'tr': lambda s: s}
        exec(compile(ast.Module(body=[app, restart], type_ignores=[]), 'launcher', 'exec'), scope)
        return scope, base

    def test_update_starts_independent_launcher_before_quitting(self):
        scope, base = self.launcher_scope()
        with mock.patch.object(sys, 'argv', ['launcher', '--updated']):
            scope['LauncherApp']()
        self.assertEqual(base.call_args.kwargs['flags'], 1)
        with mock.patch.object(sys, 'argv', ['launcher']):
            scope['LauncherApp']()
        self.assertEqual(base.call_args.kwargs['flags'], 0)
        window = mock.Mock()
        scope['restart_into'](window, Path('/opt/CTR Game/SacredFire.AppImage'))
        scope['subprocess'].Popen.assert_called_once_with(
            ['/opt/CTR Game/SacredFire.AppImage', '--updated'], cwd='/opt/CTR Game',
            env={'PATH': '/usr/bin'}, start_new_session=True,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        window.presence.shutdown.assert_called_once()
        window.get_application().quit.assert_called_once()

    def test_failed_update_handoff_keeps_launcher_open(self):
        scope, _ = self.launcher_scope()
        scope['subprocess'].Popen.side_effect = OSError('permission denied')
        window = mock.Mock()
        scope['restart_into'](window, Path('/opt/SacredFire.AppImage'))
        window.get_application.assert_not_called()
        window.presence.shutdown.assert_not_called()
        self.assertIn('permission denied', window.update_banner.set_title.call_args.args[0])

    @unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc'), 'requires Linux C compiler')
    def test_runtime_restart_exits_to_supervisor(self):
        source = (ROOT / 'src/probe.c').read_text()
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
        source = (ROOT / 'src/probe.c').read_text()
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
        source = (ROOT / 'launcher/windows/launcher.cpp').read_text()
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
            self.assertEqual((root / 'reopened.txt').read_text(), 'updated')


if __name__ == '__main__':
    unittest.main()
