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
bool Fsr4Available() { return true; }
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

static int native_events;
static u64 PS4_SYSV_ABI native_event(void*,void*,void*,void*,void*,void*) { ++native_events; return 17; }
static u64 PS4_SYSV_ABI event_type(void*) { return 123; }
static void controls_test() {
    using namespace BbGraphicsTab;
    // Native cloned placeables have no name. Recognition must survive layout changes
    // and must leave Privacy and the five ordinary Options buttons alone.
    alignas(8) u8 group[0x100] = {}, buttons[7][0x100] = {};
    *reinterpret_cast<float*>(group+0x60) = 510;
    *reinterpret_cast<float*>(group+0x64) = -9;
    u8 readback[sizeof(group)] = {};
    if (!SafeRead(group, readback, sizeof(group))) { std::perror("SafeRead"); assert(false); }
    for (int i = 0; i < 7; ++i) {
        *reinterpret_cast<u64*>(buttons[i]+0x40) = reinterpret_cast<u64>(group);
        *reinterpret_cast<u64*>(buttons[i]+0x50) = reinterpret_cast<u64>(i ? buttons[i-1] : group);
        assert(IsTab(buttons[i], "test") == (i == 5));
    }
    *reinterpret_cast<float*>(buttons[5]+0x60) = 36;
    *reinterpret_cast<float*>(buttons[5]+0x64) = 315;
    assert(IsTab(buttons[5], "test"));
    group[0x64] = 1; // Another menu with the same number of siblings is not Options.
    assert(!IsTab(buttons[5], "test"));
    assert(!IsTab(nullptr, "test"));
    auto& v = BbSettings::Get();
    v.render_res = 0;
    assert(Row(0, -1) == "2160p");
    assert(Row(0, 1) == "480p");
    v.aspect_ratio = BbSettings::Aspect16x9;
    assert(Row(1, -1) == "32:9");
    assert(v.aspect_w == 5120 && v.aspect_h == 1440);
    assert(Row(1, 1) == "16:9");
    assert(Row(1, 1) == "16:10");
    assert(v.aspect_w == 1920 && v.aspect_h == 1200);
    assert(Row(1, 1) == "21:9");
    assert(v.aspect_w == 3440 && v.aspect_h == 1440);
    assert(Description(1).find("16:10") != std::string::npos);
    v.temporal = 0;
    Row(2, -1); assert(v.temporal == BbSettings::TemporalCount-1);
    Row(2, 1); assert(v.temporal == 0);
    v.dlss_quality = 0; Row(3, -1); assert(v.dlss_quality == 5);
    v.fps_limit = 30; Row(6, -1); assert(v.fps_limit == 0);
    Row(6, 1); assert(v.fps_limit == 30);
    unsigned char item[0x38] = {}, list_item[0x80] = {};
    rows.assign(8, 0); rows[7] = reinterpret_cast<u64>(item);
    list_items.assign(8, 0); list_items[7] = reinterpret_cast<u64>(list_item);
    *reinterpret_cast<u32*>(list_item+0x18) = 0xf;
    ShowRow(rows[7], false); assert(list_item[0x18] == 7);
    ShowRow(rows[7], true); assert(list_item[0x18] == 15);
    std::vector<u8> memory(0x2a21dc0);
    image = memory.data();
    *reinterpret_cast<u64*>(image+InputEventMeta) = 123;
    void* vtable[] = {reinterpret_cast<void*>(event_type)};
    unsigned char event[32] = {};
    *reinterpret_cast<void***>(event) = vtable;
    *reinterpret_cast<u32*>(event+0x18) = 9;
    language_item_original = native_event;
    assert(LanguageItemHook(item,event,nullptr,nullptr,nullptr,nullptr) == 17);
    assert(native_events == 1); // Back works even on the Restart row.
    alignas(8) u8 anchor[0x120] = {}, selector[0x120] = {}, button[0x120] = {};
    alignas(8) u8 root[0x120] = {}, table[0x28] = {};
    u64 children[] = {reinterpret_cast<u64>(selector),reinterpret_cast<u64>(button)};
    *reinterpret_cast<u64*>(item+0x10) = reinterpret_cast<u64>(anchor);
    *reinterpret_cast<u64*>(anchor+0x40) = reinterpret_cast<u64>(selector);
    *reinterpret_cast<u64*>(selector+0x40) = reinterpret_cast<u64>(root);
    *reinterpret_cast<const char**>(selector+0x28) = "BbGraphicsNativeSelector";
    *reinterpret_cast<const char**>(button+0x28) = "BbGraphicsNativeRestart";
    *reinterpret_cast<u64*>(root+0x48) = reinterpret_cast<u64>(table);
    *reinterpret_cast<u32*>(table+0x10) = 2;
    *reinterpret_cast<u64*>(table+0x20) = reinterpret_cast<u64>(children);
    auto controls = Controls(reinterpret_cast<u64>(item));
    assert(controls.selector == children[0] && controls.button == children[1]);
    // The native asset replaces the old text subtree; bind directly to its instance.
    *reinterpret_cast<u64*>(item+0x10) = reinterpret_cast<u64>(selector);
    controls = Controls(reinterpret_cast<u64>(item));
    assert(controls.selector == children[0] && controls.button == children[1]);
    assert(FindNativeChild(controls.root,"BbGraphicsNativeRestart") == children[1]);
    alignas(8) u8 instances[0x40] = {}, title[0x120] = {}, value[0x120] = {};
    u64 instance_values[] = {0, reinterpret_cast<u64>(selector),
        reinterpret_cast<u64>(title), reinterpret_cast<u64>(value)};
    *reinterpret_cast<const char**>(title+0x28) = "[NameText]";
    *reinterpret_cast<const char**>(value+0x28) = "[ItemText]";
    *reinterpret_cast<u64*>(selector+0x10) = reinterpret_cast<u64>(instances);
    *reinterpret_cast<u32*>(instances+0x10) = sizeof(instance_values);
    *reinterpret_cast<u64*>(instances+0x18) = reinterpret_cast<u64>(instance_values);
    u64 instance_keys[] = {0, 0x1100, 0x2200, 0x3300};
    *reinterpret_cast<u64*>(instances+0x28) = reinterpret_cast<u64>(instance_keys);
    assert(NativeInstance(children[0], 0x2200) == reinterpret_cast<u64>(title));
    assert(NativeInstance(children[0], 0x3300) == reinterpret_cast<u64>(value));
    assert(NativeInstance(children[0], 0x4400) == 0);
    assert(FindNativeChild(children[0], "[NameText]") == reinterpret_cast<u64>(title));
    assert(FindNativeChild(children[0], "[ItemText]") == reinterpret_cast<u64>(value));
    assert(FindNativeChild(children[0], "absent") == 0); // Map includes itself.
    *reinterpret_cast<u32*>(instances+0x10) = 8193;
    assert(FindNativeChild(children[0], "[NameText]") == 0);
    *reinterpret_cast<u32*>(instances+0x10) = sizeof(instance_values);
    UpdateWidgets(reinterpret_cast<u64>(item),RestartRow);
    assert(*reinterpret_cast<float*>(selector+0xe0) == 0);
    assert(*reinterpret_cast<float*>(button+0xe0) == 1);
    ShowRow(reinterpret_cast<u64>(item),false);
    assert(*reinterpret_cast<float*>(root+0xe0) == 0);
    ShowRow(reinterpret_cast<u64>(item),true);
    assert(*reinterpret_cast<float*>(root+0xe0) == 1);
    for (float width : {1920.0f, 2560.0f, 3840.0f}) {
        *reinterpret_cast<float*>(root+0x90) = width;
        *reinterpret_cast<float*>(root+0x94) = 1080;
        PlaceDescription(reinterpret_cast<u64>(value), reinterpret_cast<u64>(root));
        const float text_width = *reinterpret_cast<float*>(value+0x90);
        assert(*reinterpret_cast<float*>(value+0x60)-text_width/2 == 64);
        assert(*reinterpret_cast<float*>(value+0x64) == 950);
        assert(text_width <= width/2-128);
    }
    std::puts("Graphics controls tests passed");
    alignas(8) u8 viewport[0x120] = {}, scroll[0x120] = {};
    *reinterpret_cast<u64*>(root+0x40) = reinterpret_cast<u64>(scroll);
    *reinterpret_cast<u64*>(scroll+0x40) = reinterpret_cast<u64>(viewport);
    *reinterpret_cast<u32*>(viewport+0xc) = 0x48c002;
    UnclipGraphicsList(reinterpret_cast<u64>(root));
    assert(*reinterpret_cast<u32*>(viewport+0xc) == 0x48d000);
    assert(*reinterpret_cast<u32*>(scroll+0xc) == 0);
}

