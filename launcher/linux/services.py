#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Native launcher's Linux integrations, reusing the existing GTK launcher's services."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import urllib.parse

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / "launcher")]


def migrate(data):
    """Import missing GTK preferences once, preserving existing native values and original files."""
    data = Path(data)
    data.mkdir(parents=True, exist_ok=True)
    target = data / "launcher.ini"
    def values(text):
        return {key.strip(): value.strip() for line in text.splitlines()
                if "=" in line and not line.lstrip().startswith(("#", ";"))
                for key, value in [line.split("=", 1)]}
    original = target.read_text() if target.exists() else ""
    native = values(original)
    config = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config"))
    source = config / "sacredfire-launcher/settings.json"
    if native.get("gtk_migration") != "1" and source.is_file():
        settings = json.loads(source.read_text())
        if not isinstance(settings, dict):
            raise ValueError("GTK preferences must be an object")
        for key in ("game_dir", "user_dir"):
            if isinstance(settings.get(key), str) and settings[key]:
                settings[key] = str(Path(settings[key]).expanduser())
        if isinstance(settings.get("display"), str):
            settings["display"] = settings["display"].split(":", 1)[0].strip()
        lines = []
        for key, value in settings.items():
            if key in native or key == "gtk_migration" or not isinstance(value, (str, int, float, bool)):
                continue
            value = str(int(value)) if isinstance(value, bool) else str(value)
            if any(c in key + value for c in "\r\n") or "=" in key:
                continue
            lines.append(f"{key}={value}")
        game = Path(os.environ.get("BB_CONFIG", data / "bbport.ini"))
        existing = game.read_text() if game.exists() else ""
        game_values = values(existing)
        additions = []
        if "fullscreen" in settings and "fullscreen" not in game_values:
            additions.append("fullscreen=" + str(int(bool(settings["fullscreen"]))))
        if "neo" in settings and not game_values.get("render_res"):
            resolution = "1440" if settings["neo"] and game_values.get("patch_pro_1080p", "0") != "1" else "1080"
            additions.append("render_res=" + resolution)
        writes = [(target, original.rstrip("\n") + "\n" + "\n".join(lines + ["gtk_migration=1"]) + "\n")]
        if additions:
            writes.insert(0, (game, existing.rstrip("\n") + "\n" + "\n".join(additions) + "\n"))
        for path, content in writes:
            path.parent.mkdir(parents=True, exist_ok=True)
            backup = path.with_name(path.name + ".pre-native.bak")
            if path.exists() and not backup.exists():
                shutil.copy2(path, backup)
            temporary = path.with_name(path.name + ".tmp")
            temporary.write_text(content)
            temporary.replace(path)
    # The package is read-only. Only missing bundled patches are seeded into writable data.
    if data.resolve() != ROOT.resolve():
        (data / "patches").mkdir(exist_ok=True)
        for patch in (ROOT / "patches").glob("*.xml"):
            dest = data / "patches" / patch.name
            if not dest.exists():
                shutil.copyfile(patch, dest)
    try:
        install_icon()
    except OSError:
        pass  # A read-only desktop icon cache must not block the launcher or settings migration.
    return {"ok": True}


