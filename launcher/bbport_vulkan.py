#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Select Vulkan ICDs for the packaged port before starting GTK or the game.

Mesa comes with the package. NVIDIA's userspace driver must come from the host,
matching its kernel module. Expose only NVIDIA libraries, rather than the host's
whole library directory (which would also replace the package's libc/GTK/SDL).
"""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

MANIFEST_DIRS = (
    Path("/etc/vulkan/icd.d"), Path("/usr/share/vulkan/icd.d"),
    Path("/usr/local/share/vulkan/icd.d"), Path("/run/opengl-driver/share/vulkan/icd.d"),
)
LIBRARY_DIRS = (
    Path("/usr/lib/x86_64-linux-gnu"), Path("/lib/x86_64-linux-gnu"),
    Path("/usr/lib64"), Path("/lib64"), Path("/usr/lib"), Path("/lib"),
    Path("/run/opengl-driver/lib"),
)
# The host's EGL vendor files (glvnd). The package's own glvnd looks only in its own store.
EGL_VENDOR_DIRS = (
    Path("/usr/share/glvnd/egl_vendor.d"), Path("/etc/glvnd/egl_vendor.d"),
    Path("/usr/local/share/glvnd/egl_vendor.d"),
)


AMD_VENDOR = "0x1002"


def amd_gpu(drm_dir=Path("/sys/class/drm")):
    """Whether the system has an AMD GPU (the new memory model runs on AMD only for now).

    True or False from the kernel's PCI vendor ids; None when they cannot be read (a sandbox
    without /sys): the game itself turns the model off on another GPU.
    """
    vendors = set()
    for path in drm_dir.glob("card[0-9]*/device/vendor"):
        try:
            vendors.add(path.read_text().strip().lower())
        except OSError:
            continue
    if not vendors:
        return None
    return AMD_VENDOR in vendors


def elf64(path):
    """Reject 32-bit ICDs in distributions that install both architectures."""
    try:
        with path.open("rb") as stream:
            header = stream.read(20)
        return header[:6] == b"\x7fELF\x02\x01" and header[18:20] == b"\x3e\x00"
    except OSError:
        return False


def host_nvidia(manifest_dirs, library_dirs):
    for directory in manifest_dirs:
        for manifest in sorted(directory.glob("*nvidia*.json")):
            try:
                data = json.loads(manifest.read_text(encoding='utf-8'))
                library = Path(data["ICD"]["library_path"])
            except (OSError, ValueError, KeyError, TypeError):
                continue
            if "nvidia" not in library.name.lower():
                continue
            if library.is_absolute():
                candidates = [library, *(d / library.name for d in library_dirs)]
            elif len(library.parts) > 1:
                candidates = [manifest.parent / library]
            else:
                candidates = [d / library for d in library_dirs]
            for candidate in candidates:
                if elf64(candidate):
                    return data, candidate.resolve()
    return None


def configure(env, manifest_dirs=MANIFEST_DIRS, library_dirs=LIBRARY_DIRS,
              egl_dirs=EGL_VENDOR_DIRS):
    # Steam's Game Mode starts non-Steam shortcuts in pressure-vessel, which sets these to its
    # own proxy ICDs and layers: not the user's choice, and with them the bundled loader finds
    # no VK_KHR_surface (#172). Dropped, so the bundled or host drivers are found as usual.
    if any("/pressure-vessel/" in env.get(name, "")
           for name in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_IMPLICIT_LAYER_PATH")):
        for name in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_IMPLICIT_LAYER_PATH",
                     "VK_ADD_LAYER_PATH", "VK_LAYER_PATH", "VK_LOADER_LAYERS_ENABLE"):
            env.pop(name, None)
        # Its library aliases would shadow the drivers' own dependencies too.
        kept = [d for d in env.get("LD_LIBRARY_PATH", "").split(":")
                if d and "/pressure-vessel/" not in d]
        if kept:
            env["LD_LIBRARY_PATH"] = ":".join(kept)
        else:
            env.pop("LD_LIBRARY_PATH", None)
    # Both overrides belong to the user; VK_DRIVER_FILES takes precedence over
    # VK_ICD_FILENAMES. In particular don't replace an explicit Lavapipe setup.
    if env.get("VK_DRIVER_FILES") or env.get("VK_ICD_FILENAMES"):
        return "Vulkan: using explicit driver override"
    bundled = env.get("BB_BUNDLED_VK_DRIVER_FILES", "")
    if env.get("VK_ADD_DRIVER_FILES"):
        bundled = env["VK_ADD_DRIVER_FILES"] + (":" + bundled if bundled else "")
    extra = env.get("BB_NVIDIA_LIB_DIR")
    if extra:
        library_dirs = (Path(extra), *library_dirs)
    found = host_nvidia(manifest_dirs, library_dirs)
    if not found:
        if bundled:
            env["VK_DRIVER_FILES"] = bundled
        return "Vulkan: bundled AMD/Intel drivers; no accessible NVIDIA ICD found"

    data, driver = found
    # The driver version/path and mtime give each installed driver its own small
    # cache. exec preserves process/signal behavior; the cache survives launch.
    key = hashlib.sha256(f"{driver}:{driver.stat().st_mtime_ns}".encode()).hexdigest()[:16]
    data_dir = Path(env.get("BB_DATA_DIR") or
                    str(Path(env.get("XDG_DATA_HOME", str(Path.home() / ".local/share"))) / "bbport"))
    cache = data_dir / "vulkan" / "nvidia" / key
    libraries = cache / "lib"
    libraries.mkdir(parents=True, exist_ok=True)
    # NVIDIA dlopens versioned shader/compiler libraries at runtime, so expose
    # the driver's siblings as well as its ICD. Never link generic host libs.
    sources = [driver]
    # libcuda.so.1 is the driver's too: with DLSS loaded, NGX has the driver load it, and without it
    # vkCreateDevice fails (#135). The ICD's own directory first, then the host's library
    # directories: Debian puts the ICD in nvidia/current/ but libnvidia-glsi/-tls/-glcore one
    # level up (#205). The first file of each name wins.
    for directory in dict.fromkeys((driver.parent, *library_dirs)):
        for pattern in ("libnvidia-*.so*", "libGLX_nvidia.so*", "libEGL_nvidia.so*",
                        "libcuda.so*"):
            sources.extend(sorted(directory.glob(pattern)))
    names = set()
    for source in sources:
        if source.name in names or not elf64(source):
            continue
        names.add(source.name)
        link = libraries / source.name
        try:
            link.symlink_to(source.resolve())
        except FileExistsError:
            pass
    # Keep the manifest's API/architecture metadata, replace its library path.
    data["ICD"]["library_path"] = str(libraries / driver.name)
    manifest = cache / "nvidia_icd.json"
    temporary = cache / f"nvidia_icd.{os.getpid()}.tmp"
    temporary.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    temporary.replace(manifest)
    env["VK_DRIVER_FILES"] = str(manifest) + (":" + bundled if bundled else "")
    env["LD_LIBRARY_PATH"] = str(libraries) + (
        ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    # #107: NVIDIA's ICD gave no vkCreateInstance on some hosts (a GTX 1650 laptop on CachyOS)
    # until glvnd saw the host's EGL vendor files, NVIDIA's first; the package's glvnd only looks
    # in its own store. The user's own list, or BB_NVIDIA_EGL=0, keeps it as it is.
    if not env.get("__EGL_VENDOR_LIBRARY_FILENAMES") and env.get("BB_NVIDIA_EGL") != "0":
        vendors = [p for d in egl_dirs for p in sorted(d.glob("*.json"))]
        vendors.sort(key=lambda p: "nvidia" not in p.name.lower())
        if any("nvidia" in p.name.lower() for p in vendors):
            env["__EGL_VENDOR_LIBRARY_FILENAMES"] = ":".join(str(p) for p in vendors)
    return f"Vulkan: host NVIDIA driver {driver}; bundled AMD/Intel also available"


def nvidia_report(env, tool, run=subprocess.run):
    """--vulkan-info with the host NVIDIA driver: what the packaged loader got, for issue reports
    (#107: the driver loaded but gave no vkCreateInstance). The linked libraries, the kernel
    module, the device nodes, hybrid-graphics variables, and the dynamic linker's view of one
    vulkaninfo run: NVIDIA libraries it searched for and never loaded."""
    manifest = next((Path(p) for p in env.get("VK_DRIVER_FILES", "").split(":")
                     if "/vulkan/nvidia/" in p), None)
    if manifest is None:
        return []
    lines = [f"NVIDIA libraries linked in {manifest.parent / 'lib'}:"]
    try:
        links = sorted((manifest.parent / "lib").iterdir())
    except OSError as error:
        links = []
        lines.append(f"  cannot list: {error}")
    for link in links:
        target = os.readlink(link) if link.is_symlink() else "(file)"
        lines.append(f"  {link.name} -> {target}{'' if link.exists() else '  MISSING'}")
    try:
        module = Path("/proc/driver/nvidia/version").read_text(encoding="utf-8").splitlines()[0]
    except (OSError, IndexError):
        module = "not loaded (no /proc/driver/nvidia/version)"
    lines.append(f"Kernel module: {module}")
    nodes = sorted(Path("/dev").glob("nvidia*"))
    lines.append("Device nodes: " + (", ".join(
        f"{n.name}{'' if os.access(n, os.R_OK | os.W_OK) else ' (no access)'}" for n in nodes)
        or "none"))
    for key in ("__NV_PRIME_RENDER_OFFLOAD", "__VK_LAYER_NV_optimus", "__GLX_VENDOR_LIBRARY_NAME",
                "DRI_PRIME", "VK_LOADER_LAYERS_DISABLE"):
        if env.get(key):
            lines.append(f"{key}={env[key]}")
    try:
        result = run([tool, "--summary"], env={**env, "LD_DEBUG": "libs"},
                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, timeout=60)
        trace = result.stderr
    except (OSError, subprocess.SubprocessError) as error:
        lines.append(f"Dynamic linker trace: {tool} did not run ({error})")
        return lines
    searched, loaded = [], set()
    for line in trace.splitlines():
        if (at := line.find("find library=")) != -1:
            name = line[at + len("find library="):].split(" ", 1)[0]
            if name not in searched:
                searched.append(name)
        elif (at := line.find("calling init: ")) != -1:
            loaded.add(Path(line[at + len("calling init: "):].strip()).name)
    wanted = [n for n in searched if "nvidia" in n.lower() or "GLdispatch" in n]
    missing = [n for n in wanted if n not in loaded]
    lines.append(f"Dynamic linker: {len(wanted)} NVIDIA/glvnd libraries searched, "
                 + (f"not loaded: {', '.join(missing)}" if missing else "all loaded"))
    return lines


def main():
    if len(sys.argv) < 2:
        print("Usage: bbport_vulkan.py <program> [args...]", file=sys.stderr)
        return 1
    try:
        print(configure(os.environ), file=sys.stderr, flush=True)
    except OSError as error:
        # Startup diagnostics should identify driver/cache failures rather than
        # falling through to an unrelated Vulkan assertion.
        print(f"bbport: cannot prepare NVIDIA driver: {error}", file=sys.stderr)
        return 1
    if "--vulkan-info" in sys.argv[1:]:
        tool = os.environ.get("BB_VULKANINFO", "vulkaninfo")
        os.environ.setdefault("VK_LOADER_DEBUG", "error,warn,driver")
        # Everything on stdout, with the summary: one file for an issue report (#107: the report
        # went to stderr and was missing from saved output).
        for key in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES", "LD_LIBRARY_PATH",
                    "__EGL_VENDOR_LIBRARY_FILENAMES"):
            print(f"{key}={os.environ.get(key, '')}", flush=True)
        for line in nvidia_report(os.environ, tool):
            print(line, flush=True)
        os.execvpe(tool, [tool, "--summary"], os.environ)
    os.execvpe(sys.argv[1], sys.argv[1:], os.environ)


if __name__ == "__main__":
    sys.exit(main())
