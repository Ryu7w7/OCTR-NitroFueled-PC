/* Native wall classification, Time Trial track rules and eligible Itemless boost retention. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef void (ABI *Contact)(void *, void *, void *);
typedef void (ABI *StopBoost)(void *);
typedef void *(ABI *FindRacer)(void *, void *);
typedef void *(ABI *FindComponent)(void *, void *, int);
static Contact native_contact;
static StopBoost native_stop;
static FindRacer find_racer;
static FindComponent find_component;
static void **kart_controller_type;
static void **racers;
static _Thread_local void *contact_owner;
typedef unsigned char (ABI *LayerFilter)(void *, const char *, int);
static LayerFilter native_layer_filter;

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
    void *kart=entity ? find_component(entity,*kart_controller_type,0) : NULL;
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

static int is_racer(void *entity) {
    void *racer = entity && *racers ? find_racer(*racers, entity) : NULL;
    return racer != NULL;
}

static void ABI contact(void *self, void *context, void *collision) {
    void *previous = contact_owner;
    contact_owner = NULL;
    if (runtime_speed_race_active() && context && collision) {
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
                    if (owner != other && is_racer(owner) && is_racer(other)) contact_owner = owner;
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
    if (size < 0x2ad4b78) return 0;
    size_t shift = memcmp(image + 0xe916b0, collision_code, sizeof collision_code) ? 0x10 : 0;
    unsigned char *c = image + 0xe916b0 - shift, *s = image + 0xe92e90 - shift;
    unsigned char *layer = image + 0xfe44c0 - shift;
    unsigned char *crate = image + 0xeebaa6 - shift;
    unsigned char crate_code[] = {0x48,0x8b,0x0d,0x9b,0x23,0xbe,0x01,
        0x49,0x8b,0x44,0x24,0x10,0x8b,0x89,0x4c,0x0d,0x00,0x00};
    crate_code[3] += (unsigned char)shift;
    if (memcmp(c, collision_code, sizeof collision_code) ||
        memcmp(s, stop_code, sizeof stop_code) ||
        memcmp(layer, layer_code, sizeof layer_code) ||
        memcmp(crate, crate_code, sizeof crate_code)) return 0;
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
        !writable(layer, 14) || !writable(crate, 14) || !writable(wall,sizeof wall_code)) return 0;
    memcpy(stub, collision_code, sizeof collision_code);
    jump(stub + sizeof collision_code, c + sizeof collision_code);
    memcpy(stub + 64, stop_code, sizeof stop_code);
    jump(stub + 64 + sizeof stop_code, s + sizeof stop_code);
    memcpy(stub + 128, layer_code, sizeof layer_code);
    jump(stub + 128 + sizeof layer_code, layer + sizeof layer_code);
    native_layer_filter = (LayerFilter)(stub + 128);
    crate_mode_stub(stub + 192, image + 0x2acde48, crate + sizeof crate_code);
    find_component=(FindComponent)(image+0x3dbf90);
    kart_controller_type=(void **)(image+0x2ac4580);
    wall_axis_stub(stub+320,image+0xe918c8-shift,wall+sizeof wall_code);
    native_contact = (Contact)stub;
    native_stop = (StopBoost)(stub + 64);
    find_racer = (FindRacer)(image + 0xfd4230 - shift);
    racers = (void **)(image + 0x2ad4b70);
    jump(c, (void *)contact);
    jump(s, (void *)stop_boost);
    jump(layer, (void *)layer_filter);
    jump(crate, stub + 192);
    jump(wall,stub+320);
    memset(wall+14,0x90,sizeof wall_code-14);
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
#endif
    puts("Native ITEMLESS: Time Trial track layers and crate spawning; preserve boost on racer contact");
    puts("Native wall contact: bank continuation is classified relative to stable ground");
    return 1;
}
