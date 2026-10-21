// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later
// SacredFire: from IFreemz/shadPS4-Bloodborne-DLSS-FSR (vk_dlss_ngx); logging to stdout (the
// port's log), one process-wide instance.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>

#include "video_core/renderer_vulkan/vk_dlss_ngx.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../../../../dlss_bridge/shadps4_dlss_bridge.h"
#endif

namespace Vulkan {

namespace {
constexpr wchar_t BridgeName[] = L"sacredfire_dlss.dll";
constexpr wchar_t NgxName[] = L"nvngx_dlss.dll";

std::filesystem::path ExecutableDirectory() {
#ifdef _WIN32
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
#else
    return std::filesystem::current_path();
#endif
}
} // namespace

int DlssNgx::QualityForScale(float scale) {
    // DLAA only for equal sizes: any real upscale needs at least the Quality mode.
    return scale >= 2.9f ? 4 : scale >= 1.95f ? 3 : scale >= 1.65f ? 2 : scale > 1.01f ? 1 : 0;
}

#ifdef _WIN32

namespace {
void BridgeLog(int warning, const char* message) {
    std::printf("DLSS%s: %s\n", warning ? " (warning)" : "", message);
    std::fflush(stdout);
}

std::vector<const char*> Collect(const VkExtensionProperties* required, u32 count,
                                 const std::vector<vk::ExtensionProperties>& available,
                                 std::vector<std::string>& storage, bool& ok) {
    ok = true;
    storage.clear();
    for (u32 i = 0; i < count; ++i) {
        const auto& request = required[i];
        const auto match = std::ranges::find_if(available, [&](const auto& extension) {
            return std::strcmp(extension.extensionName.data(), request.extensionName) == 0 &&
                   extension.specVersion >= request.specVersion;
        });
        if (match == available.end()) {
            std::printf("DLSS: missing Vulkan extension %s\n", request.extensionName);
            ok = false;
            return {};
        }
        storage.emplace_back(request.extensionName);
    }
    std::vector<const char*> names;
    for (const auto& name : storage) {
        names.push_back(name.c_str());
    }
    return names;
}
} // namespace

struct DlssNgx::Impl {
    HMODULE module{};
    const ShadDlssApi* api{};
    std::vector<std::string> instance_extensions, device_extensions;
    std::optional<FeatureDesc> feature;
    std::string problem;
    bool eligible{true}, instance_ready{}, device_ready{}, initialized{}, available{};

    void Disable(std::string_view reason) {
        eligible = available = false;
        problem = std::string{reason};
        std::printf("DLSS: off (%s)\n", problem.c_str());
        std::fflush(stdout);
    }
};

DlssNgx::DlssNgx() : impl{std::make_unique<Impl>()} {
    const auto directory = ExecutableDirectory();
    impl->module = LoadLibraryW((directory / BridgeName).c_str());
    const auto get_api =
        impl->module
            ? reinterpret_cast<ShadDlssGetApiFn>(GetProcAddress(impl->module, "ShadDlssGetApi"))
            : nullptr;
    impl->api = get_api ? get_api() : nullptr;
    if (!impl->api || impl->api->abi != SHADPS4_DLSS_BRIDGE_ABI) {
        impl->api = nullptr;
        impl->Disable("sacredfire_dlss.dll is missing or from another version");
        return;
    }
    const auto data = directory / "dlss";
    std::error_code error;
    std::filesystem::create_directories(data, error);
    if (!impl->api->Configure(directory.c_str(), data.c_str(), BridgeLog)) {
        impl->Disable("the bridge could not be configured");
    }
}

DlssNgx* DlssNgx::Get() {
    static std::once_flag once;
    static DlssNgx* instance = nullptr;
    std::call_once(once, [] {
        const char* setting = std::getenv("BB_DLSS");
        if (setting && setting[0] == '0') {
            return;
        }
        const auto directory = ExecutableDirectory();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(directory / BridgeName, ec) ||
            !std::filesystem::is_regular_file(directory / NgxName, ec)) {
            return;
        }
        instance = new DlssNgx; // lives until the process ends; Shutdown() releases NGX
        std::printf("DLSS: bridge loaded\n");
    });
    return instance;
}

DlssNgx::~DlssNgx() {
    Shutdown();
}

const std::string& DlssNgx::Problem() const {
    return impl->problem;
}

void DlssNgx::AppendInstanceExtensions(std::vector<const char*>& enabled) {
    if (!impl->eligible) {
        return;
    }
    u32 count{};
    const VkExtensionProperties* required{};
    if (!impl->api->InstanceExtensions(&count, &required)) {
        return impl->Disable("instance extension query failed");
    }
    const auto [result, extensions] = vk::enumerateInstanceExtensionProperties();
    bool ok = result == vk::Result::eSuccess;
    const auto names = ok ? Collect(required, count, extensions, impl->instance_extensions, ok)
                          : std::vector<const char*>{};
    if (!ok) {
        return impl->Disable("required Vulkan instance extensions are missing");
    }
    for (const char* name : names) {
        if (std::ranges::none_of(enabled, [&](const char* e) { return std::strcmp(e, name) == 0; })) {
            enabled.push_back(name);
        }
    }
    impl->instance_ready = true;
}

