/* Native wall classification, Itemless collision rules and starting positions. */
#define _GNU_SOURCE
#include "runtime.h"
#include <math.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef void (ABI *Contact)(void *, void *, void *);
typedef void (ABI *StopBoost)(void *);
typedef void *(ABI *FindRacer)(void *, void *);
static Contact native_contact;
static StopBoost native_stop;
static FindRacer find_racer;
static void **racers;
static _Thread_local void *contact_owner;
typedef unsigned char (ABI *LayerFilter)(void *, const char *, int);
static LayerFilter native_layer_filter;
typedef void (ABI *PlaceRacer)(void *, void *, void *);
typedef void (ABI *ResolveHandle)(void **);
static PlaceRacer native_place_racer;
static ResolveHandle resolve_start_handle;
typedef void (ABI *Lifecycle)(void *);
typedef void *(ABI *Component)(void *, void *, int);
typedef void *(ABI *BodySignal)(void *, int, int);
typedef void (ABI *Subscribe)(void *, void *);
typedef void (ABI *BodyFlags)(void *, int, unsigned, unsigned);
static Lifecycle native_initialize, native_uninitialize;
static Component get_component;
static BodySignal body_signal;
static Subscribe subscribe_contact, unsubscribe_contact, disable_contact;
static BodyFlags enable_body_flags;
static void **kart_controller_type;
static void **physics_type;

int runtime_itemless_collisions_disabled(void) {
    return runtime_speed_race_active() &&
        (!runtime_online_mode_in_private() || !runtime_online_mode_itemless_collision());
}

/* Only asset/spawn decisions see Time Trial; the multiplayer race mode stays intact. A replay of a race
 * without crates (Time Trial, Itemless) loads the track as that race did. */
static int ABI track_mode(int mode) {
    return (mode == 1 || mode == 6) && (runtime_speed_race_active() || runtime_replay_no_crates()) ? 2 : mode;
}

static unsigned char ABI layer_filter(void *rule, const char *name, int mode) {
    return native_layer_filter(rule, name, track_mode(mode));
}

static void *ptr(const void *p, size_t off) {
    void *v;
    memcpy(&v, (const unsigned char *)p + off, sizeof v);
    return v;
}

static float ABI wall_axis(void *response, const float *normal) {
    const float original=fabsf(normal[2]);
    void *entity=ptr(response,0x18);
    void *kart=entity ? get_component(entity,*kart_controller_type,0) : NULL;
    if (!kart || !((unsigned char *)kart)[0x214] || !((unsigned char *)kart)[0x215])
        return original;
    float ground[3],dot=0,length2=0;
    memcpy(ground,(unsigned char *)kart+0x208,sizeof ground);
    for (unsigned i=0;i<3;++i) {
        dot+=normal[i]*ground[i];
        length2+=ground[i]*ground[i];
    }
    /* Keep the native 60-degree test, but recognize continuation of the supported bank. */
    if (isfinite(dot) && isfinite(length2) && length2>0 && dot*dot>=.25f*length2)
        return 1;
    return original;
}

static void *start_waypoint(void **slot) {
    for (int depth = 0; depth < 16 && *slot; ++depth) {
        unsigned flags;
        memcpy(&flags, *slot, 4);
        if (flags & 0x10000000) resolve_start_handle(slot);
        if (!*slot) return NULL;
        memcpy(&flags, *slot, 4);
        void *object = ptr(*slot, 0x28);
        if (!(flags & 0x20000000) || !object) return object;
        slot = (void **)((unsigned char *)object + 0x10);
    }
    return NULL;
}

