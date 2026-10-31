// SPDX-License-Identifier: GPL-2.0-or-later
// The game's Options menu "Graphics" tab: the menu's fifth tab (Language Select, hidden by the
// game in this region) is shown again and opens the port's settings menu instead of the language
// screen. Its label comes from scripts/graphics_tab.py (relabelled menu text in update3.pak).
//
// Changes to the game's code (CTR NF 1.21, image offsets), visual-script nodes:
//  - CGuiNodes::open2_VSC (0x1109200): track the Graphics project
//    opened by its native transition.
// The hide hook keeps the Graphics entry visible.
// The first calls of each are logged with the object names found ("Graphics tab: ...").
#include "bbport_graphics_tab.h"
#include "bbport_settings.h"
#include "common/types.h"

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

extern "C" void runtime_restart(void); // bb-probe (probe.c): run.py starts the game again
// runtime_file.c: the relabelled menus (update3.pak, scripts/graphics_tab.py) reach the game
// only when every hook below is in: a tab or a Quit Game button without its hook would open a
// menu or a map that does not exist.
extern "C" int runtime_graphics_tab_ready;

namespace BbOverlay {
void OpenMenu(); // bbport_overlay.h (Vulkan headers)
void QuitGame();
}

namespace BbGraphicsTab {
namespace {
using u8 = std::uint8_t;
using u64 = std::uint64_t;

/// regionSwitch_VSC: `call 0x1036010` (the region query filling its three output flags).
constexpr u64 RegionQueryCall = 0x10de0c9;
constexpr u8 RegionQueryCode[5] = {0xe8, 0x42, 0x7f, 0xf5, 0xff};

/// open2_VSC's prologue (push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12).
constexpr u64 Open2 = 0x1109200;
constexpr u8 Open2Prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57,
                                  0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
/// The pin getter both use for their object input: (context, pin, out object), which returns
/// the object with a reference taken; the release (0x150a790) drops it.
constexpr u64 PinGetter = 0x4708a0;
constexpr u64 Release = 0x150a790;

constexpr char Project[] = "Octane_Options_LanguageSelect";
/// hidePlaceable_VSC / showPlaceable_VSC (same prologue: push rbp; mov rbp,rsp; push r15;
/// push r14; push rbx; sub rsp,0x18). The game's menu objects are its .igz data in place: the
/// Graphics is identified by its native Options sibling chain, not a runtime name.
constexpr u64 Hide = 0x26f6b0, Show = 0x26f740;
/// CLoadMapNodes::loadMap2_VSC (same prologue): the main menu's Quit Game button (Wireless) asks
/// for this map (scripts/graphics_tab.py renames its Octane_OnlineLobby): the game quits instead.
/// Its first pin is the map name, read with 0x478290 (out string, context, pin, default 0x1df8d10).
constexpr u64 LoadMap2 = 0x11345b0, StringPin = 0x478290, StringPinDefault = 0x1df8d10;
constexpr char QuitMap[] = "Octane_BbportQuit0";
constexpr u8 PlaceablePrologue[13] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                      0x56, 0x53, 0x48, 0x83, 0xec, 0x18};

using VscFn = u64 PS4_SYSV_ABI (*)(void*, void*, void*, void*, void*, void*);
using GetterFn = void PS4_SYSV_ABI (*)(void*, void*, void**);
using ReleaseFn = void PS4_SYSV_ABI (*)(void*);

u8* image = nullptr;
// EU 1.21 moves the later functions by 16 bytes and this read-only default by 64.
u64 code_shift = 0, string_pin_default = StringPinDefault;
VscFn open2_original = nullptr, hide_original = nullptr, show_original = nullptr;
std::atomic<int> open_logged{0}, placeable_logged{0};
std::mutex last_mutex;
std::string last_opened;

bool SafeRead(const void* address, void* out, std::size_t size) {
#ifdef _WIN32
    // ReadProcessMemory on this process: a fault-safe copy for one system call. A VirtualQuery
    // before each read took about 0.7 ms here (the guest's address space is a few huge regions),
    // so opening a menu (hundreds of reads in Name) stalled 330-375 ms and each hidden placeable
    // 20-30 ms (OCTR v0.7 test, measured).
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &copied) && copied == size;
#else
    iovec local{out, size}, remote{const_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(size);
#endif
}

/// The node's object input (its first pin), with the getter's reference dropped again.
void* Input(void* context, void* args) {
    void* object = nullptr;
    void** pins = nullptr;
    void* pin = nullptr;
    if (args && SafeRead(static_cast<u8*>(args) + 0x10, &pins, sizeof(pins)) && pins &&
        SafeRead(pins, &pin, sizeof(pin)) && pin) {
        reinterpret_cast<GetterFn>(image + PinGetter)(context, pin, &object);
        if (object) {
            reinterpret_cast<ReleaseFn>(image + (Release - code_shift))(object);
        }
    }
    return object;
}

/// An identifier-like string at `address` (at least 4 characters: letters, digits, '_').
bool Identifier(u64 address, std::string& out) {
    char text[96] = {};
    if (address < 0x10000 || (address & 0xffff000000000000ull) ||
        !SafeRead(reinterpret_cast<void*>(address), text, 64)) {
        return false;
    }
    std::size_t n = 0;
    while (text[n] && (std::isalnum(static_cast<unsigned char>(text[n])) || text[n] == '_')) {
        ++n;
    }
    if (n < 4 || text[n] != 0) {
        return false;
    }
    out = text;
    return true;
}

/// The object's name (igNamedObject::_name and the like): a string a field among the object's
/// first 32 points to, else one a field of a pointed-to object points to (one level down).
/// `where` gets the field offset (one level down: offset * 0x1000 + inner offset).
std::string Name(void* object, int* where = nullptr) {
    u64 fields[32];
    if (!object || !SafeRead(object, fields, sizeof(fields))) {
        return {};
    }
    std::string text;
    for (int i = 1; i < 32; ++i) {
        if (Identifier(fields[i], text)) {
            if (where) *where = i * 8;
            return text;
        }
    }
    for (int i = 1; i < 32; ++i) {
        u64 inner[16];
        if (fields[i] < 0x10000 || (fields[i] & 0xffff000000000000ull) ||
            !SafeRead(reinterpret_cast<void*>(fields[i]), inner, sizeof(inner))) {
            continue;
        }
        for (int j = 0; j < 16; ++j) {
            if (Identifier(inner[j], text)) {
                if (where) *where = i * 8 * 0x1000 + j * 8;
                return text;
            }
        }
    }
    return {};
}

/// The first calls: the raw object and its first fields, for finding the name field.
void Dump(const char* what, void* args, void* object) {
    static std::atomic<int> dumped{0};
    if (dumped.fetch_add(1) >= 12) {
        return;
    }
    u64 fields[8] = {};
    const bool ok = object && SafeRead(object, fields, sizeof(fields));
    std::printf("Graphics tab: %s args=%p object=%p%s", what, args, object, ok ? " fields" : "");
    for (int i = 0; ok && i < 8; ++i) {
        std::printf(" %llx", static_cast<unsigned long long>(fields[i]));
    }
    std::printf("\n");
}

/// A placeable's state for the log: position (+0x60), flags (+0xc), alpha (+0xe0).
bool IsTab(void* object, const char* what) {
    struct {
        u8 head[0x60];
        float position[2];
    } p{};
    u32 flags = 0;
    float alpha = -1.0f;
    if (!object || !SafeRead(object, &p, sizeof(p))) {
        return false;
    }
    SafeRead(static_cast<u8*>(object) + 0xc, &flags, sizeof(flags));
    SafeRead(static_cast<u8*>(object) + 0xe0, &alpha, sizeof(alpha));
    // Placeables are unnamed at runtime (+0x28 is zero). The Options button group
    // has a native anchor and Graphics is followed by Extras, Hints, Controls,
    // Audio and Gameplay, then the parent sentinel. Privacy has one more hop.
    u64 parent = 0, current = reinterpret_cast<u64>(object);
    float anchor[2] = {};
    bool tab = SafeRead(static_cast<u8*>(object) + 0x40, &parent, 8) && parent &&
               SafeRead(reinterpret_cast<void*>(parent + 0x60), anchor, sizeof(anchor)) &&
               anchor[0] == 510.0f && anchor[1] == -9.0f;
    for (int i = 0; tab && i < 6; ++i) {
        u64 owner = 0, next = 0;
        tab = SafeRead(reinterpret_cast<void*>(current + 0x40), &owner, 8) && owner == parent &&
              SafeRead(reinterpret_cast<void*>(current + 0x50), &next, 8) && next;
        current = next;
        if (tab && i < 5 && current == parent) tab = false;
    }
    tab = tab && current == parent;
    if (placeable_logged.fetch_add(1) < 80 || tab) {
        std::printf("Graphics tab: %s %p at (%g, %g) flags %x alpha %g%s\n", what, object,
                    p.position[0], p.position[1], flags, alpha, tab ? " <- the tab" : "");
        std::fflush(stdout);
    }
    return tab;
}

/// The language screen's project (ui/Octane_Options_LanguageSelect.igz, loaded in place): its
/// igGuiProject has pointers to its own data at +0x10, +0x18, +0x38 and +0xa0, at these distances.
bool IsLanguageProject(void* object) {
    u64 f[0x15] = {};
    if (!object || !SafeRead(object, f, sizeof(f))) {
        return false;
    }
    const u64 o = reinterpret_cast<u64>(object);
    return f[2] == o + 0xf0 && f[3] == o + 0x210 && f[7] == o + 0x5d0 && f[0x14] == o + 0x610;
}

/// A readable text pointer: printable characters up to a terminator.
[[maybe_unused]] bool IsText(u64 address, std::string* out = nullptr) {
    char text[48] = {};
    if (address < 0x10000 || (address >> 48) || !SafeRead(reinterpret_cast<void*>(address), text, 40)) {
        return false;
    }
    std::size_t n = 0;
    while (n < 40 && text[n] >= 0x20 && text[n] < 0x7f) {
        ++n;
    }
    if (n < 3 || text[n] != 0) {
        return false;
    }
    if (out) {
        *out = text;
    }
    return true;
}

/// The navigation item among a placeable's behaviors (+0xe8: igGuiBehaviorList, its data at +0x20,
/// count at +0x10): the one that points back at the placeable's neighbours (+0x110, +0x118).
u64 NavItem(u64 placeable, u64* prev = nullptr) {
    u64 list = 0, count = 0, items = 0;
    if (!SafeRead(reinterpret_cast<void*>(placeable + 0xe8), &list, 8) || !list ||
        !SafeRead(reinterpret_cast<void*>(list + 0x10), &count, 8) ||
        !SafeRead(reinterpret_cast<void*>(list + 0x20), &items, 8) || !items) {
        return 0;
    }
    count &= 0xffffffff;
    for (u64 i = 0; i < count && i < 8; ++i) {
        u64 b = 0, links[2] = {};
        if (SafeRead(reinterpret_cast<void*>(items + 8 * i), &b, 8) && b &&
            SafeRead(reinterpret_cast<void*>(b + 0x110), links, sizeof(links)) && links[0] && links[1]) {
            if (prev) {
                *prev = links[1];
            }
            return b;
        }
    }
    return 0;
}

// The game's strings: interned in its string pool (pointer at 0x2af2600) by 0x1574080 (pool,
// text), which returns a string with a reference taken (count at -8); 0x1518e30 (pool, string -
// 0x10) drops one. A text placeable shows the string at +0x30 (CGuiBehaviorLanguageItem's init,
// 0x1276c70, sets it so and marks the placeable shown: +0xc |= 0x1000).
constexpr u64 StringPool = 0x2af2600, Intern = 0x1574080, Unref = 0x1518e30;
using InternFn = char* PS4_SYSV_ABI (*)(void*, const char*);
using UnrefFn = void PS4_SYSV_ABI (*)(void*, void*);

void* Pool() {
    return *reinterpret_cast<void**>(image + StringPool);
}

char* GameString(const char* text) {
    return reinterpret_cast<InternFn>(image + (Intern - code_shift))(Pool(), text);
}

void DropString(char* string) {
    if (string) {
        reinterpret_cast<UnrefFn>(image + (Unref - code_shift))(Pool(), string - 0x10);
    }
}

/// A string field the game owns: the new text replaces the old (whose reference is dropped).
void SetStringField(u64 address, const char* text) {
    char** field = reinterpret_cast<char**>(address);
    char* old = *field;
    *field = GameString(text);
    DropString(old);
}

void SetText(u64 placeable, const char* text) {
    if (!placeable) {
        return;
    }
    SetStringField(placeable + 0x30, text);
    *reinterpret_cast<u32*>(placeable + 0xc) |= 0x1000;
}

/// The tab's label: its navigation item (CGuiBehaviorNavigationItem: _itemText +0xe8,
/// _textPlaceable +0xf0, from the game's field tables) gets "Graphics" (the text table has none
/// for it in this region, so the button keeps its template's "Crash Cove").
const char* TabLabel(int id) {
    return (id == 3 || id == 20 || id == 7 || id == 17) ? "Gráficos"
         : (id == 2 || id == 22)                       ? "Graphismes"
         : id == 4                                    ? "Grafik"
         : id == 5                                    ? "Grafica"
         : id == 21                                   ? "ﺍﻟﺮﺳﻮﻣﻴﺎﺕ"
                                                       : "Graphics";
}

void Relabel(void* tab) {
    const u64 item = NavItem(reinterpret_cast<u64>(tab));
    if (!item) {
        std::printf("Graphics tab: label: no navigation item\n");
        return;
    }
    // In the game's language (run.py's BB_LANGUAGE, PS4 language ids), as scripts/graphics_tab.py
    // writes it in the available USA/EU text tables.
    const char* language = std::getenv("BB_LANGUAGE");
    const int id = language ? std::atoi(language) : 1;
    const char* label = TabLabel(id);
    SetStringField(item + 0xe8, label);
    SetText(*reinterpret_cast<u64*>(item + 0xf0), label);
    std::printf("Graphics tab: labelled\n");
    std::fflush(stdout);
}

// ---- The Graphics page: the language screen's rows, relabelled as settings -----------------
// CGuiBehaviorLanguageItem (size 0x38): +0x10 its text placeable, +0x28 its language. Its event
// handler (0x1276c70: this, event, ...) fills the row on the list's init event (event class at
// 0x2a21db0) and, on the input event class (0x2a21d10) with type 8 (+0x18), sets the language
// and opens the "Language Selected" dialog. Here: each row shows a setting; pressing changes it.
constexpr u64 LanguageItemEvent = 0x1276c70;
constexpr u64 InitEventMeta = 0x2a21db0, InputEventMeta = 0x2a21d10;
constexpr u64 GainFocusMeta = 0x2a21eb8;
VscFn language_item_original = nullptr;

bool IsA(void* event, u64 meta_global) {
    using MetaFn = u64 PS4_SYSV_ABI (*)(void*);
    const u64 target = *reinterpret_cast<u64*>(image + meta_global);
    u64 meta = reinterpret_cast<MetaFn>(**reinterpret_cast<u64**>(event))(event);
    for (int depth = 0; meta && depth < 16; ++depth) {
        if (meta == target) {
            return true;
        }
        meta = *reinterpret_cast<u64*>(meta + 0x40);
    }
    return false;
}

std::mutex rows_mutex;
std::vector<u64> list_items; // native igGuiListItem objects, same lifetime as rows
std::vector<u64> rows; ///< the language items, in the order the list filled them

const char* OnOff(bool on) {
    return on ? "On" : "Off";
}

/// Native selector value; its title and arrow sprites are separate placeables.
std::string Row(std::size_t row, int change) {
    auto& v = BbSettings::Get();
    char text[96];
    // The page's rows, top to bottom: Resolution, Aspect Ratio, Upscaling, Upscale Quality,
    // FPS Unlock, Motion blur, FPS limit (the cases below; 6 is Aspect Ratio).
    static constexpr std::size_t kSetting[] = {0, 6, 1, 2, 3, 4, 5};
    if (row >= std::size(kSetting)) {
        return "";
    }
    const std::size_t index = kSetting[row];
    switch (index) {
    case 0: {
        // The render size (render_res); the shape is the Aspect Ratio row's.
        int k = v.render_res.load() < 0 ? 2 : v.render_res.load();
        if (change) {
            k = (k + change + BbSettings::RenderResCount) % BbSettings::RenderResCount;
            v.render_res = k;
        }
        std::snprintf(text, sizeof(text), "%dp", BbSettings::RenderResHeights[k]);
        return text;
    }
    case 1: {
        int t = v.temporal.load();
        if (change) {
            t = (t + change + BbSettings::TemporalCount) % BbSettings::TemporalCount;
            if (t == BbSettings::TemporalFsr4 && !BbSettings::Fsr4Available())
                t = (t + change + BbSettings::TemporalCount) % BbSettings::TemporalCount;
            v.temporal = t;
        }
        static const char* names[] = {"Off", "FSR 1", "FSR 3", "DLSS", "FSR 4"};
        std::snprintf(text, sizeof(text), "%s",
                      names[unsigned(t) < std::size(names) ? t : 0]);
        return text;
    }
    case 2: {
        int q = v.dlss_quality.load();
        if (change) {
            q = (q + change + BbSettings::DlssQualityCount) % BbSettings::DlssQualityCount;
            v.dlss_quality = q;
        }
        static const char* names[] = {"Auto", "DLAA", "Quality", "Balanced", "Performance", "Ultra Perf."};
        std::snprintf(text, sizeof(text), "%s", names[q % 6]);
        return text;
    }
    case 3:
    case 4: {
        auto& patch = v.game_patches[index == 3 ? 0 : 1];
        if (change) {
            patch = !patch.load();
        }
        // Row 4 is the game's motion blur: on unless the patch removes it.
        std::snprintf(text, sizeof(text), index == 3 ? "%s" : "%s",
                      OnOff(index == 3 ? patch.load() : !patch.load()));
        return text;
    }
    case 5: {
        static const int limits[] = {30, 60, 120, 144, 240, 0};
        int limit = v.fps_limit.load(), k = 0;
        while (k < 6 && limits[k] != limit) {
            ++k;
        }
        if (change) {
            limit = limits[(k + change + 6) % 6];
            v.fps_limit = limit;
        }
        if (limit) {
            std::snprintf(text, sizeof(text), "%d", limit);
        } else {
            std::snprintf(text, sizeof(text), "Off");
        }
        return text;
    }
    case 6: {
        // The same display shape as the launcher, applied by scripts/aspect_ratio.py at start.
        int ratio = std::clamp(v.aspect_ratio.load(), 0, BbSettings::AspectRatioCount - 1);
        if (change) {
            ratio = (ratio + change + BbSettings::AspectRatioCount) % BbSettings::AspectRatioCount;
            v.aspect_ratio = ratio;
            if (ratio != BbSettings::Aspect16x9) {
                v.ultrawide = 0;
                const int w = v.aspect_w.load(), h = v.aspect_h.load();
                const bool tall = (w == 1920 && h == 1200) || (w == 1280 && h == 800) ||
                                  (w == 2560 && h == 1600);
                const bool wide = (w == 3440 && h == 1440) || (w == 2560 && h == 1080);
                const bool super = (w == 5120 && h == 1440) || (w == 3840 && h == 1080) ||
                                   (w == 2560 && h == 720) || (w == 7680 && h == 2160);
                if (!((ratio == BbSettings::Aspect16x10 && tall) ||
                      (ratio == BbSettings::Aspect21x9 && wide) || (ratio == BbSettings::Aspect32x9 && super))) {
                    v.aspect_w = ratio == BbSettings::Aspect16x10 ? 1920 : ratio == BbSettings::Aspect21x9 ? 3440 : 5120;
                    v.aspect_h = ratio == BbSettings::Aspect16x10 ? 1200 : 1440;
                }
            }
        }
        std::snprintf(text, sizeof(text), "%s", BbSettings::AspectRatioNames[ratio]);
        return text;
    }
    default:
        return "";
    }
}

constexpr const char* Titles[] = {
    "Resolution", "Aspect Ratio", "Upscaling", "Upscale Quality",
    "FPS Unlock", "Motion Blur", "FPS Limit",
};

std::string Description(std::size_t index) {
    static constexpr const char* help[] = {
        "Choose the internal rendering resolution. Restart to apply.",
        "Choose a 16:9, 16:10, 21:9, or 32:9 layout. Restart to apply.",
        "Choose the image upscaler. Restart to apply.",
        "Choose DLSS image quality. Restart to apply.",
        "Unlock the game's frame rate. Restart to apply.",
        "Enable or disable motion blur. Restart to apply.",
        "Limit the number of frames rendered per second.",
        "Restart the game to apply your saved graphics settings.",
    };

    return index < std::size(help) ? help[index] : "";
}

void ShowVisual(u64 placeable, bool shown) {
    if (!placeable) return;
    auto& flags = *reinterpret_cast<u32*>(placeable + 0xc);
    flags = shown ? (flags & ~0x1400u) | 0x1000u : flags | 0x1400u;
    *reinterpret_cast<float*>(placeable + 0xe0) = shown ? 1.0f : 0.0f;
}

bool PlaceableNamed(u64 object, const char* expected) {
    u64 name = 0;
    std::string text;
    return object && SafeRead(reinterpret_cast<void*>(object+0x28), &name, 8) &&
           IsText(name, &text) && text == expected;
}

u64 FindNativeChild(u64 object, const char* name, int depth = 0) {
    if (!object || depth > 10) return 0;
    if (PlaceableNamed(object, name)) return object;
    // Shared assets keep their cloned placeables in an instance map, not _children.
    u64 instances = 0, values = 0;
    u32 bytes = 0;
    if (SafeRead(reinterpret_cast<void*>(object+0x10), &instances, 8) && instances &&
        SafeRead(reinterpret_cast<void*>(instances+0x10), &bytes, 4) &&
        bytes <= 8192 && bytes % 8 == 0 &&
        SafeRead(reinterpret_cast<void*>(instances+0x18), &values, 8) && values) {
        for (u32 offset = 0; offset < bytes; offset += 8) {
            u64 instance = 0;
            if (SafeRead(reinterpret_cast<void*>(values+offset), &instance, 8) &&
                PlaceableNamed(instance, name)) return instance;
        }
    }
    u64 table = 0, data = 0;
    u32 count = 0;
    if (!SafeRead(reinterpret_cast<void*>(object+0x48), &table, 8) || !table ||
        !SafeRead(reinterpret_cast<void*>(table+0x10), &count, 4) || count > 64 ||
        !SafeRead(reinterpret_cast<void*>(table+0x20), &data, 8) || !data) return 0;
    for (u32 i = 0; i < count; ++i) {
        u64 child = 0;
        if (SafeRead(reinterpret_cast<void*>(data+8*i), &child, 8)) {
            if (const u64 found = FindNativeChild(child, name, depth+1)) return found;
        }
    }
    return 0;
}

struct NativeControls { u64 root = 0, selector = 0, button = 0; };
u64 NativeInstance(u64 component, u64 source) {
    u64 map = 0, keys = 0, values = 0;
    u32 bytes = 0;
    if (!source || !SafeRead(reinterpret_cast<void*>(component+0x10), &map, 8) || !map ||
        !SafeRead(reinterpret_cast<void*>(map+0x10), &bytes, 4) || bytes > 8192 || bytes % 8 ||
        !SafeRead(reinterpret_cast<void*>(map+0x18), &values, 8) || !values ||
        !SafeRead(reinterpret_cast<void*>(map+0x28), &keys, 8) || !keys) return 0;
    for (u32 offset = 0; offset < bytes; offset += 8) {
        u64 key = 0, value = 0;
        if (SafeRead(reinterpret_cast<void*>(keys+offset), &key, 8) && key == source &&
            SafeRead(reinterpret_cast<void*>(values+offset), &value, 8)) return value;
    }
    return 0;
}

using IconMaterialFn = void PS4_SYSV_ABI (*)(u64, u64*);
IconMaterialFn apply_icon_material = nullptr;
using PlayAnimationFn = void PS4_SYSV_ABI (*)(u64, u64, const void*);
PlayAnimationFn play_animation = nullptr;
constexpr u64 PlayAnimation = 0x250b20;
constexpr u8 PlayAnimationCode[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53};
constexpr u64 IconMaterial = 0x12ae8a0;
constexpr u8 IconMaterialCode[] = {0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x49,0x89,0xf6,0x48,0x85,0xff};

void UpdateRestartIcon(const NativeControls& controls) {
    u64 list = 0, data = 0, nav = 0, material = 0;
    u32 count = 0;
    if (!apply_icon_material || !SafeRead(reinterpret_cast<void*>(controls.root+0xe8), &list, 8) || !list ||
        !SafeRead(reinterpret_cast<void*>(list+0x10), &count, 4) || count != 2 ||
        !SafeRead(reinterpret_cast<void*>(list+0x20), &data, 8) || !data ||
        !SafeRead(reinterpret_cast<void*>(data+8), &nav, 8) || !nav ||
        !SafeRead(reinterpret_cast<void*>(nav+0x158), &material, 8) || !material) return;
    for (u64 field : {0x148, 0x150}) {
        u64 source = 0;
        if (SafeRead(reinterpret_cast<void*>(nav+field), &source, 8)) {
            if (u64 icon = NativeInstance(controls.button, source)) apply_icon_material(icon, &material);
        }
    }
    u64 tag = 0;
    if (play_animation && SafeRead(reinterpret_cast<void*>(nav+0x160), &tag, 8) && tag) {
        // Native playback defaults from 0x9990b0: enabled and speed 1.
        alignas(8) u8 parameters[0x60] = {};
        parameters[0x2d] = 1;
        *reinterpret_cast<float*>(parameters+0x30) = 1;
        play_animation(controls.button, tag, parameters);
    }
}

void UnclipGraphicsList(u64 row) {
    u64 parent = 0;
    SafeRead(reinterpret_cast<void*>(row+0x40), &parent, 8);
    for (int depth = 0; parent && depth < 4; ++depth) {
        u32 flags = 0;
        if (!SafeRead(reinterpret_cast<void*>(parent+0xc), &flags, 4)) return;
        if (flags & 2u) { // igGuiPlaceable::_scissorToBound, native metadata bit 1.
            *reinterpret_cast<u32*>(parent+0xc) = (flags & ~2u) | 0x1000u;
            return;
        }
        if (!SafeRead(reinterpret_cast<void*>(parent+0x40), &parent, 8)) return;
    }
}
NativeControls Controls(u64 item) {
    u64 object = 0;
    SafeRead(reinterpret_cast<void*>(item+0x10), &object, 8);
    for (int depth = 0; object && depth < 12; ++depth) {
        if (PlaceableNamed(object, "BbGraphicsNativeSelector")) {
            NativeControls result;
            result.selector = object;
            SafeRead(reinterpret_cast<void*>(object+0x40), &result.root, 8);
            result.button = FindNativeChild(result.root, "BbGraphicsNativeRestart");
            return result;
        }
        u64 parent = 0;
        if (!SafeRead(reinterpret_cast<void*>(object+0x40), &parent, 8)) break;
        object = parent;
    }
    return {};
}

void UpdateWidgets(u64 item, std::size_t index) {
    const auto controls = Controls(item);
    if (!controls.selector || !controls.button) return;
    const bool setting = index < std::size(Titles);
    ShowVisual(controls.selector, setting);
    ShowVisual(controls.button, index == 7);
    if (setting) {
        SetText(FindNativeChild(controls.selector, "[NameText]"), Titles[index]);
        SetText(FindNativeChild(controls.selector, "[ItemText]"), Row(index, 0).c_str());
    } else if (index == 7) {
        SetText(FindNativeChild(controls.button, "[LevelListName]"), "RESTART TO APPLY");
        SetText(FindNativeChild(controls.button, "LevelListName_focused"), "RESTART TO APPLY");
    }
}

void PlaceDescription(u64 text, u64 canvas) {
    float size[2] = {};
    if (!SafeRead(reinterpret_cast<void*>(canvas+0x90), size, sizeof(size)) ||
        size[0] < 640 || size[1] < 480) return;
    const float width = std::min(860.0f, size[0]*0.5f-128.0f);
    *reinterpret_cast<float*>(text+0x90) = width;
    *reinterpret_cast<float*>(text+0x60) = 64.0f + width*0.5f;
    *reinterpret_cast<float*>(text+0x64) = size[1]-130.0f;
}

void UpdateDescription(u64 item, std::size_t index) {
    u64 placeable = 0;
    SafeRead(reinterpret_cast<void*>(item+0x10), &placeable, 8);
    // Walk this instance's ancestors; never retain a UI pointer across project closes.
    for (int depth = 0; placeable && depth < 12; ++depth) {
        u64 table = 0, data = 0, parent = 0;
        u32 count = 0;
        if (SafeRead(reinterpret_cast<void*>(placeable+0x48), &table, 8) && table &&
            SafeRead(reinterpret_cast<void*>(table+0x10), &count, 4) && count && count < 64 &&
            SafeRead(reinterpret_cast<void*>(table+0x20), &data, 8) && data) {
            u64 child = 0, label = 0;
            std::string name;
            if (SafeRead(reinterpret_cast<void*>(data+8*(count-1)), &child, 8) && child &&
                SafeRead(reinterpret_cast<void*>(child+0x28), &label, 8) &&
                Identifier(label, name) && name == "BbGraphicsDescription") {
                PlaceDescription(child, placeable);
                SetText(child, Description(index).c_str());
                return;
            }
        }
        if (!SafeRead(reinterpret_cast<void*>(placeable+0x40), &parent, 8)) break;
        placeable = parent;
    }
}

/// Settings on the page that apply only after a restart (as the settings menu counts them).
bool NeedsRestart() {
    auto& v = BbSettings::Get();
    bool restart = BbSettings::ResolutionNeedsRestart() || BbSettings::RenderSizeNeedsRestart();
    for (int g = 0; g < BbSettings::GamePatchCount; ++g) {
        restart |= v.game_patches[g] != v.startup_game_patches[g];
    }
    restart |= v.render_res != v.startup_render_res || v.temporal != v.startup_temporal ||
               v.dlss_quality != v.startup_dlss_quality || v.aspect_ratio != v.startup_aspect_ratio ||
               v.output_res != v.startup_output_res;
    return restart;
}

constexpr std::size_t SettingRows = 7;  ///< rows 0..6: the settings
constexpr std::size_t RestartRow = 7;   ///< "RESTART TO APPLY", shown while a change waits

/// A row's visibility: its placeable (CGuiBehaviorLanguageItem::_placeable, +0x30) and text
/// placeable (+0x10), with hidePlaceable/showPlaceable's flags (+0xc: 0x1400 hidden, 0x1000 shown).
void ShowRow(u64 item, bool shown) {
    {
        std::scoped_lock lock{rows_mutex};
        const auto index = std::find(rows.begin(), rows.end(), item) - rows.begin();
        if (std::size_t(index) < list_items.size() && list_items[index]) {
            // igGuiListItem::_isFocusable is bit 3 of +0x18 (native field metadata).
            auto& flags = *reinterpret_cast<u32*>(list_items[index] + 0x18);
            flags = shown ? flags | 8u : flags & ~8u;
        }
    }
    if (const auto controls = Controls(item); controls.root) {
        ShowVisual(controls.root, shown);
        return;
    }
    // The row's own placeable is the text placeable's parent (igGuiPlaceable +0x40): the bar.
    u64 text = 0, parent = 0;
    SafeRead(reinterpret_cast<void*>(item + 0x10), &text, 8);
    if (text) {
        SafeRead(reinterpret_cast<void*>(text + 0x40), &parent, 8);
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 8) {
        std::printf("Graphics tab: row %llx text %llx parent %llx %s\n",
                    static_cast<unsigned long long>(item), static_cast<unsigned long long>(text),
                    static_cast<unsigned long long>(parent), shown ? "shown" : "hidden");
    }
    for (u64 placeable : {parent, text}) {
        if (!placeable) {
            continue;
        }
        u32& flags = *reinterpret_cast<u32*>(placeable + 0xc);
        flags = shown ? ((flags & ~0x1400u) | 0x1000u) : (flags | 0x1400u);
        *reinterpret_cast<float*>(placeable + 0xe0) = shown ? 1.0f : 0.0f; // _alpha
    }
}

void UpdateRestartRow() {
    u64 item = 0;
    {
        std::scoped_lock lock{rows_mutex};
        if (rows.size() > RestartRow) {
            item = rows[RestartRow];
        }
    }
    if (item) {
        const bool needed = NeedsRestart();
        SetText(*reinterpret_cast<u64*>(item + 0x10), needed ? "RESTART TO APPLY" : " ");
        ShowRow(item, needed);
        UpdateWidgets(item, RestartRow);
    }
}

u64 PS4_SYSV_ABI LanguageItemHook(void* self, void* event, void* a2, void* a3, void* a4, void* a5) {
    const u64 item = reinterpret_cast<u64>(self);
    if (event && IsA(event, InputEventMeta)) {
        const u32 type = *reinterpret_cast<u32*>(static_cast<u8*>(event) + 0x18);
        std::size_t index = SIZE_MAX;
        {
            std::scoped_lock lock{rows_mutex};
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (rows[i] == item) {
                    index = i;
                }
            }
        }
        if (index == RestartRow && type == 8 && NeedsRestart()) {
            BbSettings::Save();
            std::printf("Graphics tab: restarting to apply\n");
            std::fflush(stdout);
            runtime_restart();
            return 0;
        }
        if (type == 9) {
            return language_item_original(self, event, a2, a3, a4, a5);
        }
        if (index != SIZE_MAX && index >= SettingRows) {
            return 0; // the rows past the settings do nothing
        }
        const int direction = (type == 2 || type == 6) ? -1 :
                              (type == 3 || type == 7) ? 1 : 0;
        if (index != SIZE_MAX && direction) {
            const std::string text = Row(index, direction);
            std::vector<u64> all;
            {
                std::scoped_lock lock{rows_mutex};
                all = rows;
            }
            for (std::size_t r = 0; r < all.size() && r < SettingRows; ++r) {
                const std::string t = r == index ? text : Row(r, false);
                SetText(*reinterpret_cast<u64*>(all[r] + 0x10), t.c_str());
                UpdateWidgets(all[r], r);
            }
            BbSettings::Save();
            UpdateRestartRow();
            UpdateDescription(item, index);
            std::printf("Graphics tab: %s\n", text.c_str());
            std::fflush(stdout);
            return 0; // no language change, no dialog
        }
        if (index != SIZE_MAX && type == 8) return 0;
        if (index != SIZE_MAX && type != 8) {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1) < 20) {
                std::printf("Graphics tab: row %zu input %u\n", index, type);
            }
        }
        return language_item_original(self, event, a2, a3, a4, a5);
    }
    const u64 result = language_item_original(self, event, a2, a3, a4, a5);
    if (event && IsA(event, GainFocusMeta)) {
        std::size_t index;
        {
            std::scoped_lock lock{rows_mutex};
            index = std::find(rows.begin(), rows.end(), item) - rows.begin();
        }
        UpdateDescription(item, index);
    }
    if (event && IsA(event, InitEventMeta)) {
        std::size_t index;
        {
            std::scoped_lock lock{rows_mutex};
            index = std::find(rows.begin(), rows.end(), item) - rows.begin();
            if (index == rows.size()) {
                rows.push_back(item);
                list_items.push_back(*reinterpret_cast<u64*>(static_cast<u8*>(event) + 0x20));
            }
        }
        const std::string text = Row(index, false);
        SetText(*reinterpret_cast<u64*>(item + 0x10), text.empty() ? " " : text.c_str());
        UpdateWidgets(item, index);
        if (index == RestartRow) UpdateRestartIcon(Controls(item));
        const auto controls = Controls(item);
        if (index == 0 && controls.root) UnclipGraphicsList(controls.root);
        std::printf("Graphics native row %zu: selector=%llx button=%llx value=%llx title=%llx\n",
                    index, (unsigned long long)controls.selector, (unsigned long long)controls.button,
                    (unsigned long long)FindNativeChild(controls.selector, "[ItemText]"),
                    (unsigned long long)FindNativeChild(controls.selector, "[NameText]"));
        if (index == 0) UpdateDescription(item, index);
        if (index == RestartRow) {
            UpdateRestartRow();
        } else if (index > RestartRow) {
            ShowRow(item, false); // no setting: the row is hidden
        }
        std::printf("Graphics tab: row %zu \"%s\"\n", index, text.c_str());
        std::fflush(stdout);
    }
    return result;
}

