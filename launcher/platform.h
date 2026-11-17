// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
using LauncherWindow = HWND;
using LauncherTexture = ID3D11ShaderResourceView;
using DragRect = RECT;
inline constexpr auto kGpuTool = L"bb-gpu-capabilities.exe";
inline constexpr unsigned kCancelKey = 0x01, kMenuKey = 0x152;
#else
#include <SDL3/SDL.h>
#include <ayatana-appindicator.h>
#include <libdbusmenu-glib/server.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <png.h>
#include <codecvt>
#include <locale>
#include "imgui_impl_sdl3.h"
#include "misc/freetype/imgui_freetype.h"
#include "imgui_impl_sdlrenderer3.h"
#include "linux/process.h"
using LauncherWindow = SDL_Window*;
using LauncherTexture = SDL_Texture;
using DWORD = unsigned long;
struct DragRect { int left, top, right, bottom; };
inline constexpr auto kGpuTool = "bb-gpu-capabilities";
inline constexpr unsigned kCancelKey = SDL_SCANCODE_ESCAPE, kMenuKey = SDL_SCANCODE_INSERT;
#endif

namespace {
uint64_t Ticks() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void SleepMs(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
std::string Env(const char* name) { const char* value = std::getenv(name); return value ? value : ""; }
int LocalTime(std::tm* out, const std::time_t* at) {
#ifdef _WIN32
    return localtime_s(out, at);
#else
    return localtime_r(at, out) ? 0 : -1;
#endif
}
bool RandomBytes(unsigned char* bytes, size_t size) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, bytes, ULONG(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    return RAND_bytes(bytes, int(size)) == 1;
#endif
}
bool ReplaceFile(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    return !ec;
#endif
}
bool CopyFile(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}
std::string LastPlatformError() {
#ifdef _WIN32
    return std::to_string(GetLastError());
#else
    return std::error_code(errno, std::generic_category()).message();
#endif
}
void ReleaseTexture(LauncherTexture* texture) {
    if (!texture) return;
#ifdef _WIN32
    texture->Release();
#else
    SDL_DestroyTexture(texture);
#endif
}
bool OpenUrl(const std::string& url);
void HideLauncher(bool hidden);
void BringLauncherToFront();
bool LauncherVisible();
void ToggleLauncher();
void ToggleMaximize();
void RequestClose();
void QuitLauncher();
}