static void ABI place_racer(void *self, void *racer, void *waypoint) {
    unsigned char aligned[0x28];
    void *data = self ? ptr(self, 0x10) : NULL;
    void *list = data ? ptr(data, 0x10) : NULL;
    int count = 0;
    if (list) memcpy(&count, (unsigned char *)list + 12, 4);
    void **slots = list ? ptr(list, 0x20) : NULL;
    if (runtime_itemless_collisions_disabled() && waypoint && slots && count == 8) {
        void *points[8];
        int index = -1;
        for (int i = 0; i < 8; ++i) {
            points[i] = start_waypoint(slots + i);
            if (points[i] == waypoint) index = i;
        }
        if (index >= 0 && points[0] && points[3]) {
            float first[3], last[3], position[3];
            memcpy(first, (unsigned char *)points[0] + 0x10, sizeof first);
            memcpy(last, (unsigned char *)points[3] + 0x10, sizeof last);
            int valid = 1;
            for (int i = 0; i < 3; ++i) {
                position[i] = first[i] + (last[i] - first[i]) * ((float)index / 7.0f);
                if (!isfinite(position[i])) valid = 0;
            }
            if (valid) {
                /* All 40 tracks use two rows of four; keep the authored front-row width. */
                memcpy(aligned, waypoint, sizeof aligned);
                memcpy(aligned + 0x10, position, sizeof position);
                memcpy(aligned + 0x1c, (unsigned char *)points[0] + 0x1c, 12);
                waypoint = aligned;
            }
        }
    }
    native_place_racer(self, racer, waypoint); /* Native ground raycast and network ownership. */
}

static int is_racer(void *entity) {
    void *racer = entity && *racers ? find_racer(*racers, entity) : NULL;
    return racer != NULL;
}

static void ABI filter_contact(void *self, void *context, void *collision) {
    if ((!runtime_itemless_collisions_disabled() && !runtime_public_match_active()) || !context || !collision) return;
    void *world = ptr(context, 0);
    void *bodies = world ? ptr(world, 0x20) : NULL;
    void *cache = ptr(collision, 0x20);
    if (!bodies || !cache) return;
    int own, first, second;
    unsigned capacity;
    memcpy(&own, (unsigned char *)self + 0x58, 4);
    memcpy(&first, (unsigned char *)collision + 8, 4);
    memcpy(&second, (unsigned char *)collision + 12, 4);
    memcpy(&capacity, (unsigned char *)world + 0x2c, 4);
    int other_index = first == own ? second : second == own ? first : -1;
    if (other_index < 0 || (unsigned)other_index >= (capacity & 0x3fffffff)) return;
    unsigned char *body = (unsigned char *)bodies + (size_t)other_index * 0x90;
    if (!(body[0x42] & 4)) return;
    void *other = (void *)((uintptr_t)ptr(body, 0x88) & ~(uintptr_t)3);
    void *owner = ptr(self, 0x18);
    if (owner != other && ((runtime_itemless_collisions_disabled() && is_racer(owner) && is_racer(other)) ||
        runtime_online_mode_departed_kart(owner) || runtime_online_mode_departed_kart(other)))
        disable_contact(cache, world);
}

static void filter_subscription(void *self, int add) {
    void *physics = get_component(ptr(self, 0x18), *physics_type, 0);
    void *world = physics ? ptr(physics, 0x48) : NULL;
    int body;
    memcpy(&body, (unsigned char *)self + 0x58, 4);
    if (!world || body < 0 || body == 0x7fffffff) return;
    if (add) enable_body_flags(world, body, 0x40, 0);
    void *signal = body_signal(world, 2, body);
    unsubscribe_contact(signal, self);
    if (add) {
        subscribe_contact(signal, self);
        void *node = (void *)((uintptr_t)ptr(signal, 0) & ~(uintptr_t)3);
        Contact callback = filter_contact;
        memcpy((unsigned char *)node + 0x18, &callback, sizeof callback);
    }
}

static void ABI initialize(void *self) {
    native_initialize(self);
    /* Event 2 owns the collision cache; event 3 exposes contact data instead. */
    if (runtime_itemless_collisions_disabled() || runtime_public_match_active()) filter_subscription(self, 1);
}

static void ABI uninitialize(void *self) {
    filter_subscription(self, 0);
    native_uninitialize(self);
}

