// Exercise the production upload queue without a Vulkan device or game.
#include "../gpu/shim/bbport_native_vk_texture.cpp"
#include <cassert>

int main() {
    using namespace BbNativeVk::Tex;
    constexpr std::uint64_t MiB = 1ull << 20;
    auto queue = [](std::uint64_t key, std::uint64_t bytes) {
        Job job{};
        job.key = key;
        job.l.total = bytes;
        upload_queue.push_back(std::move(job));
    };
    // An early texture must not wait for a 64 MiB streaming burst to be detiled.
    std::array<Texture, 64> textures{};
    for (unsigned i = 0; i < 64; ++i) {
        queue(i, MiB);
        upload_queue.back().tex = &textures[i];
    }
    for (unsigned i = 0; i < 64; i += 4) {
        auto jobs = TakeUploadBatch();
        assert(jobs.size() == 4);
        for (unsigned j = 0; j < jobs.size(); ++j) {
            assert(jobs[j].key == i + j);
        }
        Settle(jobs, true);
        for (unsigned j = 0; j < textures.size(); ++j) {
            assert(textures[j].ready.load() == (j < i + 4));
        }
    }
    assert(upload_queue.empty());
    // A single oversized texture still advances the queue, in FIFO order.
    queue(0, MiB);
    queue(1, 8 * MiB);
    queue(2, MiB);
    for (unsigned i = 0; i < 3; ++i) {
        auto jobs = TakeUploadBatch();
        assert(jobs.size() == 1 && jobs[0].key == i);
    }
    assert(upload_queue.empty());
    // Planned jobs use their actual upload size rather than the guest layout.
    queue(0, MiB);
    upload_queue.back().out_bytes = 4 * MiB;
    queue(1, MiB);
    assert(TakeUploadBatch().size() == 1);
    assert(TakeUploadBatch().size() == 1);
    assert(upload_queue.empty());
    // Animated textures keep their last frame, even when the next upload takes over 2 ms.
    instance = reinterpret_cast<const Vulkan::Instance*>(1); // Get's cache path uses no device
    static int retired = 0;
    const std::uint32_t tsharp[8]{0, 1u << 20, 0, 8u << 28}; // linear R8, one pixel
    const auto key = Fnv(reinterpret_cast<const u8*>(tsharp), 24);
    auto frame = [] {
        auto t = std::make_unique<Texture>();
        t->image = reinterpret_cast<VkImage>(1);
        t->ready.store(true);
        t->stale.store(true);
        return t;
    };
    BbNativeVk::Tex::textures[key] = frame();
    Texture* previous = BbNativeVk::Tex::textures.at(key).get();
    assert(Get(tsharp) == previous);
    assert(upload_queue.size() == 1);
    Texture* next = replacements.at(key).get();
    assert(upload_queue.front().tex == next && !next->ready.load());
    host.retire = [](Texture* t) { ++retired; delete t; };
    for (int i = 0; i < 3; ++i) {
        assert(Get(tsharp) == previous);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    Evict(budget + 1, EvictAfter + 1);
    assert(BbNativeVk::Tex::textures.at(key).get() == previous);
    assert(retired == 0);
    next->image = reinterpret_cast<VkImage>(1);
    auto jobs = TakeUploadBatch();
    Settle(jobs, true);
    assert(Get(tsharp) == next);
    assert(replacements.empty());
    assert(retired == 1);
    // A failed replacement leaves the displayed frame intact and permits another attempt.
    replacements[key] = frame();
    replacements.at(key)->broken.store(true);
    assert(Get(tsharp) == next);
    assert(replacements.empty());
    assert(retired == 2);
    // Leaving the menu must not pin an abandoned upload and its old frame forever.
    replacements[key] = frame();
    Evict(budget + 1, EvictAfter + 1);
    assert(BbNativeVk::Tex::textures.empty() && replacements.empty());
    assert(retired == 4);
    instance = nullptr;
    std::puts("PASS: bounded texture uploads and animated texture replacement");
}
