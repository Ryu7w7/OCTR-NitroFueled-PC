// Exercise the production cache writer without a game or GPU.
#include "../gpu/shadps4/video_core/cache_storage.cpp"
#include <cassert>
#include <fstream>

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path folder = argv[1];
    std::filesystem::create_directories(folder);
    auto save = [&](unsigned id) {
        std::vector<u32> data(256, id);
        assert(Storage::WriteVector(Storage::BlobType::ShaderBinary,
                                    folder / std::to_string(id), std::move(data)));
    };
    auto verify = [&](unsigned id) {
        std::ifstream file(folder / (std::to_string(id) + ".spv"), std::ios::binary);
        std::vector<u32> data(256);
        assert(file.read(reinterpret_cast<char*>(data.data()), data.size() * sizeof(u32)));
        for (const auto word : data) {
            assert(word == id);
        }
        assert(file.peek() == std::char_traits<char>::eof());
        std::filesystem::remove(folder / (std::to_string(id) + ".spv"));
    };

    // A close immediately after submit must drain even if the worker has not started yet.
    save(0);
    std::stop_source stopped;
    stopped.request_stop();
    Storage::ProcessIO(stopped.get_token());
    assert(num_requests == 0 && req_queue.empty());
    verify(0);

    std::jthread worker(Storage::ProcessIO);
    std::vector<std::thread> producers;
    constexpr unsigned Producers = 8, Writes = 64;
    for (unsigned p = 0; p < Producers; ++p) {
        producers.emplace_back([&, p] {
            for (unsigned i = 0; i < Writes; ++i) {
                save(1 + p * Writes + i);
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    worker.request_stop();
    worker.join();
    assert(num_requests == 0 && req_queue.empty());
    for (unsigned id = 1; id <= Producers * Writes; ++id) {
        verify(id);
    }
    std::puts("PASS: concurrent shader cache writes and shutdown drain");
}