static void ABI contact(void *self, void *context, void *collision) {
    void *previous = contact_owner;
    contact_owner = NULL;
    if ((runtime_speed_race_active() || runtime_public_match_active()) && context && collision) {
        int side;
        memcpy(&side, (unsigned char *)context + 0x20, 4);
        void *world = ptr(context, 0);
        void *bodies = world ? ptr(world, 0x20) : NULL;
        if (bodies && (side == 0 || side == 1)) {
            int index;
            memcpy(&index, (unsigned char *)collision + 8 + 4*(1-side), 4);
            if (index >= 0) {
                unsigned char *body = (unsigned char *)bodies + (size_t)index * 0x90;
                /* Native contact handler only treats flag 4 bodies as entities. */
                if (body[0x42] & 4) {
                    void *other = (void *)((uintptr_t)ptr(body, 0x88) & ~(uintptr_t)3);
                    void *owner = ptr(self, 0x18);
                    if (owner != other && (runtime_online_mode_departed_kart(owner) || runtime_online_mode_departed_kart(other))) {
                        contact_owner = previous;
                        return;
                    }
                    if (owner != other && is_racer(owner) && is_racer(other)) {
                        if (runtime_itemless_collisions_disabled()) {
                            contact_owner = previous;
                            return;
                        }
                        contact_owner = owner;
                    }
                }
            }
        }
    }
    native_contact(self, context, collision); /* Keep impulses, steering and collision messages. */
    contact_owner = previous;
}

static void ABI stop_boost(void *boost) {
    if (!contact_owner || ptr(boost, 0x18) != contact_owner)
        runtime_race_options_stop_boost(boost,native_stop);
}

static void jump(unsigned char *at, const void *target) {
    memcpy(at, "\xff\x25\0\0\0\0", 6);
    memcpy(at + 6, &target, 8);
}

static size_t wall_axis_stub(unsigned char *code, void *not_wall, void *resume) {
    static const unsigned char instructions[] = {
        0x50,0x51,0x52,0x56,0x57,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,
        0x48,0x81,0xec,0x08,0x02,0,0, 0x48,0x0f,0xae,0x04,0x24,
        0x48,0x8b,0xbd,0x10,0xff,0xff,0xff, /* response at rbp-f0 */
        0x48,0x8d,0xb5,0x20,0xff,0xff,0xff, /* ray normal at rbp-e0 */
        0x48,0xb8, 0,0,0,0,0,0,0,0, 0xff,0xd0,
        0xf3,0x0f,0x11,0x84,0x24,0,0x02,0,0, /* preserve returned axis across fxrstor */
        0x48,0x0f,0xae,0x0c,0x24, 0xf3,0x0f,0x10,0x8c,0x24,0,0x02,0,0,
        0x48,0x81,0xc4,0x08,0x02,0,0,
        0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5f,0x5e,0x5a,0x59,0x58,
        0xc5,0xf8,0x2e,0xd1, 0x77,0x0e, /* native compare; preserve unordered rejection */
        0xff,0x25,0,0,0,0, 0,0,0,0,0,0,0,0,
        0x48,0x8b,0x9d,0x10,0xff,0xff,0xff,
        0xff,0x25,0,0,0,0, 0,0,0,0,0,0,0,0
    };
    memcpy(code,instructions,sizeof instructions);
    void *handler=(void *)wall_axis;
    memcpy(code+41,&handler,8);
    jump(code+100,not_wall);
    jump(code+121,resume);
    return sizeof instructions;
}

