"""Non-Steam shortcuts for SacredFire: Steam's userdata/<user>/config/shortcuts.vdf.

Steam keeps its non-Steam games in a binary VDF file that it rewrites when it exits, so an entry
added while Steam runs is lost: callers check `steam_running()` first. The file is a map of
numbered entries; each entry holds strings (0x01), 32-bit integers (0x02) and nested maps (0x00,
ended by 0x08). The app id of a shortcut is the CRC32 of its executable and name with the top bit
set, as Steam computes it (the same id names its grid artwork).
"""
import os
import struct
import subprocess
import zlib
from pathlib import Path

NAME = "SacredFire"


def steam_roots():
    """Steam installations of this user, existing ones only (symlinked roots resolve to one)."""
    home = Path.home()
    seen, roots = set(), []
    for candidate in (home / ".steam/steam", home / ".local/share/Steam", home / ".steam/root",
                      home / ".var/app/com.valvesoftware.Steam/.local/share/Steam"):
        if not (candidate / "userdata").is_dir():
            continue
        real = candidate.resolve()
        if real in seen:
            continue
        seen.add(real)
        roots.append(candidate)
    return roots


def shortcut_files():
    """shortcuts.vdf of every Steam user found (the file may not exist yet: the path is returned)."""
    files = []
    for root in steam_roots():
        for user in sorted((root / "userdata").iterdir()):
            if user.name.isdigit() and user.name != "0" and (user / "config").is_dir():
                files.append(user / "config/shortcuts.vdf")
    return files


def steam_running():
    try:
        return subprocess.run(["pgrep", "-x", "steam"], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
    except OSError:
        return False


# --- binary VDF ----------------------------------------------------------------------------

def _read_string(data, at):
    end = data.index(b"\0", at)
    return data[at:end].decode("utf-8", "replace"), end + 1


def parse(data):
    """The file as nested dicts (keys and string values as str, integers as int)."""
    def read_map(at):
        result = {}
        while True:
            kind = data[at]
            at += 1
            if kind == 0x08:
                return result, at
            key, at = _read_string(data, at)
            if kind == 0x00:
                result[key], at = read_map(at)
            elif kind == 0x01:
                result[key], at = _read_string(data, at)
            elif kind == 0x02:
                result[key] = struct.unpack_from("<i", data, at)[0]
                at += 4
            else:
                raise ValueError(f"unknown VDF field type {kind:#x} at {at - 1}")
    if not data:
        return {"shortcuts": {}}
    top, _ = read_map(0)
    return top


def serialize(top):
    out = bytearray()

    def write_map(mapping):
        nonlocal out
        for key, value in mapping.items():
            if isinstance(value, dict):
                out.append(0x00)
                out += key.encode() + b"\0"
                write_map(value)
            elif isinstance(value, int):
                out.append(0x02)
                out += key.encode() + b"\0" + struct.pack("<i", value)
            else:
                out.append(0x01)
                out += key.encode() + b"\0" + str(value).encode() + b"\0"
        out.append(0x08)
    write_map(top)
    return bytes(out)


def shortcut_appid(exe, name):
    """Steam's id for a non-Steam shortcut, as a signed 32-bit value (the file stores it so)."""
    value = (zlib.crc32((exe + name).encode()) | 0x80000000) & 0xFFFFFFFF
    return value - (1 << 32) if value >= 1 << 31 else value


def make_entry(exe, start_dir, name=NAME, launch_options="", icon=""):
    quoted = f'"{exe}"'
    return {
        "appid": shortcut_appid(quoted, name),
        "AppName": name,
        "Exe": quoted,
        "StartDir": f'"{start_dir}"',
        "icon": icon,
        "ShortcutPath": "",
        "LaunchOptions": launch_options,
        "IsHidden": 0,
        "AllowDesktopConfig": 1,
        "AllowOverlay": 1,
        "OpenVR": 0,
        "Devkit": 0,
        "DevkitGameID": "",
        "DevkitOverrideAppID": 0,
        "LastPlayTime": 0,
        "FlatpakAppID": "",
        "tags": {},
    }


def ours(existing, entry):
    """An entry this launcher wrote earlier: its name, and an AppImage or the checkout's script
    (an older release's AppImage, under its own versioned file name)."""
    exe = str(existing.get("Exe", "")).strip('"')
    return existing.get("AppName") == entry["AppName"] and (exe.endswith(".AppImage") or exe.endswith("bb-launcher.sh"))


def add_shortcut(path, entry):
    """Adds `entry` to the shortcuts file at `path` (created if missing). An entry with the same
    executable and name is replaced; so is one this launcher wrote for another AppImage (an older
    release), keeping its app id, so Steam keeps its artwork and play time. Returns True when a
    new entry was added, False on replace."""
    path = Path(path)
    data = path.read_bytes() if path.exists() else b""
    top = parse(data)
    shortcuts = top.setdefault("shortcuts", {})
    same = [k for k, e in shortcuts.items() if isinstance(e, dict) and e.get("Exe") == entry["Exe"]
            and e.get("AppName") == entry["AppName"]]
    older = [k for k, e in shortcuts.items() if isinstance(e, dict) and k not in same and ours(e, entry)]
    if same or older:
        key = (same or older)[0]
        if not same:
            kept = shortcuts[key]
            entry = dict(entry)
            for field in ("appid", "LastPlayTime", "IsHidden", "tags"):
                if field in kept:
                    entry[field] = kept[field]
        shortcuts[key] = entry
        added = False
    else:
        index = max((int(k) for k in shortcuts if k.isdigit()), default=-1) + 1
        shortcuts[str(index)] = entry
        added = True
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    temp.write_bytes(serialize(top))
    os.replace(temp, path)
    return added


def stable_appimage_link():
    """A fixed path for the AppImage this launcher runs from (APPIMAGE, set by its runtime):
    <data dir>/SacredFire.AppImage, a symlink kept pointing at the AppImage opened last. Releases
    come as versioned files (SacredFire-v0.5.2-x86_64.AppImage); a Steam shortcut to the link
    keeps working once a newer one has been opened. None when not running from an AppImage or
    the link cannot be made (a file of the player's own there is left alone)."""
    appimage = os.environ.get("APPIMAGE")
    if not appimage or not Path(appimage).is_file():
        return None
    data = Path(os.environ.get("BB_DATA_DIR") or
                Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "sacredfire")
    link = data / "SacredFire.AppImage"
    target = Path(appimage).resolve()
    try:
        data.mkdir(parents=True, exist_ok=True)
        if link.is_symlink():
            if Path(os.readlink(link)) == target:
                return str(link)
            link.unlink()
        elif link.exists():
            return None
        link.symlink_to(target)
        return str(link)
    except OSError:
        return None


def launcher_command():
    """What Steam should start: the AppImage when running from one (through its fixed link when
    it can be made), else launcher/bb-launcher.sh; `--play` starts the game without the launcher
    window (game mode)."""
    appimage = os.environ.get("APPIMAGE")
    if appimage and Path(appimage).is_file():
        exe = stable_appimage_link() or str(Path(appimage).resolve())
    else:
        exe = str((Path(__file__).resolve().parent / "bb-launcher.sh"))
    return exe, str(Path(exe).parent), "--play"
