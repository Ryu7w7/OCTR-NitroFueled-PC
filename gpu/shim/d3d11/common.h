// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the little of FrankHUN88453's port/src/common.h the renderer uses.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#define SYSV __attribute__((sysv_abi))

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

// A frame capture (F11, renderer.cpp) also writes the lines to its folder's draws.txt.
extern FILE* g_log_mirror;
#define LOG(...)                                                                                    \
    (std::printf(__VA_ARGS__), std::putchar('\n'), std::fflush(stdout),                             \
     g_log_mirror ? (std::fprintf(g_log_mirror, __VA_ARGS__), std::fputc('\n', g_log_mirror)) : 0)