/* Resume the native crate initializer with its effective mode in ECX and data in RAX. */
static size_t crate_mode_stub(unsigned char *code, void *race_manager, void *resume) {
    const unsigned char push[] = {0x50,0x57,0x56,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53};
    const unsigned char pop[] = {0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5a,0x5e,0x5f,0x58};
    size_t at = 0;
    memcpy(code + at, push, sizeof push); at += sizeof push;
    memcpy(code + at, "\x48\x81\xec\x00\x02\x00\x00\x48\x0f\xae\x04\x24\x48\xb8", 14); at += 14;
    memcpy(code + at, &race_manager, 8); at += 8;
    memcpy(code + at, "\x48\x8b\x00\x8b\xb8\x4c\x0d\x00\x00\x48\xb8", 11); at += 11;
    void *fn = (void *)track_mode;
    memcpy(code + at, &fn, 8); at += 8;
    memcpy(code + at, "\xff\xd0\x89\xc1\x48\x0f\xae\x0c\x24\x48\x81\xc4\x00\x02\x00\x00", 16); at += 16;
    memcpy(code + at, pop, sizeof pop); at += sizeof pop;
    memcpy(code + at, "\x49\x8b\x44\x24\x10", 5); at += 5;
    jump(code + at, resume);
    return at + 14;
}

