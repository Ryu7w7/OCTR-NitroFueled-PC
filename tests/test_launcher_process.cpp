// Linux process boundaries: quoting, input, exit codes, output pressure and descendant timeout.
#include "../launcher/linux/process.h"
#include <cassert>
#include <thread>
int main() {
    using namespace LinuxProcess;
    assert((Arguments("'a b' 'x'\\''y' '' '$HOME;$(false)' ") == std::vector<std::string>{"a b", "x'y", "", "$HOME;$(false)"}));
    assert(Arguments("'unfinished").empty());
    const auto input = Run({"cat"}, "/tmp", 1000, "settings with ' quotes and 日本語\n");
    assert(input.started && input.code == 0 && input.output == "settings with ' quotes and 日本語\n");
    assert(!Run({"/no-such-launcher-executable"}, "", 1000).started);
    assert(Run({"sh", "-c", "exit 7"}, "", 1000).code == 7);
    const auto output = Run({"sh", "-c", "head -c 131072 /dev/zero"}, "", 2000);
    assert(output.code == 0 && output.output.size() == 131072);
    assert(Run({"sh", "-c", "sleep 30 & wait"}, "", 100).code == 124);
    std::atomic<bool> cancel{false};
    std::thread stop([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); cancel = true; });
    assert(Run({"sh", "-c", "trap '' TERM; sleep 30 & wait"}, "", 10000, {}, &cancel).code == 130);
    stop.join();
}
