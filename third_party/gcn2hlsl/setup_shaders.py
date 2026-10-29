"""One-time setup for a CrashTeamRacingNitroFueled.exe release: builds the files the exe needs from
the player's own game dump, so that the release itself carries nothing of the game. The release exe
carries this script with a minimal Python runtime (tools/pack_setup.py) and runs it on the first
start (port/src/first_start.cpp) with --out %LOCALAPPDATA%\\CTRNF.

  shaders.bin       the game's GCN shaders (archives/shaders_ps4.pak, plus a few that live in
                    eboot.bin) translated to D3D11 (gcn2hlsl.py) and packed (pack_shaders.py)
  glyphs_xbox.rgba  Xbox button glyphs, made from the Crash Bandicoot N. Sane Trilogy PC files when
                    they are installed (otherwise the game shows its PlayStation glyphs with a pad)

Both go next to eboot.bin unless --out says otherwise. Needs Python 3.8+ (standard library only) on
Windows 10/11.

usage: python setup_shaders.py [game dir] [--nsane <N. Sane Trilogy dir>] [--out <dir>]
"""
import json
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import export_shaders  # noqa: E402
import gcn2hlsl  # noqa: E402
import pack_shaders  # noqa: E402
from iga import Iga  # noqa: E402

NSANE_FOLDER = "Crash Bandicoot - N Sane Trilogy"


def find_game_dir(arg):
    candidates = [arg] if arg else []
    here = os.path.dirname(os.path.abspath(__file__))
    candidates += [os.getcwd(), here, os.path.dirname(here)]
    for c in candidates:
        if c and os.path.isfile(os.path.join(c, "eboot.bin")):
            return os.path.abspath(c)
    return None


def steam_libraries():
    roots = []
    for env in ("ProgramFiles(x86)", "ProgramFiles"):
        if os.environ.get(env):
            roots.append(os.path.join(os.environ[env], "Steam"))
    libs = []
    for root in roots:
        vdf = os.path.join(root, "steamapps", "libraryfolders.vdf")
        if os.path.isfile(vdf):
            libs.append(root)
            for path in re.findall(r'"path"\s+"([^"]+)"', open(vdf, encoding="utf-8", errors="replace").read()):
                libs.append(path.replace("\\\\", "\\"))
    return libs


def find_nsane(arg):
    if arg:
        return arg
    for lib in steam_libraries():
        d = os.path.join(lib, "steamapps", "common", NSANE_FOLDER)
        if os.path.isfile(os.path.join(d, "archives", "essentialui.pak")):
            return d
    return None


def extract_shader_pak(pak, out_dir):
    a = Iga(pak)
    n = 0
    for i, (logical, _phys) in enumerate(a.names):
        if not logical.lower().endswith(".igz"):
            continue
        rel = logical.replace("\\", "/").lstrip("/").replace(":", "_")
        dst = os.path.realpath(os.path.join(out_dir, rel))
        if not dst.startswith(os.path.realpath(out_dir) + os.sep):
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(a.read(i))
        n += 1
    return n


def add_runtime_shaders(game_dir, shaders_dir):
    """Shaders the game keeps in eboot.bin (video playback and a few more): cut out of the player's
    eboot.bin at known offsets, checked against their hashes."""
    boot = open(os.path.join(game_dir, "eboot.bin"), "rb").read()
    table = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "runtime_shaders.json")))
    index_path = os.path.join(shaders_dir, "index.json")
    index = json.load(open(index_path))
    have = {e["name"] for e in index}
    added = 0
    for t in table:
        e = t["entry"]
        code = boot[t["eboot_offset"]:t["eboot_offset"] + e["size"]]
        if f"{gcn2hlsl.fnv1a64(code):016x}" != e["name"].split("_")[1]:
            print(f"  {e['name']}: not found in this eboot.bin (a different game version?)")
            continue
        if e["name"] not in have:
            index.append(e)
        with open(os.path.join(shaders_dir, e["name"] + ".bin"), "wb") as f:
            f.write(code)
        added += 1
    json.dump(index, open(index_path, "w"), indent=1)
    return added


def main(argv):
    opts = {}
    for flag in ("--nsane", "--out"):
        if flag in argv:
            i = argv.index(flag)
            opts[flag] = argv[i + 1] if i + 1 < len(argv) else None
            argv = argv[:i] + argv[i + 2:]
    nsane_arg = opts.get("--nsane")
    game_dir = find_game_dir(argv[0] if argv else None)
    if not game_dir:
        print("Could not find the game: run this next to eboot.bin, or pass the game folder.")
        return 1
    out_dir = opts.get("--out") or game_dir  # where shaders.bin / glyphs_xbox.rgba go
    os.makedirs(out_dir, exist_ok=True)
    pak = os.path.join(game_dir, "archives", "shaders_ps4.pak")
    if not os.path.isfile(pak):
        print(f"{pak} is missing: is this a complete CUSA13795 dump?")
        return 1
    print(f"Game: {game_dir}")
    t0 = time.time()
    work = tempfile.mkdtemp(prefix="ctrnf_setup_")
    try:
        igz_dir, shaders_dir, hlsl_dir = (os.path.join(work, d) for d in ("igz", "shaders", "hlsl"))
        for d in (igz_dir, shaders_dir, hlsl_dir):
            os.makedirs(d)
        print("1/4 reading archives/shaders_ps4.pak ...", flush=True)
        print(f"    {extract_shader_pak(pak, igz_dir)} shader files")
        print("2/4 collecting the shaders ...", flush=True)
        export_shaders.main(igz_dir, shaders_dir)
        print(f"    {add_runtime_shaders(game_dir, shaders_dir)} shaders from eboot.bin")
        print("3/4 translating the shaders to D3D11 (takes a few minutes) ...", flush=True)
        gcn2hlsl.main(shaders_dir, hlsl_dir)
        print("4/4 packing ...", flush=True)
        pack_shaders.main(hlsl_dir, os.path.join(out_dir, "shaders.bin"))
    finally:
        shutil.rmtree(work, ignore_errors=True)

    nsane = find_nsane(nsane_arg)
    glyphs = os.path.join(out_dir, "glyphs_xbox.rgba")
    if nsane:
        import make_glyph_atlas
        tmp = tempfile.mkdtemp(prefix="ctrnf_glyphs_")
        try:
            out = os.path.join(tmp, "glyphs_xbox.rgba")
            make_glyph_atlas.main(game_dir, nsane, out)
            if os.path.getsize(out):
                shutil.copyfile(out, glyphs)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    else:
        print("Crash Bandicoot N. Sane Trilogy (PC) not found: no Xbox button glyphs, a controller shows the"
              " PlayStation ones (pass --nsane <folder> if it is installed elsewhere)")
    print(f"Done in {time.time() - t0:.0f} s.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
