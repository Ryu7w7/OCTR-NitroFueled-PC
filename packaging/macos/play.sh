#!/bin/bash
# SacredFire on macOS (Apple silicon): builds if needed and starts the game, as run.sh does on
# Linux. run.sh needs Bash 4+ (macOS ships 3.2): Homebrew's bash runs it. The game executable is
# x86-64, so macOS starts it under Rosetta 2 by itself.
# Usage: BB_GAME_DIR=/path/to/CUSA13795-1.21 bash packaging/macos/play.sh
cd "$(dirname "$0")/../.." || exit 1
for brew in /opt/homebrew/bin/brew /usr/local/bin/brew; do [[ -x $brew ]] && break; done
[[ -x $brew ]] || { echo "Need Homebrew: see packaging/macos/README.md." >&2; exit 1; }
eval "$("$brew" shellenv)"
bash5=$("$brew" --prefix)/bin/bash
[[ -x $bash5 ]] || "$brew" install bash || exit 1
# MoltenVK from the LunarG SDK (the one build.sh uses), unless VK_DRIVER_FILES is set.
if [[ -z ${VULKAN_SDK:-} && -f out/VULKAN_SDK ]]; then VULKAN_SDK=$(cat out/VULKAN_SDK); fi
if [[ -z ${VULKAN_SDK:-} ]]; then
    for sdk in "$HOME"/VulkanSDK/*/macOS /Applications/VulkanSDK/*/macOS; do
        [[ -f $sdk/lib/libvulkan.dylib ]] && VULKAN_SDK=$sdk
    done
fi
[[ -n ${VULKAN_SDK:-} ]] && export VULKAN_SDK
# The Vulkan driver: MoltenVK by default; BB_VK_DRIVER=kosmickrisp picks LunarG's Mesa-based
# KosmicKrisp from the same SDK (the buffer cache needs sparse binding, which MoltenVK lacks).
if [[ -z ${VK_DRIVER_FILES:-} && -n ${VULKAN_SDK:-} ]]; then
    case ${BB_VK_DRIVER:-moltenvk} in
    kosmickrisp) icd=$(find "$VULKAN_SDK" -name 'libkosmickrisp_icd.json' -print -quit 2>/dev/null) ;;
    *) icd=$(find "$VULKAN_SDK" -name 'MoltenVK_icd.json' -print -quit 2>/dev/null) ;;
    esac
    if [[ -n $icd ]]; then
        export VK_DRIVER_FILES=$icd
        echo "Vulkan driver: $icd"
    fi
fi
# A Python 3 whose XML parser loads: Homebrew's 3.14 on macOS 26.2 links an older system libexpat
# (Symbol not found: _XML_SetAllocTrackerActivationThreshold) and patches.py fails on ET.parse.
if [[ -z ${PYTHON:-} ]]; then
    for candidate in "$(command -v python3 || true)" /Library/Frameworks/Python.framework/Versions/*/bin/python3                      /usr/bin/python3; do
        [[ -x $candidate ]] || continue
        if "$candidate" -c 'import xml.etree.ElementTree as e; e.fromstring("<a/>")' 2>/dev/null; then
            export PYTHON=$candidate
            break
        fi
    done
    [[ -n ${PYTHON:-} ]] || { echo "Need a Python 3 with a working XML parser (see packaging/macos/README.md)." >&2; exit 1; }
fi
exec "$bash5" run.sh "$@"
