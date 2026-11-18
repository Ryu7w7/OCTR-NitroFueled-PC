# SPDX-License-Identifier: GPL-2.0-or-later
"""Leave AppImage libraries behind when opening host programs or a replacement AppImage."""
import os
from pathlib import Path

FOREIGN_ENV = ("LD_LIBRARY_PATH", "LD_PRELOAD", "GDK_PIXBUF_MODULE_FILE", "FONTCONFIG_FILE", "GIO_EXTRA_MODULES",
               "GI_TYPELIB_PATH", "GSETTINGS_SCHEMA_DIR", "GTK_PATH", "GTK_EXE_PREFIX", "GTK_DATA_PREFIX",
               "PYTHONPATH", "PYTHONHOME", "PYTHONNOUSERSITE", "APPDIR", "APPIMAGE", "ARGV0", "OWD",
               "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_ADD_LAYER_PATH", "VK_LAYER_PATH", "LIBGL_DRIVERS_PATH",
               "__EGL_VENDOR_LIBRARY_DIRS", "LIBVA_DRIVERS_PATH")


def host_environment(*, for_restart=False):
    """The environment for a program of the player's system: without what the AppImage added, and
    with the system's own PATH and data directories."""
    env = {k: v for k, v in os.environ.items() if k not in FOREIGN_ENV and not k.startswith(("BB_", "NIX"))}
    path = [p for p in env.get("PATH", "").split(":") if p and not p.startswith("/nix/")]
    env["PATH"] = ":".join(path + [p for p in ("/usr/local/bin", "/usr/bin", "/bin") if p not in path])
    data = [d for d in env.get("XDG_DATA_DIRS", "").split(":") if d and not d.startswith("/nix/")]
    home = Path.home()
    for d in (str(home / ".local/share/flatpak/exports/share"), "/var/lib/flatpak/exports/share",
              "/usr/local/share", "/usr/share"):
        if d not in data:
            data.append(d)
    env["XDG_DATA_DIRS"] = ":".join(data)
    if env.get("LC_ALL") == "C.UTF-8":
        del env["LC_ALL"]  # the wrapper's default, not the player's
    if for_restart:
        for key in ("BB_DATA_DIR", "BB_CONFIG", "BB_USER_DIR", "BB_PATCHES_CONFIG", "BB_MODS_DIR", "BB_MODS_CONFIG"):
            if key in os.environ:
                env[key] = os.environ[key]
    return env
