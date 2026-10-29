// Synthetic image checks; an optional loaded image verifies locally owned game files.
#include "../gpu/shim/bbport_graphics_tab.cpp"
#include <cassert>
#include <fstream>
#include <iterator>

extern "C" {
int runtime_graphics_tab_ready = 0;
void runtime_restart() {}
}
namespace BbOverlay {
void QuitGame() {}
}
namespace BbSettings {
Values& Get() { static Values values; return values; }
void Save() {}
bool RenderSizeNeedsRestart() { return false; }
bool ResolutionNeedsRestart() { return false; }
bool UltrawideAvailable() { return false; }
void SetUltrawide(bool) {}
}

static void check(std::vector<u8> image, u64 shift) {
    using namespace BbGraphicsTab;
    const u64 sites[] = {Open2 - shift, Hide, Show, LanguageItemEvent - shift, LoadMap2 - shift};
    runtime_graphics_tab_ready = 0;
    // Invalid helper code must leave all detour sites unchanged.
    const auto original = image;
    image[Release - shift] ^= 1;
    PatchImage(image.data(), image.size());
    assert(!runtime_graphics_tab_ready);
    for (u64 site : sites) assert(!std::memcmp(image.data() + site, original.data() + site, 16));
    image = original;
    // A mismatched read-only default is rejected before any writes.
    image[LoadMap2 - shift + 0x41] ^= 1;
    PatchImage(image.data(), image.size());
    assert(!runtime_graphics_tab_ready);
    image = original;
    BbSettings::Get().graphics_tab = false;
    PatchImage(image.data(), image.size());
    assert(!runtime_graphics_tab_ready && image == original);
    BbSettings::Get().graphics_tab = true;
    PatchImage(image.data(), 16);
    assert(!runtime_graphics_tab_ready && image == original);
    PatchImage(image.data(), image.size());
    assert(runtime_graphics_tab_ready && code_shift == shift);
    assert(string_pin_default == (shift ? 0x1df8cd0 : 0x1df8d10));
    for (u64 site : sites) {
        assert(image[site] == 0x48 && image[site + 1] == 0xb8);
        assert(image[site + 10] == 0xff && image[site + 11] == 0xe0);
    }
}

int main(int argc, char** argv) {
    using namespace BbGraphicsTab;
    if (argc == 3) {
        std::ifstream input(argv[1], std::ios::binary);
        assert(input);
        std::vector<u8> image{std::istreambuf_iterator<char>(input), {}};
        check(std::move(image), std::stoul(argv[2], nullptr, 16));
        return 0;
    }
    for (u64 shift : {0u, 0x10u}) {
        std::vector<u8> image(0x2af3000);
        const auto put = [&](u64 at, const void* bytes, std::size_t n) {
            std::memcpy(image.data() + at, bytes, n);
        };
        put(RegionQueryCall - shift, RegionQueryCode, sizeof(RegionQueryCode));
        for (u64 site : {Open2 - shift, LanguageItemEvent - shift, Intern - shift, Unref - shift})
            put(site, Open2Prologue, sizeof(Open2Prologue));
        for (u64 site : {Hide, Show, LoadMap2 - shift})
            put(site, PlaceablePrologue, sizeof(PlaceablePrologue));
        put(PinGetter, "\x48\x89\xf8\x48\x85\xf6\x74\x12", 8);
        put(StringPin, "\x55\x48\x89\xe5\x53\x50\x48\x89\xf0", 9);
        put(Release - shift, "\x48\x85\xff\x74\x05\xe9", 6);
        put(LoadMap2 - shift + 0x3e, "\x48\x8d\x0d", 3);
        const s32 displacement = (shift ? 0x1df8cd0 : StringPinDefault) - (LoadMap2 - shift + 0x45);
        put(LoadMap2 - shift + 0x41, &displacement, 4);
        check(std::move(image), shift);
    }
}
