// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: MIT
//
// C interface between shadPS4 and shadps4_dlss.dll. The emulator contains no NVIDIA code: it
// loads the bridge at runtime when present, and keeps its normal rendering when it is absent.

#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHADPS4_DLSS_BRIDGE_ABI 2

typedef void (*ShadDlssLogFn)(int warning, const char* message);

typedef struct ShadDlssImage {
    VkImage image;
    VkImageView view;
    VkImageSubresourceRange range;
    VkFormat format;
    uint32_t width;
    uint32_t height;
} ShadDlssImage;

typedef struct ShadDlssFeature {
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    int32_t quality; // 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance
    int32_t depth_inverted;
    uint32_t preset; // NVSDK_NGX_DLSS_Hint_Render_Preset value, 0 = driver default
    int32_t hdr;     // 1: linear HDR colour with automatic exposure, 0: LDR
} ShadDlssFeature;

typedef struct ShadDlssEvaluate {
    ShadDlssImage color;  // sampled, LDR
    ShadDlssImage depth;  // sampled, hardware depth
    ShadDlssImage motion; // sampled, render-resolution pixels, current to previous
    ShadDlssImage output; // storage, general layout
    float jitter_x;
    float jitter_y;
    int32_t reset;
    float frame_ms;
} ShadDlssEvaluate;

typedef struct ShadDlssApi {
    uint32_t abi;
    // Directories are UTF-16 paths: where nvngx_dlss.dll lives, and a writable data directory.
    int32_t (*Configure)(const wchar_t* dll_directory, const wchar_t* data_directory,
                         ShadDlssLogFn log);
    // Extensions NGX needs. The arrays stay valid until Shutdown.
    int32_t (*InstanceExtensions)(uint32_t* count, const VkExtensionProperties** extensions);
    int32_t (*DeviceExtensions)(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                                const VkExtensionProperties** extensions);
    int32_t (*Initialize)(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                          PFN_vkGetInstanceProcAddr get_instance_proc,
                          PFN_vkGetDeviceProcAddr get_device_proc);
    int32_t (*CreateFeature)(VkCommandBuffer command, const ShadDlssFeature* feature);
    int32_t (*Evaluate)(VkCommandBuffer command, const ShadDlssEvaluate* evaluate);
    // The caller must have drained the GPU.
    void (*ReleaseFeature)(void);
    void (*Shutdown)(void);
} ShadDlssApi;

typedef const ShadDlssApi* (*ShadDlssGetApiFn)(void);

#ifdef __cplusplus
}
#endif
