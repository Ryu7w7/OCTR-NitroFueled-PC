#include "bbport_native_vk_spirv.h"

#include <cassert>
#include <cstdio>
#include <fstream>

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path path = argv[1];
    const std::vector<std::uint32_t> module{
        0x07230203, 0x10000, 0, 2, 0,
        (4u << 16) | 71, 1, 34, 0, // OpDecorate %1 DescriptorSet 0
    };
    auto write = [&](const void* data, std::size_t size) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(static_cast<const char*>(data), size);
        assert(file.good());
    };
    using namespace BbNativeVk::Spirv;
    assert(ReadFile(path).empty());
    write(module.data(), module.size() * 4);
    auto code = ReadFile(path);
    assert(code == module);
    std::vector<Binding> bindings;
    std::uint32_t locations = 0;
    assert(Prepare(code, 2, &bindings, &locations));
    assert(code.back() == 2);
    // A partial word, incomplete header, empty file, or wrong magic must never reach Vulkan.
    write(module.data(), module.size() * 4 - 1);
    assert(ReadFile(path).empty());
    write(module.data(), 4);
    assert(ReadFile(path).empty());
    write(module.data(), 0);
    assert(ReadFile(path).empty());
    auto damaged = module;
    damaged[0] = 0;
    write(damaged.data(), damaged.size() * 4);
    assert(ReadFile(path).empty());
    // A whole-word truncation passes file framing but must still fail reflection.
    write(module.data(), module.size() * 4 - 4);
    code = ReadFile(path);
    assert(!Prepare(code, 2, &bindings, &locations));
    std::filesystem::remove(path);
    std::puts("PASS: native shader file loading");
}