VscFn load_map_original = nullptr;
using StringPinFn = void PS4_SYSV_ABI (*)(char**, void*, void*, const void*);

u64 PS4_SYSV_ABI LoadMapHook(void* context, void* args, void* a2, void* a3, void* a4, void* a5) {
    void** pins = nullptr;
    void* pin = nullptr;
    char* map = nullptr;
    if (args && SafeRead(static_cast<u8*>(args) + 0x10, &pins, sizeof(pins)) && pins &&
        SafeRead(pins, &pin, sizeof(pin)) && pin) {
        reinterpret_cast<StringPinFn>(image + StringPin)(&map, context, pin, image + string_pin_default);
    }
    const bool quit = map && std::strstr(map, QuitMap);
    std::printf("Graphics tab: load map %s%s\n", map ? map : "(none)", quit ? " -> quit" : "");
    std::fflush(stdout);
    DropString(map);
    if (quit) {
        BbOverlay::QuitGame();
        return 0;
    }
    return load_map_original(context, args, a2, a3, a4, a5);
}

/// The main menu's Quit Game button: its navigation item's text (_itemText, +0xe8).
[[maybe_unused]] bool IsQuitButton(void* object) { // the Quit Game button, off since v0.7.1
    const u64 item = object ? NavItem(reinterpret_cast<u64>(object)) : 0;
    u64 text = 0;
    char label[16] = {};
    if (!item || !SafeRead(reinterpret_cast<void*>(item + 0xe8), &text, 8) || !text ||
        !SafeRead(reinterpret_cast<void*>(text), label, sizeof(label) - 1)) {
        return false;
    }
    for (const char* name : {"Quit Game", "Quitter", "Salir", "Sair"}) {
        if (!std::strncmp(label, name, std::strlen(name))) {
            return true;
        }
    }
    return false;
}

