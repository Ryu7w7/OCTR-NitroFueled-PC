"""Milestone 2, shader step: the game's GCN shaders -> HLSL (the D3D11 port's gcn2hlsl) -> SPIR-V (DXC).

Runs the D3D11 port's tools (github.com/straightarrows/CTR-NitroFueled-PC-d3d11, tools/) against a
CUSA13795 dump, keeping every intermediate so the Vulkan renderer and failures can be inspected:

  <out>/igz/      shader .igz files from archives/shaders_ps4.pak
  <out>/gcn/      <effect>_<hash>_<kind>.bin + index.json (export_shaders.py)
  <out>/hlsl/     .hlsl, .dxbc, shaders.idx, results.json (gcn2hlsl.py, compiled with fxc as the port does)
  <out>/spirv/    .spv per shader + spirv.json (DXC -spirv; the Vulkan HLSL is kept only for failures)

shaders_ps4.pak is byte-identical in 1.00 and 1.21. The few shaders the game keeps in eboot.bin
(runtime_shaders.json) sit at other offsets in 1.21, so they are found by their OrbShdr footer and
checked against the hash the port's table records.

Vulkan bindings (set 0): b# -> # + B_SHIFT, t# -> # + T_SHIFT, s# -> # + S_SHIFT, u# -> # + U_SHIFT.

usage: python build_spirv.py <d3d11 port tools dir> <game dir> <out dir> [--dxc path] [--only substr]
"""
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time
from collections import Counter

B_SHIFT, T_SHIFT, S_SHIFT, U_SHIFT = 0, 64, 128, 192
PROFILES = {"vs": "vs_6_0", "es": "vs_6_0", "gs": "gs_6_0", "ps": "ps_6_0"}


def find_runtime_shaders(tools, eboot, gcn_dir):
    """cut the eboot-resident shaders out of this eboot.bin, wherever they moved to"""
    import gcn2hlsl
    boot = open(eboot, "rb").read()
    table = json.load(open(os.path.join(tools, "runtime_shaders.json")))
    footers = []
    p = boot.find(b"OrbShdr")
    while p >= 0:
        footers.append(p)
        p = boot.find(b"OrbShdr", p + 1)
    index_path = os.path.join(gcn_dir, "index.json")
    index = json.load(open(index_path))
    have = {e["name"] for e in index}
    found = 0
    for t in table:
        e = t["entry"]
        want = e["name"].split("_")[1]
        code = None
        start = t["eboot_offset"]
        if f"{gcn2hlsl.fnv1a64(boot[start:start + e['size']]):016x}" == want:
            code = boot[start:start + e["size"]]
        else:
            # the footer lies inside the shader's bytes: try each footer at each spot it could take
            for f in footers:
                for rel in range(max(0, e["size"] - 64), e["size"] - 6):
                    s = f - rel
                    if s >= 0 and f"{gcn2hlsl.fnv1a64(boot[s:s + e['size']]):016x}" == want:
                        code, start = boot[s:s + e["size"]], s
                        break
                if code:
                    break
        if code is None:
            print(f"  {e['name']}: not in this eboot.bin")
            continue
        if e["name"] not in have:
            index.append(e)
        open(os.path.join(gcn_dir, e["name"] + ".bin"), "wb").write(code)
        print(f"  {e['name']}: eboot.bin+{start:#x}")
        found += 1
    json.dump(index, open(index_path, "w"), indent=1)
    return found


# DXC numbers stage inputs/outputs in declaration order, and a stage declares only the varyings it
# uses, so pin them: VARYINGn -> location n-15 (gcn2hlsl's VARYINGS start at 15), ATTRIBn -> n.
IO_DECL = re.compile(r"^(\s*)([a-z ]*float4 a\d+ : (VARYING|ATTRIB)(\d+);)", re.M)


def pin_locations(text):
    def loc(m):
        n = int(m.group(4)) - (15 if m.group(3) == "VARYING" else 0)
        return f"{m.group(1)}[[vk::location({n})]] {m.group(2).lstrip()}"
    return IO_DECL.sub(loc, text)


