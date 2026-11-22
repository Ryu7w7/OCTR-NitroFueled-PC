"""Native Linux launcher services without real accounts or user data."""
from paths import ROOT
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("launcher_linux_services", ROOT / "launcher/linux/services.py")
services = importlib.util.module_from_spec(spec)
spec.loader.exec_module(services)


class NativeLauncherTests(unittest.TestCase):
    def test_restart_waits_for_old_process_and_opens_replacement(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            replacement = root / "replacement"
            replacement.write_text('#!/bin/sh\nprintf "%s" "$1" > "$2"\n')
            replacement.chmod(0o755)
            old = subprocess.Popen(["sleep", "10"])
            try:
                with mock.patch.object(services.os, "getppid", return_value=old.pid):
                    self.assertTrue(services.dispatch({"op": "restart", "data": tmp,
                        "executable": str(replacement), "args": ["--updated", str(root / "opened")]})["ok"])
                self.assertFalse((root / "opened").exists())
            finally:
                old.terminate()
                old.wait()
            for _ in range(50):
                if (root / "opened").exists():
                    break
                time.sleep(0.1)
            self.assertEqual((root / "opened").read_text(), "--updated")

    def test_bundled_patch_updates_preserve_custom_xml(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            package, data = root / "package", root / "data"
            (package / "patches").mkdir(parents=True)
            (data / "patches").mkdir(parents=True)
            patch = package / "patches/game.xml"
            patch.write_text("bundled v1")
            (package / "patches/legacy.xml").write_text("legacy v1")
            (data / "patches/legacy.xml").write_text("legacy v1")
            (package / "patches/custom.xml").write_text("default")
            (data / "patches/custom.xml").write_text("custom override")
            with mock.patch.object(services, "ROOT", package), mock.patch.dict(os.environ,
                    {"XDG_CONFIG_HOME": str(root / "config"), "XDG_DATA_HOME": str(root / "xdg")}):
                services.prepare(data)
                patch.write_text("bundled v2")
                (package / "patches/legacy.xml").write_text("legacy v2")
                services.prepare(data)
                self.assertEqual((data / "patches/game.xml").read_text(), "bundled v2")
                self.assertEqual((data / "patches/legacy.xml").read_text(), "legacy v2")
                self.assertEqual((data / "patches/custom.xml").read_text(), "custom override")
                (data / "patches/game.xml").write_text("edited v2")
                patch.write_text("bundled v3")
                (package / "patches/legacy.xml").unlink()
                (package / "patches/custom.xml").unlink()
                services.prepare(data)
                self.assertEqual((data / "patches/game.xml").read_text(), "edited v2")
                self.assertFalse((data / "patches/legacy.xml").exists())
                self.assertEqual((data / "patches/custom.xml").read_text(), "custom override")




    def test_empty_post_and_auth_options_reach_existing_http_service(self):
        http = mock.Mock()
        http.request.return_value = {"status": "approved"}
        http.clock_offset.return_value = 42
        with mock.patch.dict("sys.modules", {"ryunet_http": http}), mock.patch.dict(os.environ):
            result = services.dispatch({"op": "http", "data": "/data", "server": "localhost:8080",
                "path": "/ryunet/account/device", "method": "POST", "form": "", "timeout": 1000,
                "register": True, "key": "other-key", "bridge_map": "1234,8080:18080"})
            self.assertEqual(os.environ["BB_ONLINE_BRIDGE"], "1234,8080:18080")
        http.request.assert_called_once_with(Path("/data"), "localhost:8080", "/ryunet/account/device", {}, 1.0, True, "other-key")
        self.assertEqual(result, {"status": "approved", "clock_offset": 42, "clock_stamps": False})

    def test_updater_never_replaces_a_checkout(self):
        update = mock.Mock()
        update.running_appimage.return_value = None
        with mock.patch.dict("sys.modules", {"appimage_update": update}):
            with self.assertRaisesRegex(ValueError, "AppImage"):
                services.dispatch({"op": "update_install", "release": {}})
        update.install.assert_not_called()

    def test_steam_artwork_uses_preserved_appid_and_persistent_icon(self):
        import steam_shortcuts
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "steam/config/shortcuts.vdf"
            old = steam_shortcuts.make_entry("/old/SacredFire.AppImage", "/old")
            steam_shortcuts.add_shortcut(path, old)
            with mock.patch.dict(os.environ, {"XDG_DATA_HOME": str(root / "data")}), \
                 mock.patch.object(steam_shortcuts, "steam_running", return_value=False), \
                 mock.patch.object(steam_shortcuts, "shortcut_files", return_value=[path]), \
                 mock.patch.object(steam_shortcuts, "launcher_command", return_value=("/new/SacredFire.AppImage", "/new", "--play")):
                self.assertTrue(services.steam()["ok"])
            saved = steam_shortcuts.parse(path.read_bytes())["shortcuts"]["0"]
            self.assertEqual(saved["appid"], old["appid"])
            self.assertTrue(Path(saved["icon"]).is_relative_to(root / "data"))
            self.assertTrue(Path(saved["icon"]).is_file())
            self.assertEqual(Path(saved["icon"]).read_bytes(),
                             (ROOT / "launcher/io.github.sacredfire.Launcher.png").read_bytes())
            for suffix in ("p.png", "_hero.png", "_logo.png", ".png", "_icon.png"):
                self.assertTrue((path.parent / "grid" / f'{old["appid"] & 0xffffffff}{suffix}').is_file())

    def test_desktop_icon_survives_appimage_unmount(self):
        import steam_shortcuts
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with mock.patch.dict(os.environ, {"XDG_DATA_HOME": str(root / "data")}), \
                 mock.patch.object(services.subprocess, "check_output", return_value=str(root / "Desktop")), \
                 mock.patch.object(steam_shortcuts, "launcher_command", return_value=('/games/100% $CTR.AppImage', '/games', '--play')):
                self.assertTrue(services.desktop()["ok"])
            desktop = root / "Desktop/io.github.sacredfire.Launcher.desktop"
            contents = desktop.read_text()
            self.assertIn('Exec="/games/100%% \\\\$CTR.AppImage" --play', contents)
            icon = next(line[5:] for line in contents.splitlines() if line.startswith("Icon="))
            self.assertTrue(Path(icon).is_relative_to(root / "data"))
            self.assertTrue(Path(icon).is_file())
            self.assertTrue(desktop.stat().st_mode & 0o100)


    def test_discord_launch_registration_preserves_paths_and_updates_handler(self):
        import steam_shortcuts
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with mock.patch.dict(os.environ, {"XDG_DATA_HOME": str(root / "xdg"),
                    "BB_CONFIG": str(root / "custom settings.ini"), "BB_USER_DIR": str(root / "saves")}), \
                    mock.patch.object(steam_shortcuts, "launcher_command", return_value=("/games/100% $CTR.AppImage", "/games", "--play")) as command, \
                    mock.patch.object(services.subprocess, "run", return_value=services.subprocess.CompletedProcess([], 0, "", "")) as register:
                request = {"op": "discord_register", "app_id": "123456", "data": str(root / "data")}
                self.assertTrue(services.dispatch(request)["ok"])
                path = root / "xdg/applications/discord-123456.desktop"
                contents = path.read_text()
                self.assertIn('"/games/100%% \\\\$CTR.AppImage" %u', contents)
                self.assertIn('"BB_DATA_DIR=' + str(root / "data") + '"', contents)
                self.assertIn('"BB_CONFIG=' + str(root / "custom settings.ini") + '"', contents)
                self.assertIn('"BB_USER_DIR=' + str(root / "saves") + '"', contents)
                self.assertIn("NoDisplay=true\n", contents)
                self.assertIn("MimeType=x-scheme-handler/discord-123456;\n", contents)
                self.assertNotIn("--play", contents)
                register.assert_called_with(["gio", "mime", "x-scheme-handler/discord-123456", path.name],
                                            capture_output=True, text=True)
                command.return_value = ("/games/new.AppImage", "/games", "--play")
                self.assertTrue(services.dispatch(request)["ok"])
                self.assertIn('"/games/new.AppImage" %u', path.read_text())
                self.assertNotIn("100%%", path.read_text())

    def test_discord_launch_registration_rejects_invalid_id(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(os.environ, {"XDG_DATA_HOME": tmp}), \
                mock.patch.object(services.subprocess, "run") as register:
            for app_id in ("", "../123", "123\nExec=bad", "１２３"):
                with self.subTest(app_id=app_id), self.assertRaises(ValueError):
                    services.discord_register(app_id, tmp)
            self.assertEqual(list(Path(tmp).iterdir()), [])
            register.assert_not_called()

    def test_appimage_restart_drops_old_libraries_but_keeps_user_paths(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(os.environ, {"APPIMAGE": "/old.AppImage", "FONTCONFIG_FILE": "/nix/store/old/fonts.conf",
                "PYTHONPATH": "/nix/store/old/python", "BB_DATA_DIR": "/custom/data", "BB_CONFIG": "/custom/game.ini",
                "BB_USER_DIR": "/custom/saves", "BB_PREBUILT": "1"}), mock.patch.object(services.subprocess, "Popen") as spawn:
            result = services.dispatch({"op": "restart", "data": tmp, "executable": "/games/new.AppImage", "args": ["--updated"]})
        self.assertTrue(result["ok"])
        self.assertEqual(spawn.call_args.args[0][-2:], ["/games/new.AppImage", "--updated"])
        env = spawn.call_args.kwargs["env"]
        for key in ("APPIMAGE", "FONTCONFIG_FILE", "PYTHONPATH", "BB_PREBUILT"):
            self.assertNotIn(key, env)
        self.assertEqual(env["BB_DATA_DIR"], "/custom/data")
        self.assertEqual(env["BB_CONFIG"], "/custom/game.ini")
        self.assertEqual(env["BB_USER_DIR"], "/custom/saves")

    def test_appimage_open_drops_bundle_environment(self):
        with mock.patch.dict(os.environ, {"APPIMAGE": "/games/SacredFire.AppImage",
                "LD_LIBRARY_PATH": "/nix/store/bundled/lib"}), mock.patch.object(services.subprocess, "Popen") as spawn:
            result = services.dispatch({"op": "open", "target": "/games/My Port"})
        self.assertTrue(result["ok"])
        self.assertEqual(spawn.call_args.args[0], ["xdg-open", "/games/My Port"])
        env = spawn.call_args.kwargs["env"]
        self.assertNotIn("APPIMAGE", env)
        self.assertNotIn("LD_LIBRARY_PATH", env)
