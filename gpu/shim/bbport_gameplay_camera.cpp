// Options > Gameplay. The native toggle owns navigation, focus and animations.
// Choose the initial CKartCamera mode once when the race camera is initialized.
// The game's normal camera-switch input remains responsible for later changes.
#include "bbport_gameplay_camera.h"
#include "bbport_settings.h"
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace BbGameplayCamera {
namespace {
constexpr unsigned CameraOption = 0x43414d;
using OptionFn = void (__attribute__((sysv_abi)) *)(unsigned char*, void*);
using CameraFn = std::uintptr_t (__attribute__((sysv_abi)) *)(unsigned char*);
OptionFn original_read, original_write;
CameraFn original_init;
bool reversed_presets;

bool is_camera(const unsigned char* selector) {
    unsigned id;
    std::memcpy(&id, selector + 0xdc, sizeof id);
    return id == CameraOption;
}
void __attribute__((sysv_abi)) read_option(unsigned char* selector, void* owner) {
    if (!is_camera(selector)) return original_read(selector,owner);
    const int choice = BbSettings::Get().starting_camera;
    selector[0xd8] = choice < 0 ? reversed_presets : (choice == 1);
}
void __attribute__((sysv_abi)) write_option(unsigned char* selector, void* owner) {
    if (!is_camera(selector)) return original_write(selector,owner);
    const int choice = selector[0xd8] != 0;
    if (BbSettings::Get().starting_camera.exchange(choice) != choice) {
        BbSettings::Save();
        std::printf("Starting camera: saved %s\n",choice ? "Far" : "Default");
    }
}
std::uintptr_t __attribute__((sysv_abi)) init_camera(unsigned char* camera) {
    const int choice = BbSettings::Get().starting_camera;
    if (choice >= 0) {
        const int mode = (choice == 1) != reversed_presets;
        std::memcpy(camera+0x250,&mode,sizeof mode);
    }
    return original_init(camera);
}

// Locate one pair of related entries in the relocated vtables, before the
// loader applies page permissions. EU and USA have slightly different layouts.
std::uintptr_t* unique_pair(unsigned char* image, std::size_t begin, std::size_t end,
                            std::uintptr_t a, std::uintptr_t b, std::size_t distance) {
    std::uintptr_t* found = nullptr;
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    for (std::size_t off=begin; off+(distance+1)*sizeof(std::uintptr_t)<=end; off+=sizeof(std::uintptr_t)) {
        auto* slot = reinterpret_cast<std::uintptr_t*>(image+off);
        if (slot[0] == base+a && slot[distance] == base+b) {
            if (found) return nullptr;
            found = slot;
        }
    }
    return found;
}
}

void Install(unsigned char* image, std::uint64_t size) {
    if (size < 0x2800000) return;
    constexpr unsigned char cmp[] = {0x49,0x8d,0x87,0x70,0x02,0,0,0x41,0x83,0xbf,0x50,0x02,0,0};
    constexpr unsigned char select[] = {0x48,0x0f,0x44,0xc2,0x48,0x0f,0x44,0xce,0x41,0x80,0xbf,0x54,0x02,0,0,0};
    constexpr unsigned char init[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xec,0x20};
    constexpr unsigned char read[] = {0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x89,0xfb,0x80,0xbb,0xe0,0,0,0,0};
    constexpr unsigned char write[] = {0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x89,0xfb,0x44,0x8a,0xb3,0xd8,0,0,0};
    for (unsigned opt_shift : {0u,16u}) {
        const auto get = 0x126e250u-opt_shift, set = 0x126e2f0u-opt_shift;
        if (std::memcmp(image+get,read,sizeof read) || std::memcmp(image+set,write,sizeof write)) continue;
        auto* options = unique_pair(image,0x2400000,0x2800000,get,set,1);
        if (!options) continue;
        for (unsigned cam_shift : {0u,16u}) {
            const auto start = 0xc88960u-cam_shift, update = 0xc88b30u-cam_shift;
            const auto* site = image+0xc89103-cam_shift;
            if (std::memcmp(site,cmp,sizeof cmp) || site[14]>1 || std::memcmp(site+15,select,sizeof select) ||
                std::memcmp(image+start,init,sizeof init)) continue;
            auto* camera = unique_pair(image,0x2400000,0x2800000,start,update,2);
            if (!camera) continue;
            reversed_presets = site[14] == 1; // separately installed Far Camera Default mod
            original_read = reinterpret_cast<OptionFn>(options[0]);
            original_write = reinterpret_cast<OptionFn>(options[1]);
            original_init = reinterpret_cast<CameraFn>(*camera);
            options[0] = reinterpret_cast<std::uintptr_t>(&read_option);
            options[1] = reinterpret_cast<std::uintptr_t>(&write_option);
            *camera = reinterpret_cast<std::uintptr_t>(&init_camera);
            std::puts("Starting camera: Gameplay selector and race camera connected");
            return;
        }
    }
    std::puts("Starting camera: unrecognized game layout");
}
}
