#!/usr/bin/env python3
"""Fetch the pinned NVIDIA SDK files used by the optional DLSS bridge."""
import argparse
from pathlib import Path
import urllib.request

REVISION = "9981c6350dc58c0ce21f024fd65e31e851c31689"
HEADERS = ("nvsdk_ngx.h", "nvsdk_ngx_defs.h", "nvsdk_ngx_defs_vk.h", "nvsdk_ngx_helpers.h",
           "nvsdk_ngx_helpers_vk.h", "nvsdk_ngx_helpers_d3d.h", "nvsdk_ngx_helpers_cuda.h",
           "nvsdk_ngx_params.h", "nvsdk_ngx_loader.h", "nvsdk_ngx_vk.h")


def files(platform):
    common = ["LICENSE.txt", *("include/" + name for name in HEADERS)]
    if platform == "linux":
        return common + ["lib/Linux_x86_64/libnvsdk_ngx.a",
                         "lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16"]
    if platform == "windows":
        return common + ["lib/Windows_x86_64/x64/nvsdk_ngx_s.lib",
                         "lib/Windows_x86_64/rel/nvngx_dlss.dll"]
    raise ValueError("Expected linux or windows")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=("linux", "windows"))
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    for name in files(args.platform):
        target = args.destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(f"https://raw.githubusercontent.com/NVIDIA/DLSS/{REVISION}/{name}", timeout=180) as reply:
            data = reply.read()
        if not data or data.startswith(b"version https://git-lfs.github.com/"):
            raise ValueError(f"SDK download is incomplete: {name}")
        target.write_bytes(data)
    (args.destination / "REVISION").write_text(REVISION + "\n")


if __name__ == "__main__":
    main()
