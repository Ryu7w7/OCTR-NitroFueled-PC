/* Credit every beaten Time Trial tier through the native finish/save/reward path. */
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

typedef unsigned char *(ABI *FindChallenges)(const char *);
typedef unsigned char *(ABI *LoadGhost)(void *, const char *);
typedef void (ABI *ChallengeEvent)(void *, const char *, void *, int, int, int);
static FindChallenges find_challenges;
static LoadGhost load_ghost;
static ChallengeEvent challenge_event;
static unsigned char **players;
static void **events;

static void *ptr(const void *object, size_t offset) {
    void *value;
    memcpy(&value, (const unsigned char *)object + offset, sizeof value);
    return value;
}

static int integer(const void *object, size_t offset) {
    int value;
    memcpy(&value, (const unsigned char *)object + offset, sizeof value);
    return value;
}

static float seconds(const void *object, size_t offset) {
    float value;
    memcpy(&value, (const unsigned char *)object + offset, sizeof value);
    return value;
}

static void ABI finish_challenges(unsigned char *manager, unsigned char *save,
                                  const char *track, float time) {
    static const char *names[] = {
        "beat_ntropy_ghost", "beat_oxide_ghost", "beat_velo_ghost", "beat_dev_ghost"
    };
    if (!manager || !save || !track || !isfinite(time) || time <= 0) return;
    int tier = integer(save, 0x0c);
    if (tier < -1 || tier >= 4) return;
    unsigned char *header = find_challenges(track);
    if (!header) return;
    unsigned char *list = ptr(header, 0x20);
    if (!list) return;
    const int count = integer(list, 0x0c);
    if (count < 1 || count > 4) return;
    unsigned char **items = ptr(list, 0x20);
    if (!items) return;
    while (tier < count) {
        float target;
        if (tier == -1) {
            target = seconds(header, 0x18); /* Initial N. Tropy availability time. */
        } else {
            if (!items[tier]) break;
            const char *source = ptr(items[tier], 0x28);
            if (!source) break;
            char path[1024];
            size_t i;
            for (i = 0; i < sizeof path - 1 && source[i]; ++i)
                path[i] = source[i] >= 'A' && source[i] <= 'Z' ? source[i] + 32 : source[i];
            if (source[i]) break;
            path[i] = 0;
            /* The native loader owns this data in manager->_ghosts; no racing ghost is spawned. */
            unsigned char *ghost = load_ghost(manager, path);
            if (!ghost) break;
            target = seconds(ghost, 0x28);
        }
        if (!isfinite(target) || !(time < target)) break;
        if (tier >= 0) {
            void *player = NULL;
            unsigned char *roster = *players ? ptr(*players, 0x10) : NULL;
            if (roster) {
                unsigned char **entries = ptr(roster, 0x20);
                for (int i = 0; entries && i < integer(roster, 0x0c); ++i) {
                    if (entries[i] && (unsigned)integer(entries[i], 0x20) <= 7) {
                        player = entries[i];
                        break;
                    }
                }
            }
            challenge_event(*events, names[tier], player, 0, 0, 1);
        }
        ++tier;
        memcpy(save + 0x0c, &tier, sizeof tier);
        manager[0x30] = 1;
    }
}

static void jump_to(unsigned char *code, const void *target) {
    memcpy(code, "\xff\x25\0\0\0\0", 6);
    memcpy(code + 6, &target, sizeof target);
}

static int write_code(void *target, const void *source, size_t size) {
#ifdef _WIN32
    DWORD old, ignored;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(target, source, size);
    FlushInstructionCache(GetCurrentProcess(), target, size);
    return VirtualProtect(target, size, old, &ignored) != 0;
#else
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *start = (void *)((uintptr_t)target & ~(uintptr_t)(page - 1));
    size_t span = (unsigned char *)target + size - (unsigned char *)start;
    if (mprotect(start, span, PROT_READ | PROT_WRITE | PROT_EXEC)) return 0;
    memcpy(target, source, size);
    __builtin___clear_cache(target, (char *)target + size);
    return mprotect(start, span, PROT_READ | PROT_EXEC) == 0;
#endif
}

/* These signatures cover the finish gate, frame layout, progress write and ghost data layout.
 * USA/EU 1.21 differ by 16 bytes here; an unknown executable keeps native behavior. */
