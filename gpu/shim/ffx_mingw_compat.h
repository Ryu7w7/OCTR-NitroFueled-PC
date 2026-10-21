// SPDX-License-Identifier: MIT
// SacredFire (Windows, MinGW): AMD's FidelityFX 1.1.4 host sources take MSVC's CRT for granted
// on _WIN32 (_countof, swprintf_s) and FSR-Vulkan's portability header covers non-Windows only.
// Force-included into those targets by gpu/CMakeLists.txt.
#pragma once
#include <stdlib.h>
#include <stdio.h>
#include <wchar.h>
#ifdef __cplusplus
#include <cmath>
#include <new>
#include <mutex>
#else
#include <math.h>
#endif
#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif
