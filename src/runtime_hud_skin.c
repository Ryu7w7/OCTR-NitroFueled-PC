/* Skin-specific race HUD and minimap portraits (issue #203).
 *
 * scripts/skin_portraits.py puts an optional RacerHud/Minimap pair handle in
 * CDriverSkinData. Native UI binders then use the active
 * skin's handle for this racer only; racers without one keep native behavior.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime_replay_internal.h"

#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef void *ABI portrait_fn(void *racer);
typedef void ABI handle_fn(void **slot, void *object);
typedef void ABI minimap_fn(void *behavior, void *event, void *target);
typedef void *ABI meta_fn(void *object);

static portrait_fn *native_leader_portrait;
static handle_fn *native_leader_handle;
static minimap_fn *native_minimap;
static void **racer_setup_meta;
static _Thread_local void *leader_material;
static _Thread_local void *minimap_handle;

static void *player_of_racer(void *racer) {
    return racer ? rr_handle_object(&RR_AT(racer, 0x10, void *)) : NULL;
}

static void *racer_of_entity_handle(void **handle) {
    void *entity = handle ? rr_handle_object(handle) : NULL;
    return entity ? rr_racer_of(entity) : NULL;
}

static void racer_look(void *racer, const char **driver, void **outfit) {
    *driver = NULL;
    *outfit = NULL;
    if (!racer) return;
    void *player = player_of_racer(racer);
    *driver = player ? RR_AT(player, 0x88, const char *) : NULL;
    *outfit = player ? rr_handle_object(&RR_AT(player, 0x90, void *)) : NULL;
    if (*driver && *outfit) return;
    void *kart = NULL;
    for (int i = 0; i < 8 && !kart; ++i) {
        void *candidate = rr_kart_of(i, NULL);
        if (candidate && rr_racer_of(candidate) == racer) kart = candidate;
    }
    void *manager = rr_ptr(RR_D_RACER_MANAGER);
    void *infos = manager ? RR_AT(manager, 0x28, void *) : NULL;
    int id = -1;
    for (int i = 0; i < rr_count(infos); ++i) {
        void *info = rr_item(infos, i);
        if (info && RR_AT(info, 0x38, void *) == kart) {
            *driver = RR_AT(info, 0x18, const char *);
            id = RR_AT(info, 0xc, int32_t);
            break;
        }
    }
    void *persistent = manager ? RR_AT(manager, 0x30, void *) : NULL;
    for (int i = 0; id >= 0 && i < rr_count(persistent) && !*outfit; ++i) {
        void *info = rr_item(persistent, i);
        if (info && RR_AT(info, 0x58, int32_t) == id)
            *outfit = rr_handle_object(&RR_AT(info, 0x20, void *));
    }
}

static void *skin_portrait(void *racer, int index) {
    const char *driver = NULL;
    void *outfit = NULL;
    racer_look(racer, &driver, &outfit);
    void *data = driver ? rr_game_data(RR_DRIVERS, driver) : NULL;
    void *skins = data ? RR_AT(data, 0x98, void *) : NULL;
    for (int i = 0; outfit && i < rr_count(skins); ++i) {
        void *skin = rr_item(skins, i);
        if (skin && RR_AT(skin, 0x68, void *) == outfit) {
            void *hud = rr_handle_object(&RR_AT(skin, 0x78, void *));
            if (!index) return hud;
            return NULL;
        }
    }
    return NULL;
}

static void *ABI leader_portrait(void *racer) {
    void *base = native_leader_portrait(racer);
    leader_material = skin_portrait(racer, 0);
    return base;
}

static void ABI leader_handle(void **slot, void *material) {
    native_leader_handle(slot, leader_material ? leader_material : material);
    leader_material = NULL;
}

static void ABI bind_minimap(void *behavior, void *event, void *target) {
    void **slot = NULL;
    void *saved = NULL;
    if (event && racer_setup_meta && ((meta_fn *)(*(void ***)event)[0])(event) == *racer_setup_meta) {
        void *racer = racer_of_entity_handle(&RR_AT(event, 0x10, void *));
        if (!racer) racer = racer_of_entity_handle(&RR_AT(event, 0x30, void *));
        void *material = skin_portrait(racer, 1);
        if (material) {
            native_leader_handle(&minimap_handle, material);
            slot = &RR_AT(event, 0x38, void *);
            saved = *slot;
            *slot = minimap_handle;
        }
    }
    native_minimap(behavior, event, target);
    if (slot) *slot = saved;
}

static int writable(void *address, size_t length) {
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(address, length, PAGE_EXECUTE_READWRITE, &old) != 0;
#else
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *start = (void *)((uintptr_t)address & ~(uintptr_t)(page - 1));
    return !mprotect(start, (char *)address + length - (char *)start,
                     PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
}

int runtime_hud_skin_install(int eu) {
    const size_t shift = eu ? 0x10 : 0;
    native_leader_portrait = (portrait_fn *)rr_game(0xff1770 - shift);
    native_leader_handle = (handle_fn *)rr_game(0x14bea20 - shift);
    native_minimap = (minimap_fn *)rr_game(0x9d17e0 - shift);
    racer_setup_meta = (void **)rr_game(0x2ac04d0);
    void **minimap_vtable = (void **)rr_game(0x2544f68);
    const int leader = rr_redirect_call(rr_game(0x9dd347 - shift), (unsigned char *)native_leader_portrait,
                                        (void (*)(void))leader_portrait);
    const int material = rr_redirect_call(rr_game(0x9dd4ed - shift), (unsigned char *)native_leader_handle,
                                          (void (*)(void))leader_handle);
    const int minimap = *minimap_vtable == (void *)native_minimap && writable(minimap_vtable, sizeof *minimap_vtable);
    if (minimap) *minimap_vtable = (void *)bind_minimap;
    if (!leader || !material || !minimap) {
        puts("Skin portraits: native HUD/minimap code not expected; disabled");
        return 0;
    }
    puts("Skin portraits: race leaders and minimap use the active skin portrait");
    return 1;
}
