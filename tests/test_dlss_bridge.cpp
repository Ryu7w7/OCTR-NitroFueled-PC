// Linux NGX bridge smoke check; a supported NVIDIA GPU also exercises real initialization.
#include "../dlss_bridge/shadps4_dlss_bridge.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

int main(int argc, char** argv) {
    assert(argc >= 2);
    const auto directory = std::filesystem::absolute(argv[1]);
    void* module = dlopen((directory / "libsacredfire_dlss.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module) std::fprintf(stderr, "%s\n", dlerror());
    assert(module);
    const auto get_api = reinterpret_cast<ShadDlssGetApiFn>(dlsym(module, "ShadDlssGetApi"));
    assert(get_api && get_api()->abi == SHADPS4_DLSS_BRIDGE_ABI);
    const auto* api = get_api();
    char temporary[] = "/tmp/sacredfire-ngx-test-XXXXXX";
    assert(mkdtemp(temporary));
    assert(api->Configure(directory.wstring().c_str(), std::filesystem::path(temporary).wstring().c_str(),
        [](int, const char* message) { std::puts(message); }));
    uint32_t count = 0;
    const VkExtensionProperties* required = nullptr;
    assert(api->InstanceExtensions(&count, &required));
    std::vector<const char*> extensions;
    for (uint32_t i = 0; i < count; ++i) extensions.push_back(required[i].extensionName);
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "DLSS bridge check", 1,
        nullptr, 0, VK_API_VERSION_1_2};
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = uint32_t(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.data();
    VkInstance instance;
    const auto instance_result = vkCreateInstance(&instance_info, nullptr, &instance);
    if (instance_result == VK_ERROR_INCOMPATIBLE_DRIVER && argc == 2) {
        dlclose(module);
        std::filesystem::remove_all(temporary);
        std::puts("PASS: Linux DLSS bridge; GPU initialization skipped (no Vulkan driver)");
        return 0;
    }
    assert(instance_result == VK_SUCCESS);
    assert(vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS);
    std::vector<VkPhysicalDevice> devices(count);
    assert(vkEnumeratePhysicalDevices(instance, &count, devices.data()) == VK_SUCCESS);
    bool initialized = false;
    for (auto physical : devices) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical, &properties);
        if (properties.vendorID != 0x10de) continue;
        assert(api->DeviceExtensions(instance, physical, &count, &required));
        extensions.clear();
        for (uint32_t i = 0; i < count; ++i) extensions.push_back(required[i].extensionName);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
        uint32_t family = 0;
        while (family < count && !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
        assert(family < count);
        float priority = 1;
        const VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0,
            family, 1, &priority};
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue;
        info.enabledExtensionCount = uint32_t(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        VkDevice device;
        assert(vkCreateDevice(physical, &info, nullptr, &device) == VK_SUCCESS);
        initialized = api->Initialize(instance, physical, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr);
        assert(initialized);
        api->Shutdown();
        vkDestroyDevice(device, nullptr);
        break;
    }
    vkDestroyInstance(instance, nullptr);
    dlclose(module);
    std::filesystem::remove_all(temporary);
    if (argc > 2) assert(initialized);
    std::puts(initialized ? "PASS: Linux DLSS bridge and NVIDIA NGX initialization" :
        "PASS: Linux DLSS bridge; GPU initialization skipped (no NVIDIA GPU)");
}
