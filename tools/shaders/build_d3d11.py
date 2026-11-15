"""The Direct3D 11 engine's shaders: the game's GCN programs -> HLSL -> DXBC (fxc).

As build_spirv.py, with GCN2HLSL_D3D11=1: Direct3D 11 views have no channel order, so the shaders
keep their channel selects. Nothing derived from the game leaves <out>.

  <out>/igz/   shader .igz files from archives/shaders_ps4.pak
  <out>/gcn/   <effect>_<hash>_<kind>.bin + index.json
  <out>/hlsl/  .hlsl, .dxbc, shaders.idx, results.json

usage: python build_d3d11.py <game dir> <out dir> [--only substr] [--translate-only]

--translate-only (what run.py does at the first start): GCN -> HLSL only, about a minute; the engine
compiles each shader in the background the first time it is needed and keeps its .dxbc
(gpu/shim/d3d11/renderer.cpp CompileTranslation). fxc on every shader up front took ~30 minutes.
"""
import json
import os
import sys
import time

os.environ["GCN2HLSL_D3D11"] = "1"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(ROOT, "third_party", "gcn2hlsl")
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def main(argv):
    only = None
    if "--translate-only" in argv:
        argv = [a for a in argv if a != "--translate-only"]
        os.environ["GCN2HLSL_NO_FXC"] = "1"  # read by the workers (gcn2hlsl.work)
    if "--only" in argv:
        i = argv.index("--only")
        only = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    game, out = (os.path.abspath(a) for a in argv[:2])
    import build_spirv
    import export_shaders
    import gcn2hlsl
    from setup_shaders import extract_shader_pak

    igz_dir, gcn_dir, hlsl_dir = (os.path.join(out, d) for d in ("igz", "gcn", "hlsl"))
    for d in (igz_dir, gcn_dir, hlsl_dir):
        os.makedirs(d, exist_ok=True)
    t0 = time.time()
    if not os.path.exists(os.path.join(gcn_dir, "index.json")):
        print("1/3 extracting archives/shaders_ps4.pak", flush=True)
        print(f"    {extract_shader_pak(os.path.join(game, 'archives', 'shaders_ps4.pak'), igz_dir)} files")
        print("2/3 exporting the GCN programs", flush=True)
        export_shaders.main(igz_dir, gcn_dir)
        print(f"    {build_spirv.find_runtime_shaders(TOOLS, os.path.join(game, 'eboot.bin'), gcn_dir)} from eboot.bin")
    else:
        print("1-2/3 reusing", gcn_dir)
    print("3/3 GCN -> HLSL -> DXBC", flush=True)
    gcn2hlsl.main(gcn_dir, hlsl_dir, only)
    results = json.load(open(os.path.join(hlsl_dir, "results.json")))
    good = sum(1 for r in results.values() if r.get("ok"))
    print(f"{good} of {len(results)} shaders in {time.time() - t0:.0f} s")


if __name__ == "__main__":
    main(sys.argv[1:])
