// Exercise the actual capture/compile handoff, including a deliberately blocked loader.
// The test keeps assertions; libbbgpu's dispatcher is built with NDEBUG (a different layout).
namespace vk::detail { class DispatchLoaderDynamic; }
extern vk::detail::DispatchLoaderDynamic test_dispatch;
#define VULKAN_HPP_DEFAULT_DISPATCHER test_dispatch
#include "../gpu/shim/bbport_native_vk.cpp"
#include "bbport_native_vk_host_shaders.h"

#include <cassert>
#include <future>

vk::detail::DispatchLoaderDynamic test_dispatch;

int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace BbNativeVk;
    Vulkan::Instance gpu(0, false);
    static vk::detail::DynamicLoader loader;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(
        loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    VULKAN_HPP_DEFAULT_DISPATCHER.init(gpu.GetInstance());
    VULKAN_HPP_DEFAULT_DISPATCHER.init(gpu.GetDevice());
    instance.store(&gpu);
    shader_root = argv[1];
    std::filesystem::create_directories(shader_root / "spirv");
    {
        std::ofstream out(shader_root / "spirv/test.spv", std::ios::binary);
        out.write(reinterpret_cast<const char*>(HostShaders::fullscreen_vert),
                  sizeof(HostShaders::fullscreen_vert));
        assert(out.good());
    }
    drawing = async_pipelines = true;
    ShaderInfo shader;
    shader.name = "test";
    shader.kind = "vs";
    assert(!ModuleReady(shader));
    assert(compile_queue.size() == 1);
    auto job = std::move(compile_queue.front());
    compile_queue.pop_front();
    assert(job->module == &shader);
    std::promise<void> release;
    auto gate = release.get_future();
    auto worker = std::async(std::launch::async, [&] {
        gate.wait(); // simulate slow storage; the capture side must keep returning
        LoadModule(*job->module);
    });
    for (int i = 0; i < 10000; ++i) {
        assert(!ModuleReady(shader));
    }
    assert(compile_queue.empty()); // never queue the same in-flight module twice
    release.set_value();
    worker.get();
    assert(ModuleReady(shader));
    assert(shader.module && shader.set_layout && !shader.code.empty());

    ShaderInfo missing;
    missing.name = "missing";
    missing.kind = "vs";
    assert(!ModuleReady(missing));
    job = std::move(compile_queue.front());
    compile_queue.pop_front();
    LoadModule(*job->module);
    assert(ModuleReady(missing)); // failure is published, not retried every draw
    assert(!missing.module && compile_queue.empty());

    async_pipelines = false;
    ShaderInfo sync;
    sync.name = "test";
    sync.kind = "vs";
    assert(ModuleReady(sync));
    assert(sync.module && sync.set_layout && compile_queue.empty());
    for (ShaderInfo* s : {&shader, &sync}) {
        gpu.GetDevice().destroyShaderModule(s->module);
        gpu.GetDevice().destroyDescriptorSetLayout(s->set_layout);
    }
    std::filesystem::remove(shader_root / "spirv/test.spv");
    std::puts("PASS: async shader loading, publication, failure and synchronous mode");
}
