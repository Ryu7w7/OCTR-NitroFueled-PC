#!/usr/bin/env bash
# SacredFire: header-only dependencies of gpu/ that the host's package manager does not carry
# (magic_enum, VulkanMemoryAllocator, xbyak). Pinned tags are cloned into out/deps/src and
# installed with their own CMake into out/deps/prefix; build.sh adds that prefix to
# CMAKE_PREFIX_PATH. Idempotent: an installed package is skipped. DEPS_PREFIX overrides the prefix.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
prefix=${DEPS_PREFIX:-$PWD/out/deps/prefix}
src=$PWD/out/deps/src
mkdir -p "$prefix" "$src"
fetch_and_install() {
    local name=$1 url=$2 tag=$3 marker=$4; shift 4
    if [[ -e $prefix/$marker ]]; then echo "deps: $name present"; return; fi
    if [[ ! -d $src/$name/.git ]]; then
        git clone -q --depth 1 --branch "$tag" "$url" "$src/$name"
    fi
    cmake -S "$src/$name" -B "$src/$name/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" "$@" >/dev/null
    cmake --install "$src/$name/build" >/dev/null
    echo "deps: $name $tag installed"
}
fetch_and_install magic_enum https://github.com/Neargye/magic_enum.git v0.9.7 \
    include/magic_enum/magic_enum.hpp -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF -DMAGIC_ENUM_OPT_INSTALL=ON
fetch_and_install VulkanMemoryAllocator https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git v3.3.0 \
    include/vk_mem_alloc.h
fetch_and_install xbyak https://github.com/herumi/xbyak.git v7.30 include/xbyak/xbyak.h
echo "deps: prefix $prefix"
