"""After a gcn2hlsl fix: translate every shader again (without fxc's slow D3D11 check), take only the
ones that changed into <shaders>/hlsl and recompile those with DXC (apply_retranslation.py), and
remove the temporary folder.

usage: python retranslate.py <d3d11 port tools dir> <shaders dir> <dxc.exe> <tag> [--only substr]
"""
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apply_retranslation  # noqa: E402


def main(tools, shaders, dxc, tag, only=None):
    os.environ["GCN2HLSL_NO_FXC"] = "1"
    sys.path.insert(0, tools)
    import gcn2hlsl
    t0 = time.time()
    tmp = os.path.join(shaders, "hlsl-" + tag)
    gcn2hlsl.main(os.path.join(shaders, "gcn"), tmp, only)
    t1 = time.time()
    print(f"translated in {t1 - t0:.0f} s", flush=True)
    rc = apply_retranslation.main(shaders, "hlsl-" + tag, tag, dxc, False)
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"applied in {time.time() - t1:.0f} s", flush=True)
    return rc


if __name__ == "__main__":
    args = sys.argv[1:]
    only = None
    if "--only" in args:
        i = args.index("--only")
        only = args[i + 1]
        args = args[:i] + args[i + 2:]
    sys.exit(main(*args[:4], only=only))
