#!/usr/bin/env bash
# ryunet-bridge (eosbridge.c) for Windows and Linux, into out/eosbridge/.
#   bash eosbridge/build.sh            this platform (Windows: MSYS2 CLANG64)
#   bash eosbridge/build.sh linux      Linux x86-64 from Windows, through WSL (the gateway)
# EOS_SDK: the Epic Online Services SDK folder (the one with SDK/Include); it is not in the
# repository. The SDK library is loaded at run time and copied next to the binary.
# eosbridge/eosbridge.ini (the Epic product's ids, not in git), when there is one, is built into
# the binary (embed_settings.py): an official build ships no settings file. Without it the
# bridge reads an eosbridge.ini next to itself, as a test or a server's gateway does.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
sdk=${EOS_SDK:-}
if [[ -z $sdk ]]; then
    for guess in "$HOME"/Downloads/EOS-SDK-* /c/Users/*/Downloads/EOS-SDK-* /mnt/c/Users/*/Downloads/EOS-SDK-*; do
        [[ -d $guess/SDK/Include ]] && sdk=$guess
    done
fi
[[ -d $sdk/SDK/Include ]] || { echo 'Set EOS_SDK to the EOS SDK folder (it has SDK/Include).' >&2; exit 1; }
mkdir -p out/eosbridge
flags=(-std=gnu11 -O2 -g -Wall -Wextra -Wno-missing-field-initializers -I"$sdk/SDK/Include")
target=${1:-}
embed() {
    rm -f out/eosbridge/eos_embedded.h
    [[ -f eosbridge/eosbridge.ini ]] || { echo 'No eosbridge/eosbridge.ini: nothing built in (the bridge reads one next to it)'; return; }
    "$(command -v python3 || command -v python)" eosbridge/embed_settings.py eosbridge/eosbridge.ini out/eosbridge/eos_embedded.h
    flags+=(-Iout/eosbridge '-DEOS_EMBEDDED="eos_embedded.h"')
}
if [[ $target == linux ]]; then
    case $(uname -s) in
    MINGW*|MSYS*)
        # Built in WSL; glibc 2.34 or later is enough (the test VPS has Ubuntu 22.04, 2.35):
        # C11 (not C23, whose strtol needs 2.38) and no fortify (its _chk functions are newer).
        here=$(cygpath -m "$PWD")
        there=$(cygpath -m "$sdk")
        exec wsl -- bash -lc "cd \"\$(wslpath '$here')\" && EOS_SDK=\"\$(wslpath '$there')\" bash eosbridge/build.sh linux"
        ;;
    esac
    embed
    gcc "${flags[@]}" -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector eosbridge/eosbridge.c -ldl \
        -o out/eosbridge/ryunet-bridge
    cp "$sdk/SDK/Bin/libEOSSDK-Linux-Shipping.so" out/eosbridge/
    need=$(objdump -T out/eosbridge/ryunet-bridge | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1)
    echo "Built out/eosbridge/ryunet-bridge (needs $need)"
    exit 0
fi
case $(uname -s) in
MINGW*|MSYS*)
    # shellcheck source=../msys2-env.sh
    source ./msys2-env.sh
    embed
    clang "${flags[@]}" -D_WIN32_WINNT=0x0601 eosbridge/eosbridge.c -lws2_32 -o out/eosbridge/ryunet-bridge.exe
    cp "$sdk/SDK/Bin/EOSSDK-Win64-Shipping.dll" out/eosbridge/
    echo 'Built out/eosbridge/ryunet-bridge.exe'
    ;;
*)
    embed
    "${CC:-cc}" "${flags[@]}" eosbridge/eosbridge.c -ldl -o out/eosbridge/ryunet-bridge
    lib=libEOSSDK-Linux-Shipping.so
    [[ $(uname -m) == aarch64 ]] && lib=libEOSSDK-LinuxArm64-Shipping.so   # an ARM server
    cp "$sdk/SDK/Bin/$lib" out/eosbridge/
    echo 'Built out/eosbridge/ryunet-bridge'
    ;;
esac
