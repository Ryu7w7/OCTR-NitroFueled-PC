#!/usr/bin/env bash
# Compiles shaders written for the PC in place of translated ones (gpu/shim/native_shaders/
# <effect>_<hash>_<stage>.hlsl, same interface as the translation) into <shader dir>/native/,
# with the bindings and options build_spirv.py gives the translations; the renderer prefers
# them (BB_VK_NATIVE_SHADERS=0: the translations). For translations that render wrong: the
# driver's compiler already removes the translation's overhead (a hand-written Fur pixel shader
# measured the same GPU time as its translation, 0.31 ms per draw).
#
# usage: tools/shaders/build_native.sh [shader dir (default out/shaders)] [dxc (default
#        $VULKAN_SDK/Bin/dxc.exe, else dxc)]
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
out=${1:-$root/out/shaders}/native
dxc=${2:-${VULKAN_SDK:+$VULKAN_SDK/Bin/dxc.exe}}
dxc=${dxc:-dxc}
shopt -s nullglob
mkdir -p "$out"
for src in "$root"/gpu/shim/native_shaders/*.hlsl; do
    name=$(basename "$src" .hlsl)
    case $name in
    *_ps) profile=ps_6_0 ;;
    *_vs | *_es) profile=vs_6_0 ;;
    *_gs) profile=gs_6_0 ;;
    *) echo "skipped $name (no stage suffix)"; continue ;;
    esac
    "$dxc" -spirv -T "$profile" -E main -HV 2018 -fspv-target-env=vulkan1.1 \
        -fvk-b-shift 0 0 -fvk-t-shift 64 0 -fvk-s-shift 128 0 -fvk-u-shift 192 0 \
        -O3 -Fo "$out/$name.spv" "$src"
    echo "built $name"
done