static int supported(const unsigned char *image, size_t size, size_t delta) {
    if (size < 0x2ad1168) return 0;
#define MATCH(at, bytes) (!memcmp(image + (at) - delta, bytes, sizeof(bytes) - 1))
    return MATCH(0xfcc430, "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x58") &&
        MATCH(0xfcc5fc, "\x0a\x5d\xa8\x80\xfb\x01\x0f\x85\x56\xff\xff\xff\x84\xc0\x0f\x85\x4e\xff\xff\xff") &&
        MATCH(0xfcc6d2, "\xc5\xfa\x10\x45\x98\x48\x8b\x5d\x80\xc5\xf8\x2e\x45\x90\x0f\x86\xc1\x01\x00\x00") &&
        MATCH(0xfcc890, "\x48\x8b\x55\xc0\x8b\x4a\x0c\x41\x3b\x4d\x34\x75\x0a\xff\xc1\x89\x4a\x0c\x41\xc6\x45\x30\x01") &&
        MATCH(0xf9b970, "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x49\x89\xfe\xe8\x7d\x00\x00\x00") &&
        MATCH(0xf9b913, "\x48\x8b\x4b\x20\x48\x63\x41\x0c\x48\x85\xc0\x0f\x84\x75\xff\xff\xff\x48\x8b\x49\x20\x48\xc1\xe0\x03\x31\xd2\x4c\x39\x24\xd1\x75\x12\xc4\xc1\x7a\x10\x47\x28") &&
        MATCH(0xf9b88c, "\xe8\xdf\x01\x00\x00") &&
        MATCH(0xf9ba70, "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x28") &&
        MATCH(0xf9b83e, "\x49\x8b\x74\x24\x28") &&
        MATCH(0xfcc888, "\xe8") &&
        MATCH(0xfcc704, "\x48\x8b\x05") && MATCH(0xfcc70b, "\x48\x8b\x3d");
#undef MATCH
}

static void *relative(unsigned char *instruction, size_t length) {
    int32_t displacement;
    memcpy(&displacement, instruction + length - 4, 4);
    return instruction + length + displacement;
}

int runtime_time_trial_start(unsigned char *image, size_t size) {
    size_t delta = 0;
    if (!supported(image, size, delta)) {
        delta = 16;
        if (!supported(image, size, delta)) {
            puts("Time Trial multi-tier credit: unsupported executable; keeping native behavior");
            return 1;
        }
    }
    if (relative(image + 0xfcc888 - delta, 5) != image + 0xaa2310 ||
        relative(image + 0xfcc704 - delta, 7) != image + 0x2ad1160 ||
        relative(image + 0xfcc70b - delta, 7) != image + 0x2aa48c0) {
        puts("Time Trial multi-tier credit: unrecognized native services; keeping native behavior");
        return 1;
    }
#ifdef _WIN32
    unsigned char *bridge = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    unsigned char *bridge = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bridge == MAP_FAILED) return 0;
#endif
    if (!bridge) return 0;
    find_challenges = (FindChallenges)(image + 0xf9b970 - delta);
    load_ghost = (LoadGhost)(image + 0xf9ba70 - delta);
    challenge_event = (ChallengeEvent)relative(image + 0xfcc888 - delta, 5);
    players = relative(image + 0xfcc704 - delta, 7);
    events = relative(image + 0xfcc70b - delta, 7);
    /* The original finish frame is already SysV-aligned. Keep its callee-saved registers and
     * restore the track name and lookup-result byte consumed by the native save continuation. */
    static const unsigned char args[] = {
        0x4c,0x89,0xef, 0x48,0x8b,0x75,0xc0, 0x48,0x8b,0x55,0x80,
        0xf3,0x0f,0x10,0x45,0x90, 0x48,0xb8
    };
    memcpy(bridge, args, sizeof args);
    void *handler = (void *)finish_challenges;
    memcpy(bridge + sizeof args, &handler, 8);
    memcpy(bridge + sizeof args + 8, "\xff\xd0\x48\x8b\x5d\x80\x8a\x45\xb7", 9);
    jump_to(bridge + sizeof args + 17, image + 0xfcc8a7 - delta);
    if (!write_code(bridge, bridge, sizeof args + 31)) return 0;
    unsigned char jump[14];
    jump_to(jump, bridge);
    if (!write_code(image + 0xfcc6d2 - delta, jump, sizeof jump)) return 0;
    /* Evaluate earned tiers even when this finish is slower than the player's personal best.
     * The native cheat gate and the separate personal-best replacement gate remain in place. */
    if (!write_code(image + 0xfcc602 - delta, "\x90\x90\x90\x90\x90\x90", 6)) return 0;
    puts("Time Trial multi-tier credit: all beaten challenges count, with or without ghosts");
    return 1;
}