u64 PS4_SYSV_ABI HideHook(void* context, void* args, void* a2, void* a3, void* a4, void* a5) {
    void* object = Input(context, args);
    // OCTR v0.7.1: no Quit Game button (scripts/graphics_tab.py), so no placeable is kept shown
    // for it: its label test ("Salir", "Sair"...) also matched the game's own buttons.
    if (IsTab(object, "hide")) {
        Relabel(object);
        return 0; // stays shown
    }
    return hide_original(context, args, a2, a3, a4, a5);
}

u64 PS4_SYSV_ABI ShowHook(void* context, void* args, void* a2, void* a3, void* a4, void* a5) {
    IsTab(Input(context, args), "show");
    return show_original(context, args, a2, a3, a4, a5);
}

u64 PS4_SYSV_ABI Open2Hook(void* context, void* args, void* a2, void* a3, void* a4, void* a5) {
    int where = -1;
    void* object = Input(context, args);
    Dump("open", args, object);
    const std::string name = Name(object, &where);
    const bool ours = name == Project || IsLanguageProject(object);
    if (open_logged.fetch_add(1) < 40) {
        std::printf("Graphics tab: open %s (+0x%x)%s\n", name.empty() ? "(no name)" : name.c_str(),
                    where, ours ? " -> settings menu" : "");
        std::fflush(stdout);
    }
    {
        std::scoped_lock lock{last_mutex};
        last_opened = name;
    }
    if (ours) {
        // The Graphics page: the language screen opens; its rows are filled again.
        std::scoped_lock lock{rows_mutex};
        rows.clear();
        list_items.clear();
    }
    return open2_original(context, args, a2, a3, a4, a5);
}

