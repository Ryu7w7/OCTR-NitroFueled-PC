"""Linux UI migration and service boundary; no GTK, real accounts, or user data."""
from paths import ROOT
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("launcher_linux_services", ROOT / "launcher/linux/services.py")
services = importlib.util.module_from_spec(spec)
spec.loader.exec_module(services)


class NativeLauncherTests(unittest.TestCase):
    def test_migration_keeps_unicode_settings_and_original_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = root / "config/sacredfire-launcher"
            config.mkdir(parents=True)
            source = config / "settings.json"
            original = json.dumps({"game_dir": "/games/日本語 café", "mangohud": True, "gpu_nth": 1,
                                   "extra_env": "BB_TRACE=1", "bad": "value\nrenderer=d3d11"})
            source.write_text(original)
            with mock.patch.dict(os.environ, {"XDG_CONFIG_HOME": str(config.parent), "XDG_DATA_HOME": str(root / "xdg-data")}):
                icon = Path(services.install_icon())
                icon.write_bytes(b"old icon")
                services.migrate(root / "data")
                self.assertEqual(icon.read_bytes(), (ROOT / "launcher/io.github.sacredfire.Launcher.png").read_bytes())
                ini = (root / "data/launcher.ini").read_text()
                self.assertIn("game_dir=/games/日本語 café\n", ini)
                self.assertIn("mangohud=1\n", ini)
                self.assertIn("gpu_nth=1\n", ini)
                self.assertNotIn("renderer=d3d11", ini)
                self.assertEqual(source.read_text(), original)
                (root / "data/launcher.ini").write_text("game_dir=/new\ngtk_migration=1\n")
                with mock.patch.object(services, "install_icon", side_effect=PermissionError):
                    services.migrate(root / "data")
                self.assertEqual((root / "data/launcher.ini").read_text(), "game_dir=/new\ngtk_migration=1\n")

    def test_malformed_preferences_do_not_replace_native_settings(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = root / "config/sacredfire-launcher"
            config.mkdir(parents=True)
            (config / "settings.json").write_text("{")
            with mock.patch.dict(os.environ, {"XDG_CONFIG_HOME": str(config.parent)}):
                with self.assertRaises(ValueError):
                    services.migrate(root / "data")
            self.assertFalse((root / "data/launcher.ini").exists())

    def test_migration_converts_legacy_pro_mode_display_and_paths(self):
        for game_settings, expected in (("", "1440"), ("patch_pro_1080p=1\n", "1080"), ("render_res=720\n", "720")):
            with self.subTest(game_settings=game_settings), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                config = root / "config/sacredfire-launcher"
                config.mkdir(parents=True)
                (config / "settings.json").write_text(json.dumps({"neo": True, "display": "2: External", "game_dir": "~/CTR"}))
                data = root / "data"
                data.mkdir()
                game = data / "bbport.ini"
                game.write_text(game_settings)
                with mock.patch.dict(os.environ, {"HOME": tmp, "XDG_CONFIG_HOME": str(config.parent),
                                                  "XDG_DATA_HOME": str(root / "xdg-data"), "BB_CONFIG": str(game)}):
                    services.migrate(data)
                self.assertIn("render_res=" + expected, game.read_text())
                ini = (data / "launcher.ini").read_text()
                self.assertIn("display=2\n", ini)
                self.assertIn(f"game_dir={root}/CTR\n", ini)

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

    def test_migration_merges_missing_keys_and_keeps_backups(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = root / "config/sacredfire-launcher"
            config.mkdir(parents=True)
            original = json.dumps({"game_dir": "/old", "ui_language": "id", "fullscreen": True, "neo": True})
            (config / "settings.json").write_text(original)
            data = root / "data"
            data.mkdir()
            (data / "launcher.ini").write_text("game_dir=/chosen\n")
            game = data / "bbport.ini"
            game.write_text("key.cross=Q\npad.cross=b\nvolume=42\n")
            with mock.patch.dict(os.environ, {"XDG_CONFIG_HOME": str(config.parent),
                                              "XDG_DATA_HOME": str(root / "xdg-data"), "BB_CONFIG": str(game)}):
                services.migrate(data)
                first = (data / "launcher.ini").read_text()
                services.migrate(data)
            self.assertIn("game_dir=/chosen\n", first)
            self.assertIn("ui_language=id\n", first)
            self.assertEqual((data / "launcher.ini").read_text(), first)
            self.assertEqual((data / "launcher.ini.pre-native.bak").read_text(), "game_dir=/chosen\n")
            self.assertEqual((data / "bbport.ini.pre-native.bak").read_text(), "key.cross=Q\npad.cross=b\nvolume=42\n")
            self.assertEqual((config / "settings.json").read_text(), original)
            self.assertIn("fullscreen=1", game.read_text())
            self.assertIn("render_res=1440", game.read_text())

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
        with mock.patch.dict(os.environ, {"APPIMAGE": "/old.AppImage", "FONTCONFIG_FILE": "/nix/store/old/fonts.conf",
                "PYTHONPATH": "/nix/store/old/python", "BB_DATA_DIR": "/custom/data", "BB_CONFIG": "/custom/game.ini",
                "BB_USER_DIR": "/custom/saves", "BB_PREBUILT": "1"}), mock.patch.object(services.subprocess, "Popen") as spawn:
            result = services.dispatch({"op": "restart", "executable": "/games/new.AppImage", "args": ["--updated"]})
        self.assertTrue(result["ok"])
        self.assertEqual(spawn.call_args.args[0], ["/games/new.AppImage", "--updated"])
        env = spawn.call_args.kwargs["env"]
        for key in ("APPIMAGE", "FONTCONFIG_FILE", "PYTHONPATH", "BB_PREBUILT"):
            self.assertNotIn(key, env)
        self.assertEqual(env["BB_DATA_DIR"], "/custom/data")
        self.assertEqual(env["BB_CONFIG"], "/custom/game.ini")
        self.assertEqual(env["BB_USER_DIR"], "/custom/saves")
