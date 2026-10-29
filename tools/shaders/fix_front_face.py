"""The PS front-face input as the hardware gives it: +1.0 / -1.0 (float), as shadPS4 does when
SPI_BARYC_CNTL.FRONT_FACE_ALL_BITS is 0. gcn2hlsl wrote the integer 1 / 0, so shaders that flip the
normal for two-sided materials (n * face) multiplied it by a denormal and lost that part of the normal
(Roo's Tubes floor plates and lights black/purple).

Patches <shaders>/hlsl/*.hlsl in place, keeps the old .spv in <shaders>/spirv-ff-old/ and recompiles
the patched ones with DXC, the same way build_spirv.py does.

usage: python fix_front_face.py <shaders dir> <dxc.exe>
"""
import concurrent.futures
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_spirv  # noqa: E402

OLD = "IN.ff ? 1u : 0u;"
NEW = "asuint(IN.ff ? 1.0f : -1.0f);"


def main(shaders, dxc):
    hlsl_dir, spv_dir = os.path.join(shaders, "hlsl"), os.path.join(shaders, "spirv")
    old_dir = os.path.join(shaders, "spirv-ff-old")
    os.makedirs(old_dir, exist_ok=True)
    kinds = {e["name"]: e["kind"] for e in json.load(open(os.path.join(shaders, "gcn", "index.json")))}
    names = []
    for f in sorted(os.listdir(hlsl_dir)):
        if not f.endswith(".hlsl"):
            continue
        p = os.path.join(hlsl_dir, f)
        text = open(p).read()
        if OLD not in text:
            continue
        open(p, "w").write(text.replace(OLD, NEW))
        name = f[:-5]
        spv = os.path.join(spv_dir, name + ".spv")
        if os.path.exists(spv) and not os.path.exists(os.path.join(old_dir, name + ".spv")):
            shutil.copy2(spv, old_dir)
        names.append(name)
    print(f"patched {len(names)} shaders")
    failed = []
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        jobs = [pool.submit(build_spirv.dxc_one, dxc, hlsl_dir, spv_dir, n, kinds[n]) for n in names]
        for j in concurrent.futures.as_completed(jobs):
            name, ok, err = j.result()
            if not ok:
                failed.append((name, err))
    print(f"compiled {len(names) - len(failed)}/{len(names)}")
    for name, err in failed[:20]:
        print("  failed:", name, err)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