def open_url(url):
    return subprocess.Popen(["xdg-open", url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def install_icon():
    # AppImage resource paths disappear when the launcher exits.
    data = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
    icon = data / "icons/hicolor/256x256/apps/io.github.sacredfire.Launcher.png"
    icon.parent.mkdir(parents=True, exist_ok=True)
    source = ROOT / "launcher/io.github.sacredfire.Launcher.png"
    if not icon.is_file() or icon.read_bytes() != source.read_bytes():
        shutil.copyfile(source, icon)
        os.utime(data / "icons/hicolor", None)
    (data / "icons/hicolor/512x512/apps/io.github.sacredfire.Launcher.png").unlink(missing_ok=True)
    return str(icon)


def steam():
    import steam_shortcuts
    if steam_shortcuts.steam_running():
        return {"ok": False, "error": "Close Steam before adding the shortcut, then try again."}
    files = steam_shortcuts.shortcut_files()
    if not files:
        return {"ok": False, "error": "No Steam user found. Open Steam and sign in first."}
    exe, cwd, options = steam_shortcuts.launcher_command()
    entry = steam_shortcuts.make_entry(exe, cwd, launch_options=options, icon=install_icon())
    artwork = next((ROOT / name for name in ("art/steamgrid", "steamgrid", "packaging/windows/steamgrid")
                    if (ROOT / name).is_dir()), None)
    for path in files:
        steam_shortcuts.add_shortcut(path, entry)
        grid = path.parent / "grid"
        grid.mkdir(exist_ok=True)
        saved = steam_shortcuts.parse(path.read_bytes())["shortcuts"]
        appid = next(e["appid"] for e in saved.values()
                     if e.get("Exe") == entry["Exe"] and e.get("AppName") == entry["AppName"]) & 0xffffffff
        shutil.copyfile(ROOT / "launcher/io.github.sacredfire.Launcher.png", grid / f"{appid}_icon.png")
        (grid / f"{appid}_icon.jpg").unlink(missing_ok=True)
        if artwork is None:
            continue
        for name, suffix in [("portada.png", "p.png"), ("fondo.png", "_hero.png"), ("logo.png", "_logo.png"),
                             ("portada_ancha.png", ".png")]:
            image = artwork / name
            if image.is_file():
                shutil.copyfile(image, grid / f"{appid}{suffix}")
    return {"ok": True, "message": "Added to Steam. Restart Steam to see the shortcut."}


def desktop():
    import steam_shortcuts
    exe, cwd, _ = steam_shortcuts.launcher_command()
    try:
        desktop_dir = subprocess.check_output(["xdg-user-dir", "DESKTOP"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        desktop_dir = str(Path.home() / "Desktop")
    directory = Path(desktop_dir)
    directory.mkdir(parents=True, exist_ok=True)
    # Desktop Entry Exec quoting is distinct from shell quoting.
    escaped = exe.replace("\\", "\\\\").replace('"', '\\"').replace("`", "\\`").replace("$", "\\$").replace("%", "%%")
    def value(text):
        return text.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")
    path = directory / "io.github.sacredfire.Launcher.desktop"
    path.write_text(f'[Desktop Entry]\nType=Application\nName=SacredFire\nExec="{value(escaped)}" --play\n'
                    f'Path={value(cwd)}\nIcon={value(install_icon())}\nTerminal=false\nCategories=Game;\n')
    path.chmod(0o755)
    return {"ok": True}


def dispatch(request):
    operation = request["op"]
    data = Path(request.get("data", ROOT))
    if operation == "migrate":
        return migrate(data)
    if operation == "http":
        import ryunet_http
        os.environ["BB_ONLINE_BRIDGE"] = request.get("bridge_map", "")
        ryunet_http.run.SERVER_CLOCK_OFFSET = int(request.get("clock_offset", 0))
        ryunet_http.run.SERVER_STAMPS = bool(request.get("clock_stamps", False))
        form = dict(urllib.parse.parse_qsl(request.get("form", ""), keep_blank_values=True)) if request["method"] == "POST" else None
        answer = ryunet_http.request(data, request["server"], request["path"], form,
                                    request["timeout"] / 1000, request.get("register", False), request.get("key") or None)
        return {**(answer or {}), "clock_offset": ryunet_http.clock_offset(), "clock_stamps": ryunet_http.run.SERVER_STAMPS}
    if operation == "password":
        import ryunet_http
        ryunet_http.run.SERVER_CLOCK_OFFSET = int(request.get("clock_offset", 0))
        body = json.loads(request["body"])
        return ryunet_http.password_login(data, request["server"], body["nick"], body["password"])
    if operation == "oauth":
        import discord_oauth
        return discord_oauth.authorize(request["client"], open_url, timeout=request["timeout"])
    if operation == "steam":
        return steam()
    if operation == "desktop":
        return desktop()
    if operation == "restart":
        from host_environment import host_environment
        executable = Path(request["executable"])
        subprocess.Popen([str(executable), *request["args"]], cwd=executable.parent,
                         env=host_environment(for_restart=True) if os.environ.get("APPIMAGE") else None,
                         start_new_session=True, stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return {"ok": True}
    if operation == "trophy_key":
        import trophies
        return {"key": trophies.shadps4_key()[0]}
    if operation == "update_check":
        import appimage_update
        return appimage_update.check(request["version"])
    if operation == "update_install":
        import appimage_update
        target = appimage_update.running_appimage()
        if target is None:
            raise ValueError("Automatic updates require an AppImage. Update this installation with its package manager or Git.")
        def cancelled(_signal, _frame):
            raise InterruptedError("Update cancelled")
        signal.signal(signal.SIGTERM, cancelled)
        return {"ok": True, "path": str(appimage_update.install(request["release"], target))}
    raise ValueError("Unknown launcher operation")


def presence():
    import discord_ipc
    client = discord_ipc.Presence()
    try:
        for line in sys.stdin:
            request = json.loads(line)
            activity = request.get("activity")
            client.accept_requests = request.get("accept", True)
            client.update(request.get("enabled", False), request.get("app_id", ""),
                          discord_ipc.Activity(**activity) if activity else None)
            status, error = client.status()
            print(json.dumps({"status": status, "error": error, "join": client.take_join() or ""}, separators=(",", ":")), flush=True)
    finally:
        client.shutdown()


if __name__ == "__main__":
    if sys.argv[1:] == ["--presence"]:
        presence()
    else:
        try:
            result = dispatch(json.load(sys.stdin))
        except (OSError, ValueError, KeyError) as error:
            result = {"error": str(error)}
        print(json.dumps(result, separators=(",", ":")))