int main(int argc, char** argv) {
    controls_test();
    using namespace BbGraphicsTab;
    auto& settings = BbSettings::Get();
    // controls_test changed other rows: start from their saved values.
    settings.startup_render_res = settings.render_res;
    settings.startup_temporal = settings.temporal;
    settings.startup_dlss_quality = settings.dlss_quality;
    settings.startup_output_res = settings.output_res;
    for (int g = 0; g < BbSettings::GamePatchCount; ++g)
        settings.startup_game_patches[g] = settings.game_patches[g].load();
    settings.aspect_ratio = BbSettings::Aspect16x9;
    settings.startup_aspect_ratio = BbSettings::Aspect16x9;
    settings.ultrawide = 1;
    assert(Row(1, 0) == "16:9");
    assert(Row(1, 1) == "16:10");
    assert(settings.aspect_w == 1920 && settings.aspect_h == 1200 && settings.ultrawide == 0);
    assert(NeedsRestart());
    assert(Row(1, 1) == "21:9");
    assert(settings.aspect_w == 3440 && settings.aspect_h == 1440);
    assert(Row(1, 1) == "32:9");
    assert(settings.aspect_w == 5120 && settings.aspect_h == 1440);
    assert(Row(1, 1) == "16:9");
    assert(!NeedsRestart());
    settings.aspect_w = 2560;
    settings.aspect_h = 1600;
    assert(Row(1, 1) == "16:10");
    assert(settings.aspect_w == 2560 && settings.aspect_h == 1600);
    assert(std::strcmp(TabLabel(21), "ﺍﻟﺮﺳﻮﻣﻴﺎﺕ") == 0);
    assert(std::strcmp(TabLabel(1), "Graphics") == 0);
    assert(std::strcmp(TabLabel(2), "Graphismes") == 0);
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
        put(IconMaterial - shift, IconMaterialCode, sizeof(IconMaterialCode));
        put(PlayAnimation, PlayAnimationCode, sizeof(PlayAnimationCode));
        put(PinGetter, "\x48\x89\xf8\x48\x85\xf6\x74\x12", 8);
        put(StringPin, "\x55\x48\x89\xe5\x53\x50\x48\x89\xf0", 9);
        put(Release - shift, "\x48\x85\xff\x74\x05\xe9", 6);
        put(LoadMap2 - shift + 0x3e, "\x48\x8d\x0d", 3);
        const s32 displacement = (shift ? 0x1df8cd0 : StringPinDefault) - (LoadMap2 - shift + 0x45);
        put(LoadMap2 - shift + 0x41, &displacement, 4);
        check(std::move(image), shift);
    }
}
