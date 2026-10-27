#!/bin/bash
# OCTR macOS smoke test: builds smoke.c for x86-64, runs it under Rosetta 2 and writes report.txt.
# Usage: bash run.sh   (on a Mac with Apple silicon; needs Xcode command line tools and Rosetta 2)
cd "$(dirname "$0")" || exit 1
out=report.txt
{
    echo "== host"
    sw_vers
    uname -m
    sysctl -n machdep.cpu.brand_string hw.memsize 2>/dev/null
    if /usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null; then echo "Rosetta 2: installed"; else echo "Rosetta 2: MISSING"; fi
    xcrun clang --version 2>&1 | head -1

    echo "== build"
    rm -f smoke
    if ! xcrun clang -arch x86_64 -O1 -Wall -o smoke smoke.c 2>&1; then echo "BUILD FAILED"; fi

    echo "== run"
    if [ -x smoke ]; then ./smoke 2>&1; echo "exit code: $?"; fi

    echo "== vulkan"
    if command -v vulkaninfo >/dev/null 2>&1; then
        vulkaninfo --summary 2>&1 | grep -E "apiVersion|deviceName|driverName|driverInfo" | head -8
        vulkaninfo 2>&1 | grep -E "geometryShader|maxGeometryInputComponents" | head -4
    else
        echo "vulkaninfo not installed (optional: brew install vulkan-tools molten-vk)"
    fi
} 2>&1 | tee "$out"
echo
echo "Done: send $(pwd)/$out back."
