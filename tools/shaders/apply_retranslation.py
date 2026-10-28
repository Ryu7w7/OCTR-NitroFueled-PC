"""Takes the shaders a fixed gcn2hlsl translates differently: compares <shaders>/<new>/*.hlsl with
<shaders>/hlsl/*.hlsl, copies the changed ones over, keeps their old .spv in <shaders>/spirv-<tag>-old/
and recompiles them with DXC the way build_spirv.py does.

usage: python apply_retranslation.py <shaders dir> <new hlsl dir name> <tag> <dxc.exe> [--dry]
"""
import concurrent.futures
import json
import os
import re
import shutil
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_spirv  # noqa: E402


HELPER = re.compile(r"^float3 cubedir\(float u, float v, float f\) \{.*?^\}\n", re.M | re.S)


def Strip(text):
    return HELPER.sub("", text)


def main(shaders, new, tag, dxc, dry):
    new_dir, hlsl_dir, spv_dir = (os.path.join(shaders, d) for d in (new, "hlsl", "spirv"))
    old_dir = os.path.join(shaders, f"spirv-{tag}-old")
    kinds = {e["name"]: e["kind"] for e in json.load(open(os.path.join(shaders, "gcn", "index.json")))}
    changed = []
    for f in sorted(os.listdir(new_dir)):
        if not f.endswith(".hlsl"):
            continue
        a, b = os.path.join(new_dir, f), os.path.join(hlsl_dir, f)
        # helpers every file carries (a new one alone changes nothing a shader does)
        if not os.path.exists(b) or Strip(open(a).read()) != Strip(open(b).read()):
            changed.append(f[:-5])
    print(f"{len(changed)} shaders changed")
    print("by effect:", Counter(n.rsplit("_", 2)[0] for n in changed).most_common(12))
    if dry:
        return 0
    os.makedirs(old_dir, exist_ok=True)
    for name in changed:
        spv = os.path.join(spv_dir, name + ".spv")
        if os.path.exists(spv) and not os.path.exists(os.path.join(old_dir, name + ".spv")):
            shutil.copy2(spv, old_dir)
        hl = os.path.join(hlsl_dir, name + ".hlsl")
        if os.path.exists(hl) and not os.path.exists(os.path.join(old_dir, name + ".hlsl")):
            shutil.copy2(hl, old_dir)
        shutil.copy2(os.path.join(new_dir, name + ".hlsl"), hl)
    failed = []
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        jobs = [pool.submit(build_spirv.dxc_one, dxc, hlsl_dir, spv_dir, n, kinds[n]) for n in changed]
        for j in concurrent.futures.as_completed(jobs):
            name, ok, err = j.result()
            if not ok:
                failed.append((name, err))
    print(f"compiled {len(changed) - len(failed)}/{len(changed)}")
    for name, err in failed[:20]:
        print("  failed:", name, err)
    return 1 if failed else 0


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--dry"]
    sys.exit(main(*args[:4], dry="--dry" in sys.argv))
