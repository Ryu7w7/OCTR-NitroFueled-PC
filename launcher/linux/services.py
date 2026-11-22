#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Native launcher's Linux integrations."""
import hashlib
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


def prepare(data):
    """Refresh bundled patches and the desktop icon."""
    data = Path(data)
    data.mkdir(parents=True, exist_ok=True)
    # Update managed copies, but keep edited or independently installed XML as user overrides.
    if data.resolve() != ROOT.resolve():
        (data / "patches").mkdir(exist_ok=True)
        manifest = data / "patches/.bundled.json"
        try:
            managed = json.loads(manifest.read_text())
        except (FileNotFoundError, ValueError):
            managed = {}
        if not isinstance(managed, dict):
            managed = {}
        bundled_names = set()
        for patch in (ROOT / "patches").glob("*.xml"):
            bundled_names.add(patch.name)
            dest = data / "patches" / patch.name
            if dest.is_symlink():
                continue
            bundled = hashlib.sha256(patch.read_bytes()).hexdigest()
            current = hashlib.sha256(dest.read_bytes()).hexdigest() if dest.exists() else None
            if current is None or current == managed.get(patch.name) or current == bundled:
                if current != bundled:
                    temporary = dest.with_suffix(".tmp")
                    shutil.copyfile(patch, temporary)
                    temporary.replace(dest)
                managed[patch.name] = bundled
        for dest in (data / "patches").glob("*.xml"):
            if dest.name not in bundled_names and not dest.is_symlink() and \
                    hashlib.sha256(dest.read_bytes()).hexdigest() == managed.get(dest.name):
                dest.unlink()
                del managed[dest.name]
        temporary = manifest.with_suffix(".tmp")
        temporary.write_text(json.dumps(managed, sort_keys=True))
        temporary.replace(manifest)
    try:
        install_icon()
    except OSError:
        pass  # A read-only desktop icon cache must not block the launcher.
    return {"ok": True}


def open_url(url):
    from host_environment import host_environment
    return subprocess.Popen(["xdg-open", url], env=host_environment() if os.environ.get("APPIMAGE") else None,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


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


def desktop_value(text):
    return text.replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")


def desktop_arg(text):
    # Desktop Entry Exec quoting is distinct from shell quoting.
    escaped = text.replace("\\", "\\\\").replace('"', '\\"').replace("`", "\\`").replace("$", "\\$").replace("%", "%%")
    return '"' + desktop_value(escaped) + '"'


def discord_register(app_id, data):
    if not app_id or any(c not in "0123456789" for c in app_id):
        raise ValueError("Invalid Discord application ID")
    return url_register(f"discord-{app_id}", data)


def url_register(scheme, data):
    """Opens <scheme>:// links with this launcher (the link as its argument): discord-<app id> for
    Discord invites, sacredfire for the Join button of RyuNET's Ranked announcements."""
    import steam_shortcuts
    from host_environment import host_environment
    if not scheme or any(not (c.isascii() and (c.isalnum() or c == "-")) for c in scheme):
        raise ValueError("Invalid URL scheme")
    exe, cwd, _ = steam_shortcuts.launcher_command()
    paths = {k: v for k, v in host_environment(for_restart=True).items() if k.startswith("BB_")}
    paths["BB_DATA_DIR"] = str(Path(data).resolve())
    command = " ".join(desktop_arg(arg) for arg in ["env", *(f"{k}={v}" for k, v in paths.items()), exe])
    mime = f"x-scheme-handler/{scheme}"
    directory = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "applications"
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{scheme}.desktop"
    path.write_text(f"[Desktop Entry]\nType=Application\nName=SacredFire\nNoDisplay=true\n"
                    f"Exec={command} %u\nPath={desktop_value(cwd)}\nMimeType={mime};\n")
    result = subprocess.run(["gio", "mime", mime, path.name], capture_output=True, text=True)
    if result.returncode:
        raise OSError(result.stderr.strip() or f"Could not register {scheme}:// links")
    return {"ok": True}


def desktop():
    import steam_shortcuts
    exe, cwd, _ = steam_shortcuts.launcher_command()
    try:
        desktop_dir = subprocess.check_output(["xdg-user-dir", "DESKTOP"], text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        desktop_dir = str(Path.home() / "Desktop")
    directory = Path(desktop_dir)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "io.github.sacredfire.Launcher.desktop"
    path.write_text(f'[Desktop Entry]\nType=Application\nName=SacredFire\nExec={desktop_arg(exe)} --play\n'
                    f'Path={desktop_value(cwd)}\nIcon={desktop_value(install_icon())}\nTerminal=false\nCategories=Game;\n')
    path.chmod(0o755)
    return {"ok": True}


def dispatch(request):
    operation = request["op"]
    data = Path(request.get("data", ROOT))
    if operation == "prepare":
        return prepare(data)
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
    if operation == "open":
        open_url(request["target"])
        return {"ok": True}
    if operation == "steam":
        return steam()
    if operation == "desktop":
        return desktop()
    if operation == "discord_register":
        return discord_register(request["app_id"], data)
    if operation == "url_register":
        return url_register(request["scheme"], data)
    if operation == "restart":
        from host_environment import host_environment
        executable = Path(request["executable"])
        log = Path(request.get("data", ROOT)) / "launcher-restart.log"
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("a") as output:
            # shortcut: wait at most 10 seconds for shutdown; use a supervisor if shutdown can exceed it.
            subprocess.Popen(["/bin/sh", "-c",
                              'i=0; while [ -e "/proc/$1/exe" ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i+1)); done; shift; exec "$@"',
                              "restart", str(os.getppid()), str(executable), *request["args"]],
                             cwd=executable.parent,
                             env=host_environment(for_restart=True) if os.environ.get("APPIMAGE") else None,
                             start_new_session=True, stdin=subprocess.DEVNULL,
                             stdout=output, stderr=subprocess.STDOUT)
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
