// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: see bbport_d3d11.h.
#include "bbport_d3d11.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <mutex>
#include <wrl/client.h>

#include "bbport_hook.h"
#include "bbport_native_memory.h"
#include "bbport_overlay_d3d11.h"
#include "d3d11/gfx.h"
#include "d3d11/glue.h"

using Microsoft::WRL::ComPtr;
#endif

namespace BbD3d11 {

bool On() {
#ifdef _WIN32
    static const bool on = [] {
        const char* env = std::getenv("BB_D3D11");
        return env && env[0] == '1';
    }();
    return on;
#else
    return false;
#endif
}

#ifdef _WIN32
namespace {
constexpr int UnsupportedExitCode = 24; // run.py's UNSUPPORTED_GPU_CODE
constexpr float EmptyPicture[4] = {0.02f, 0.05f, 0.12f, 1.0f};

ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ComPtr<IDXGISwapChain1> swap_chain;
ComPtr<ID3D11RenderTargetView> target;
HWND window_handle = nullptr;
UINT width = 0, height = 0, swap_flags = 0, present_flags = 0;
std::uint64_t presents = 0;
std::uint64_t last_address = 0;
std::uint32_t last_snapshot = 0; // GfxSnapshot of last_address's flip, 0 none
const char* const shaders = std::getenv("BB_D3D11_SHADERS"); // Start runs before PatchImage

// Without shaders the work is only left out: the game runs behind an empty picture.
__attribute__((sysv_abi)) bool SkipClear(std::uint64_t, std::uint32_t, const float*, std::uint32_t, float) {
    return true;
}

__attribute__((sysv_abi)) bool SkipCopy(std::uint64_t, const std::uint32_t*, const std::uint32_t*, const std::int32_t*) {
    return true;
}

[[noreturn]] void Stop(const char* what, HRESULT result) {
    std::fprintf(stderr, "STOP: Direct3D 11: %s failed (0x%08lx)\n", what, (unsigned long)result);
    std::fflush(stderr);
    std::exit(UnsupportedExitCode);
}

void MakeTarget() {
    ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT result = swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(result)) {
        Stop("the swap chain's picture", result);
    }
    result = device->CreateRenderTargetView(back_buffer.Get(), nullptr, &target);
    if (FAILED(result)) {
        Stop("the render target", result);
    }
}

/// The swap chain follows the window, as the Vulkan swapchain does. A minimized window keeps it.
void FollowWindow() {
    RECT client{};
    if (!GetClientRect(window_handle, &client)) {
        return;
    }
    const UINT w = UINT(client.right - client.left), h = UINT(client.bottom - client.top);
    if (!w || !h || (w == width && h == height)) {
        return;
    }
    target.Reset();
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->Flush();
    const HRESULT result = swap_chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swap_flags);
    if (FAILED(result)) {
        Stop("resizing the swap chain", result);
    }
    width = w;
    height = h;
    MakeTarget();
}

/// The adapter's name and memory, in the line run.py and the performance reports read.
void LogAdapter(IDXGIAdapter* adapter, D3D_FEATURE_LEVEL level) {
    DXGI_ADAPTER_DESC description{};
    if (FAILED(adapter->GetDesc(&description))) {
        return;
    }
    char name[256] = "";
    WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, sizeof name, nullptr, nullptr);
    std::printf("GPU: %s, %llu MiB VRAM; Direct3D 11, feature level %x_%x\n", name,
                (unsigned long long)(description.DedicatedVideoMemory >> 20), (unsigned)level >> 12,
                ((unsigned)level >> 8) & 0xf);
}
} // namespace

void PatchImage(unsigned char* image, std::uint64_t size) {
    if (!On()) {
        return;
    }
    const std::uint64_t shift = BbHook::EditionShift(image, size);
    if (shift == BbHook::UnknownEdition) {
        std::fprintf(stderr, "STOP: Direct3D 11: not a USA or EU 1.21 executable\n");
        std::fflush(stderr);
        std::exit(UnsupportedExitCode);
    }
    GlueInstall(image, size);
    BbHook::Replace(image, size, BbHook::ClearFunction - shift, BbHook::ClearPrologue,
                    shaders ? reinterpret_cast<std::uint64_t>(&GfxClearHook) : reinterpret_cast<std::uint64_t>(&SkipClear),
                    "D3D11", "kClearRenderTarget");
    BbHook::Replace(image, size, BbHook::CopyFunction - shift, BbHook::CopyPrologue,
                    shaders ? reinterpret_cast<std::uint64_t>(&GfxCopyHook) : reinterpret_cast<std::uint64_t>(&SkipCopy),
                    "D3D11", "kCopyTexture");
}