void* AllocateCode(std::size_t size) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

/// movabs rax, target; jmp rax (12 bytes).
void WriteJump(u8* at, const void* target) {
    at[0] = 0x48;
    at[1] = 0xb8;
    const u64 t = reinterpret_cast<u64>(target);
    std::memcpy(at + 2, &t, 8);
    at[10] = 0xff;
    at[11] = 0xe0;
}

/// The function at `offset` jumps to `hook`; the returned stub runs the moved prologue and
/// continues in the function.
VscFn Detour(u8* stub, u64 offset, const u8* prologue, std::size_t size, const void* hook) {
    std::memcpy(stub, prologue, size);
    WriteJump(stub + size, image + offset + size);
    WriteJump(image + offset, hook);
    return reinterpret_cast<VscFn>(stub);
}
} // namespace

void PatchImage(unsigned char* base, std::uint64_t size) {
    if (!BbSettings::Get().graphics_tab) {
        return;
    }
    // Recognize both 1.21 layouts before installing anything; helpers must match too.
    if (StringPool + 8 > size) {
        std::printf("Graphics tab: image too small, off\n");
        return;
    }
    u64 shift;
    if (!std::memcmp(base + RegionQueryCall, RegionQueryCode, sizeof(RegionQueryCode))) {
        shift = 0;
    } else if (!std::memcmp(base + RegionQueryCall - 0x10, RegionQueryCode, sizeof(RegionQueryCode))) {
        shift = 0x10;
    } else {
        std::printf("Graphics tab: unknown region query, off\n");
        return;
    }
    const u64 pin_default = shift ? 0x1df8cd0 : StringPinDefault;
    const u8* default_lea = base + LoadMap2 - shift + 0x3e;
    s32 displacement;
    std::memcpy(&displacement, default_lea + 3, sizeof(displacement));
    if (std::memcmp(base + PlayAnimation, PlayAnimationCode, sizeof(PlayAnimationCode)) ||
        std::memcmp(base + IconMaterial - shift, IconMaterialCode, sizeof(IconMaterialCode)) ||
        std::memcmp(base + Open2 - shift, Open2Prologue, sizeof(Open2Prologue)) ||
        std::memcmp(base + Hide, PlaceablePrologue, sizeof(PlaceablePrologue)) ||
        std::memcmp(base + Show, PlaceablePrologue, sizeof(PlaceablePrologue)) ||
        std::memcmp(base + LanguageItemEvent - shift, Open2Prologue, sizeof(Open2Prologue)) ||
        std::memcmp(base + LoadMap2 - shift, PlaceablePrologue, sizeof(PlaceablePrologue)) ||
        std::memcmp(base + PinGetter, "\x48\x89\xf8\x48\x85\xf6\x74\x12", 8) ||
        std::memcmp(base + StringPin, "\x55\x48\x89\xe5\x53\x50\x48\x89\xf0", 9) ||
        std::memcmp(base + Release - shift, "\x48\x85\xff\x74\x05\xe9", 6) ||
        std::memcmp(base + Intern - shift, Open2Prologue, sizeof(Open2Prologue)) ||
        std::memcmp(base + Unref - shift, Open2Prologue, sizeof(Open2Prologue)) ||
        std::memcmp(default_lea, "\x48\x8d\x0d", 3) ||
        LoadMap2 - shift + 0x45 + displacement != pin_default) {
        std::printf("Graphics tab: unexpected game code, off\n");
        return;
    }
    u8* stubs = static_cast<u8*>(AllocateCode(4096));
    if (!stubs) {
        std::printf("Graphics tab: no code memory, off\n");
        return;
    }
    image = base;
    code_shift = shift;
    string_pin_default = pin_default;
    apply_icon_material = reinterpret_cast<IconMaterialFn>(base + IconMaterial - shift);
    play_animation = reinterpret_cast<PlayAnimationFn>(base + PlayAnimation);
    open2_original = Detour(stubs, Open2 - shift, Open2Prologue, sizeof(Open2Prologue),
                            reinterpret_cast<const void*>(&Open2Hook));
    hide_original = Detour(stubs + 64, Hide, PlaceablePrologue, sizeof(PlaceablePrologue),
                           reinterpret_cast<const void*>(&HideHook));
    show_original = Detour(stubs + 128, Show, PlaceablePrologue, sizeof(PlaceablePrologue),
                           reinterpret_cast<const void*>(&ShowHook));
    language_item_original = Detour(stubs + 192, LanguageItemEvent - shift, Open2Prologue,
                                    sizeof(Open2Prologue),
                                    reinterpret_cast<const void*>(&LanguageItemHook));
    load_map_original = Detour(stubs + 256, LoadMap2 - shift, PlaceablePrologue, sizeof(PlaceablePrologue),
                               reinterpret_cast<const void*>(&LoadMapHook));
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), stubs, 320);
#endif
    if (!open2_original || !hide_original || !show_original || !language_item_original ||
        !load_map_original) {
        std::printf("Graphics tab: unexpected game code, the menus stay as they are\n");
        return;
    }
    runtime_graphics_tab_ready = 1;
    std::printf("Graphics tab: on (Options menu, fifth tab; %s 1.21)\n", shift ? "EU" : "USA");
}

} // namespace BbGraphicsTab