std::vector<const char*> DlssNgx::DeviceExtensions(vk::Instance instance,
                                                   vk::PhysicalDevice physical,
                                                   const std::vector<std::string>& already) {
    if (!impl->eligible || !impl->instance_ready) {
        return {};
    }
    const auto properties = physical.getProperties();
    if (properties.vendorID != 0x10de) {
        impl->Disable("this is not an NVIDIA GPU");
        return {};
    }
    u32 count{};
    const VkExtensionProperties* required{};
    if (!impl->api->DeviceExtensions(instance, physical, &count, &required)) {
        impl->Disable("this GPU or driver does not support DLSS (an RTX GPU is needed)");
        return {};
    }
    const auto [result, extensions] = physical.enumerateDeviceExtensionProperties();
    bool ok = result == vk::Result::eSuccess;
    auto names = ok ? Collect(required, count, extensions, impl->device_extensions, ok)
                    : std::vector<const char*>{};
    if (!ok) {
        impl->Disable("required Vulkan device extensions are missing");
        return {};
    }
    std::erase_if(names, [&](const char* name) {
        return std::ranges::find(already, std::string{name}) != already.end();
    });
    impl->device_ready = true;
    return names;
}

void DlssNgx::Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device) {
    if (!impl->eligible || !impl->device_ready || impl->initialized) {
        return;
    }
    impl->initialized = true;
    if (!impl->api->Initialize(instance, physical, device,
                               VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                               VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr)) {
        return impl->Disable("NVIDIA NGX initialization failed (update the GPU driver)");
    }
    impl->available = true;
    std::printf("DLSS: ready\n");
    std::fflush(stdout);
}

bool DlssNgx::IsAvailable() const {
    return impl->available;
}

bool DlssNgx::HasFeature(const FeatureDesc& desc) const {
    const auto& d = impl->feature;
    return d && d->input_width == desc.input_width && d->input_height == desc.input_height &&
           d->output_width == desc.output_width && d->output_height == desc.output_height &&
           d->quality == desc.quality && d->depth_inverted == desc.depth_inverted &&
           d->preset == desc.preset && d->hdr == desc.hdr;
}

bool DlssNgx::CreateFeature(vk::CommandBuffer command, const FeatureDesc& desc) {
    if (!impl->available) {
        return false;
    }
    impl->feature.reset();
    const ShadDlssFeature feature{desc.input_width,   desc.input_height, desc.output_width,
                                  desc.output_height, desc.quality,      desc.depth_inverted,
                                  desc.preset,        desc.hdr ? 1 : 0};
    if (!impl->api->CreateFeature(command, &feature)) {
        return false;
    }
    impl->feature = desc;
    return true;
}

bool DlssNgx::Evaluate(vk::CommandBuffer command, const Resource& color, const Resource& depth,
                       const Resource& motion, const Resource& output, const EvalDesc& eval) {
    if (!impl->available || !impl->feature) {
        return false;
    }
    const auto image = [](const Resource& r) {
        return ShadDlssImage{r.image,
                             r.view,
                             static_cast<VkImageSubresourceRange>(r.range),
                             static_cast<VkFormat>(r.format),
                             r.extent.width,
                             r.extent.height};
    };
    const ShadDlssEvaluate parameters{image(color),  image(depth),  image(motion), image(output),
                                      eval.jitter_x, eval.jitter_y, eval.reset,    eval.frame_ms};
    return impl->api->Evaluate(command, &parameters) != 0;
}

void DlssNgx::ReleaseFeatureAfterGpuDrain() {
    impl->feature.reset();
    if (impl->api) {
        impl->api->ReleaseFeature();
    }
}

void DlssNgx::Shutdown() {
    impl->available = false;
    impl->feature.reset();
    if (impl->api && impl->initialized) {
        impl->api->Shutdown();
        impl->initialized = false;
    }
}

#else

struct DlssNgx::Impl {
    std::string problem{"DLSS is Windows only"};
};
DlssNgx::DlssNgx() = default;
DlssNgx::~DlssNgx() = default;
DlssNgx* DlssNgx::Get() {
    return nullptr;
}
const std::string& DlssNgx::Problem() const {
    return impl->problem;
}
void DlssNgx::AppendInstanceExtensions(std::vector<const char*>&) {}
std::vector<const char*> DlssNgx::DeviceExtensions(vk::Instance, vk::PhysicalDevice,
                                                   const std::vector<std::string>&) {
    return {};
}
void DlssNgx::Initialize(vk::Instance, vk::PhysicalDevice, vk::Device) {}
void DlssNgx::Shutdown() {}
bool DlssNgx::IsAvailable() const {
    return false;
}
bool DlssNgx::CreateFeature(vk::CommandBuffer, const FeatureDesc&) {
    return false;
}
bool DlssNgx::HasFeature(const FeatureDesc&) const {
    return false;
}
bool DlssNgx::Evaluate(vk::CommandBuffer, const Resource&, const Resource&, const Resource&,
                       const Resource&, const EvalDesc&) {
    return false;
}
void DlssNgx::ReleaseFeatureAfterGpuDrain() {}

#endif

} // namespace Vulkan