static int writable(void *address, size_t length) {
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(address, length, PAGE_EXECUTE_READWRITE, &old) != 0;
#else
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *start = (void *)((uintptr_t)address & ~(uintptr_t)(page-1));
    return !mprotect(start, (unsigned char *)address + length - (unsigned char *)start,
                     PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
}

int runtime_itemless_start(unsigned char *image, size_t size) {
    const unsigned char collision_code[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x81,0xec,0x08,0x01,0,0};
    const unsigned char stop_code[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x28};
    const unsigned char layer_code[] = {0x55,0x48,0x89,0xe5,0x41,0x56,0x53,
        0x41,0x89,0xd6,0x48,0x89,0xfb,0x48,0x85,0xf6};
    /* Small loader images do not contain CTR's native collision hooks. */
    if (size < 0x2ad4b78) return 1;
    const unsigned char place_code[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x58};
    size_t shift = memcmp(image + 0xe916b0, collision_code, sizeof collision_code) ? 0x10 : 0;
    unsigned char *c = image + 0xe916b0 - shift, *s = image + 0xe92e90 - shift;
    unsigned char *layer = image + 0xfe44c0 - shift;
    unsigned char *crate = image + 0xeebaa6 - shift;
    unsigned char *place = image + 0xf31b80 - shift;
    unsigned char crate_code[] = {0x48,0x8b,0x0d,0x9b,0x23,0xbe,0x01,
        0x49,0x8b,0x44,0x24,0x10,0x8b,0x89,0x4c,0x0d,0x00,0x00};
    crate_code[3] += (unsigned char)shift;
    if (memcmp(c, collision_code, sizeof collision_code) ||
        memcmp(s, stop_code, sizeof stop_code) ||
        memcmp(layer, layer_code, sizeof layer_code) ||
        memcmp(place, place_code, sizeof place_code) ||
        memcmp(crate, crate_code, sizeof crate_code) ||
        ptr(image, 0x2589af8) != image + 0xe90fc0 - shift ||
        ptr(image, 0x2589b00) != image + 0xe91340 - shift ||
        memcmp(image + 0xba3320, "\xf6\x47\x17\x10\x74\x1c\x8b\x47\x18\x0f\xb6\x4f\x16", 13) ||
        memcmp(image + 0x6d48c0, "\x55\x48\x89\xe5\x48\x8b\xbf\x48\x06\x00\x00", 11) ||
        memcmp(image + 0x6e4eb0, "\x55\x48\x89\xe5\x41\x57\x41\x56\x53\x48\x83\xec\x18", 13) ||
        memcmp(image + 0x7634e0, "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x49\x89\xff", 14) ||
        memcmp(image + 0xe92c10 - shift, "\x55\x48\x89\xe5\x41\x56\x53\x49\x89\xf6\x48\x89\xfb", 13)) return 0;
    unsigned char *wall=image+0xe91d86-shift;
    const unsigned char wall_code[]={0xc5,0xf8,0x2e,0xd1,0x0f,0x86,0x38,0xfb,0xff,0xff,
        0x48,0x8b,0x9d,0x10,0xff,0xff,0xff};
    const unsigned char *threshold_load=image+0xe91d5e - shift;
    int32_t threshold_displacement;
    memcpy(&threshold_displacement,threshold_load+4,4);
    const int64_t threshold_offset=(int64_t)(0xe91d66-shift)+threshold_displacement;
    if (memcmp(wall,wall_code,sizeof wall_code) ||
        memcmp(image+0xe91d56-shift,"\xc5\xf9\x10\x9d\x20\xff\xff\xff",8) ||
        memcmp(threshold_load,"\xc5\xfa\x10\x15",4) || threshold_offset<0 ||
        (uint64_t)threshold_offset>size-4 || memcmp(image+threshold_offset,"\0\0\0\x3f",4)) return 0;
#ifdef _WIN32
    unsigned char *stub = VirtualAlloc(NULL, 512, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    unsigned char *stub = mmap(NULL, 512, PROT_READ | PROT_WRITE | PROT_EXEC,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) stub = NULL;
#endif
    if (!stub || !writable(c, 14) || !writable(s, 14) ||
        !writable(layer, 14) || !writable(crate, 14) || !writable(place, 14) ||
        !writable(image + 0x2589af8, 16) || !writable(wall, sizeof wall_code)) return 0;
    memcpy(stub, collision_code, sizeof collision_code);
    jump(stub + sizeof collision_code, c + sizeof collision_code);
    memcpy(stub + 64, stop_code, sizeof stop_code);
    jump(stub + 64 + sizeof stop_code, s + sizeof stop_code);
    memcpy(stub + 128, layer_code, sizeof layer_code);
    jump(stub + 128 + sizeof layer_code, layer + sizeof layer_code);
    native_layer_filter = (LayerFilter)(stub + 128);
    crate_mode_stub(stub + 192, image + 0x2acde48, crate + sizeof crate_code);
    kart_controller_type = (void **)(image + 0x2ac4580);
    wall_axis_stub(stub + 320, image + 0xe918c8 - shift, wall + sizeof wall_code);
    native_contact = (Contact)stub;
    native_stop = (StopBoost)(stub + 64);
    find_racer = (FindRacer)(image + 0xfd4230 - shift);
    racers = (void **)(image + 0x2ad4b70);
    memcpy(stub + 464, place_code, sizeof place_code);
    jump(stub + 464 + sizeof place_code, place + sizeof place_code);
    native_place_racer = (PlaceRacer)(stub + 464);
    resolve_start_handle = (ResolveHandle)(image + 0x149fca0 - shift);
    native_initialize = (Lifecycle)ptr(image, 0x2589af8);
    native_uninitialize = (Lifecycle)ptr(image, 0x2589b00);
    get_component = (Component)(image + 0x3dbf90);
    physics_type = (void **)(image + 0x2acfc10);
    body_signal = (BodySignal)(image + 0x6d48c0);
    enable_body_flags = (BodyFlags)(image + 0x6e4eb0);
    subscribe_contact = (Subscribe)(image + 0xe92c10 - shift);
    unsubscribe_contact = (Subscribe)(image + 0x7634e0);
    disable_contact = (Subscribe)(image + 0xba3320);
    Lifecycle init = initialize, uninit = uninitialize;
    memcpy(image + 0x2589af8, &init, sizeof init);
    memcpy(image + 0x2589b00, &uninit, sizeof uninit);
    jump(c, (void *)contact);
    jump(s, (void *)stop_boost);
    jump(layer, (void *)layer_filter);
    jump(crate, stub + 192);
    jump(wall, stub + 320);
    memset(wall + 14, 0x90, sizeof wall_code - 14);
    jump(place, (void *)place_racer);
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
#endif
    puts("Native ITEMLESS: Time Trial track layers; synchronized racer collisions and aligned starts");
    puts("Native wall contact: bank continuation is classified relative to stable ground");
    return 1;
}