void Start(void* window) {
    window_handle = static_cast<HWND>(window);
    // BB_RENDERDOC=<renderdoc.dll> (a lab aid): RenderDoc loaded before the device is made, so it
    // hooks Direct3D 11; F11 then captures one whole frame of the game (renderer.cpp GfxSnapshot)
    if (const wchar_t* rd = _wgetenv(L"BB_RENDERDOC"); rd && *rd && !GetModuleHandleW(L"renderdoc.dll")) {
        const HMODULE m = LoadLibraryW(rd);
        std::printf("D3D11: RenderDoc %s (F11 captures a frame)\n", m ? "loaded" : "not found at BB_RENDERDOC");
    }
    UINT device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (const char* debug = std::getenv("BB_D3D11_DEBUG"); debug && debug[0] == '1') {
        device_flags |= D3D11_CREATE_DEVICE_DEBUG;
    }
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level{};
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, device_flags, levels,
                                       UINT(std::size(levels)), D3D11_SDK_VERSION, &device, &level, &context);
    if (result == E_INVALIDARG) { // a runtime that does not know 11_1
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, device_flags, levels + 1, 1,
                                   D3D11_SDK_VERSION, &device, &level, &context);
    }
    if (FAILED(result)) {
        Stop("creating the device (feature level 11_0 is needed)", result);
    }

    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    result = device.As(&dxgi_device);
    if (SUCCEEDED(result)) {
        result = dxgi_device->GetAdapter(&adapter);
    }
    if (SUCCEEDED(result)) {
        result = adapter->GetParent(IID_PPV_ARGS(&factory));
    }
    if (FAILED(result)) {
        Stop("reaching the adapter", result);
    }
    LogAdapter(adapter.Get(), level);
    // Present runs outside GfxMutex, so that the game's draws never wait for the screen.
    ComPtr<ID3D11Multithread> protection;
    if (SUCCEEDED(context.As(&protection))) {
        protection->SetMultithreadProtected(TRUE);
    }

    // The game's own limiter paces the frames, so the swap chain must not wait for vblanks.
    ComPtr<IDXGIFactory5> factory5;
    BOOL tearing = FALSE;
    if (SUCCEEDED(factory.As(&factory5))) {
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
    }
    if (tearing) {
        swap_flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        present_flags = DXGI_PRESENT_ALLOW_TEARING;
    }
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.Flags = swap_flags;
    result = factory->CreateSwapChainForHwnd(device.Get(), window_handle, &description, nullptr, nullptr, &swap_chain);
    if (FAILED(result)) {
        Stop("creating the swap chain", result);
    }
    factory->MakeWindowAssociation(window_handle, DXGI_MWA_NO_ALT_ENTER); // SDL handles fullscreen
    swap_chain->GetDesc1(&description);
    width = description.Width;
    height = description.Height;
    MakeTarget();

    BbOverlay::InitD3d11(device.Get(), context.Get());
    BbMem::Install(); // no Rasterizer: as the native Vulkan renderer alone
    GfxStart(device.Get(), context.Get(), swap_chain.Get(), shaders);
    std::printf("D3D11: %s\n", shaders ? "renderer on" : "no shaders (BB_D3D11_SHADERS): the game runs behind an empty picture");
    std::fflush(stdout);
}

void SetGamma(float gamma) {
    GfxSetGamma(gamma);
}

void OnDraw(bool indexed, std::uint32_t count, std::uint64_t index_addr) {
    if (shaders) {
        GfxDraw(count, index_addr, indexed);
    }
}

void OnDrawOffset(std::uint32_t count, std::uint32_t index_offset) {
    if (shaders) {
        GfxDrawOffset(count, index_offset);
    }
}

void OnDrawUnsupported(const char* kind) {
    if (shaders) {
        GfxDrawUnsupported(kind);
    }
}

std::uint32_t Snapshot(std::uint64_t address) {
    return swap_chain && shaders ? GfxSnapshot(address) : 0;
}

void Present(std::uint64_t address, std::uint32_t snapshot) {
    if (!swap_chain) {
        return;
    }
    if (address) {
        last_address = address;
        last_snapshot = snapshot;
    }
    std::unique_lock lock{GfxMutex()};
    FollowWindow();
    ID3D11RenderTargetView* const view = target.Get();
    if (shaders) {
        GfxPresent(last_address, view, last_snapshot);
    } else {
        context->ClearRenderTargetView(view, EmptyPicture);
    }
    context->OMSetRenderTargets(1, &view, nullptr);
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    BbOverlay::RenderD3d11(width, height);
    GfxForgetState();
    lock.unlock();
    const auto present_start = std::chrono::steady_clock::now();
    const HRESULT result = swap_chain->Present(0, present_flags);
    GfxNotePresent(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - present_start).count());
    if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
        std::fprintf(stderr, "STOP: Direct3D 11: the GPU was lost (0x%08lx, reason 0x%08lx)\n", (unsigned long)result,
                     (unsigned long)device->GetDeviceRemovedReason());
        std::fflush(stderr);
        std::exit(23); // as Vulkan's device lost
    }
    if (++presents % 600 == 0) {
        std::printf("D3D11: %llu pictures presented\n", (unsigned long long)presents);
        std::fflush(stdout);
    }
}
#else
void PatchImage(unsigned char*, std::uint64_t) {}
void Start(void*) {}
void SetGamma(float) {}
void OnDraw(bool, std::uint32_t, std::uint64_t) {}
void OnDrawOffset(std::uint32_t, std::uint32_t) {}
void OnDrawUnsupported(const char*) {}
std::uint32_t Snapshot(std::uint64_t) {
    return 0;
}
void Present(std::uint64_t, std::uint32_t) {}
#endif

} // namespace BbD3d11
