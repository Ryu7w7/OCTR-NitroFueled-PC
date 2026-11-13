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
    std::puts("PASS: bounded texture uploads, FIFO order and oversized textures");
}
