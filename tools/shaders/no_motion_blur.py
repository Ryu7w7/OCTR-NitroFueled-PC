"""Lab option: no motion blur. The game's MotionBlur pixel shaders skip the blur where the largest
velocity near the pixel is at most half a pixel (and then only apply the colour-grading LUT); this
makes that test always pass, so the colours stay and the blur goes. The patched shaders go to
<shaders>/native/ (preferred over the translations), their HLSL to <shaders>/native-src/.

usage: python no_motion_blur.py <shaders dir> <dxc.exe>          (turn the blur off)
       python no_motion_blur.py <shaders dir> <dxc.exe> --undo   (back to the game's blur)
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_spirv  # noqa: E402

SHADERS = ["MotionBlur_21a0fd55_ps", "MotionBlur_ea382dda_ps"]
TEST = re.compile(r"vcc = ex && \(asfloat\(asuint\(0\.5f\)\) >= asfloat\(v\d+\)\);")


def main(shaders, dxc, undo):
    hlsl_dir, src_dir, native_dir = (os.path.join(shaders, d) for d in ("hlsl", "native-src", "native"))
    os.makedirs(src_dir, exist_ok=True)
    for name in SHADERS:
        spv = os.path.join(native_dir, name + ".spv")
        if undo:
            for p in (spv, os.path.join(src_dir, name + ".hlsl")):
                if os.path.exists(p):
                    os.remove(p)
            print(name, "back to the translation")
            continue
        text = open(os.path.join(hlsl_dir, name + ".hlsl")).read()
        found = TEST.findall(text)
        assert len(found) == 1, (name, found)
        text = TEST.sub("vcc = ex; // lab: no motion blur (always the no-blur path)", text, count=1)
        open(os.path.join(src_dir, name + ".hlsl"), "w").write(text)
        _, ok, err = build_spirv.dxc_one(dxc, src_dir, native_dir, name, "ps")
        print(name, "no blur" if ok else "FAILED " + err)
        if not ok:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2], "--undo" in sys.argv))