def dxc_one(dxc, hlsl_dir, spv_dir, name, kind):
    src = os.path.join(spv_dir, name + ".hlsl")
    with open(src, "w") as f:
        f.write(pin_locations(open(os.path.join(hlsl_dir, name + ".hlsl")).read()))
    dst = os.path.join(spv_dir, name + ".spv")
    cmd = [dxc, "-spirv", "-T", PROFILES[kind], "-E", "main", "-HV", "2018", "-fspv-target-env=vulkan1.1",
           "-fvk-b-shift", str(B_SHIFT), "0", "-fvk-t-shift", str(T_SHIFT), "0",
           "-fvk-s-shift", str(S_SHIFT), "0", "-fvk-u-shift", str(U_SHIFT), "0",
           "-O3", "-Fo", dst, src]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if r.returncode == 0:
        os.remove(src)
        return name, True, ""
    err = next((l for l in r.stderr.splitlines() if "error" in l), r.stderr.strip()[:200])
    return name, False, err


def main(argv):
    opts = {}
    for flag in ("--dxc", "--only"):
        if flag in argv:
            i = argv.index(flag)
            opts[flag] = argv[i + 1]
            argv = argv[:i] + argv[i + 2:]
    tools, game, out = (os.path.abspath(a) for a in argv[:3])
    sdk = os.environ.get("VULKAN_SDK", "")
    dxc = opts.get("--dxc") or (os.path.join(sdk, "Bin", "dxc.exe") if sdk else "dxc")
    only = opts.get("--only")
    sys.path.insert(0, tools)
    import export_shaders
    from setup_shaders import extract_shader_pak

    igz_dir, gcn_dir, hlsl_dir, spv_dir = (os.path.join(out, d) for d in ("igz", "gcn", "hlsl", "spirv"))
    for d in (igz_dir, gcn_dir, hlsl_dir, spv_dir):
        os.makedirs(d, exist_ok=True)
    t0 = time.time()
    if not os.path.exists(os.path.join(gcn_dir, "index.json")):
        print("1/4 extracting archives/shaders_ps4.pak", flush=True)
        print(f"    {extract_shader_pak(os.path.join(game, 'archives', 'shaders_ps4.pak'), igz_dir)} files")
        print("2/4 exporting the GCN programs", flush=True)
        export_shaders.main(igz_dir, gcn_dir)
        print(f"    {find_runtime_shaders(tools, os.path.join(game, 'eboot.bin'), gcn_dir)} from eboot.bin")
    else:
        print("1-2/4 reusing", gcn_dir)
    if only or not os.path.exists(os.path.join(hlsl_dir, "results.json")):
        print("3/4 GCN -> HLSL (gcn2hlsl, checked with fxc)", flush=True)
        import gcn2hlsl
        gcn2hlsl.main(gcn_dir, hlsl_dir, only)
    else:
        print("3/4 reusing", hlsl_dir, "(delete it to translate again)")

    print("4/4 HLSL -> SPIR-V (dxc)", flush=True)
    index = json.load(open(os.path.join(gcn_dir, "index.json")))
    kinds = {e["name"]: e["kind"] for e in index}
    names = sorted(n[:-5] for n in os.listdir(hlsl_dir) if n.endswith(".hlsl") and (not only or only in n))
    results, errors = {}, Counter()
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        jobs = [pool.submit(dxc_one, dxc, hlsl_dir, spv_dir, n, kinds[n]) for n in names]
        for j in concurrent.futures.as_completed(jobs):
            name, ok, err = j.result()
            results[name] = {"ok": ok, "error": err} if not ok else {"ok": True}
            if not ok:
                errors[err.split("error: ")[-1][:100]] += 1
    json.dump(results, open(os.path.join(spv_dir, "spirv.json"), "w"), indent=1, sort_keys=True)
    good = sum(r["ok"] for r in results.values())
    by_kind = Counter(kinds[n] for n, r in results.items() if r["ok"])
    print(f"SPIR-V: {good}/{len(results)} compiled {dict(by_kind)}")
    print("dxc errors:", errors.most_common(15))
    print(f"Done in {time.time() - t0:.0f} s.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
