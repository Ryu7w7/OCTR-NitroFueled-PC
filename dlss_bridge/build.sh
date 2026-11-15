#!/usr/bin/env bash
# Native Linux NGX bridge; NVIDIA SDK libraries stay outside the GPL game library.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
sdk=${DLSS_SDK:?Set DLSS_SDK to the NVIDIA DLSS SDK directory}
out=${DLSS_OUT:-out/dlss}
mkdir -p "$out"
"${CXX:-c++}" -std=c++20 -O2 -fPIC -fvisibility=hidden -shared \
    -I"$sdk/include" ${VULKAN_CFLAGS:-} dlss_bridge/shadps4_dlss_bridge.cpp \
    "$sdk/lib/Linux_x86_64/libnvsdk_ngx.a" -ldl -pthread -Wl,-z,defs \
    -o "$out/libsacredfire_dlss.so"
runtime=$(find "$sdk/lib/Linux_x86_64/rel" -maxdepth 1 -name 'libnvidia-ngx-dlss.so.*' -type f | sort -V | tail -1)
[[ -n $runtime ]] || { echo 'NVIDIA DLSS runtime is missing' >&2; exit 1; }
cp "$runtime" "$out/"
ln -sfn "$(basename "$runtime")" "$out/libnvidia-ngx-dlss.so"
cp "$sdk/LICENSE.txt" "$out/NVIDIA-DLSS-SDK-LICENSE.txt"
cp dlss_bridge/LICENSE.txt "$out/dlss-bridge-LICENSE.txt"
