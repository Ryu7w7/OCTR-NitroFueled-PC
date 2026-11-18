/* The start screen's racers read before the game's hero data exists.
 *
 * At start, 0x9f1960 reads "numHerosToSpawn" and asks 0xc08280 for each racer's name ("Name%d"),
 * whose default comes from the system data's hero table: [systemData (0x9b9ec0) + 0x10] + 0x98.
 * When that table is not there yet, the game reads NULL + 0x98 and stops (production, v0.8.7/v0.8.8:
 * 96 crashes at start, mostly with custom racer mods; guest 0xc082c0). Its callers (0x9f1960 and
 * 0xc2e400) already treat a NULL name as "no name", so then the name is NULL instead.
 * The same function at the same address in the USA and European 1.21 games.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "runtime.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef void (ABI *HeroName)(void **out, int index);
typedef unsigned char *(ABI *SystemData)(void);

static HeroName native_hero_name;
static SystemData system_data;
static int told;

static void ABI hero_name(void **out, int index) {
    unsigned char *data = system_data();
    void *heroes = NULL;
    if (data) memcpy(&heroes, data + 0x10, sizeof heroes);
    if (!heroes) {
        if (!told++) puts("Hero guard: the racers' names were asked before the hero data was loaded; left empty");
        *out = NULL;
        return;
    }
    native_hero_name(out, index);
}

static void jump(unsigned char *at, const void *target) {
    memcpy(at, "\xff\x25\0\0\0\0", 6);
    memcpy(at + 6, &target, 8);
}

static int writable(void *address, size_t length) {
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(address, length, PAGE_EXECUTE_READWRITE, &old) != 0;
#else
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *start = (void *)((uintptr_t)address & ~(uintptr_t)(page - 1));
    return !mprotect(start, (unsigned char *)address + length - (unsigned char *)start,
                     PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
}

int runtime_hero_guard_start(unsigned char *image, size_t size) {
    /* push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; sub rsp,0xd0 (18 bytes, no
     * RIP-relative operand), then mov r12,[rip+...] */
    static const unsigned char prologue[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,
                                             0x48,0x81,0xec,0xd0,0x00,0x00,0x00,0x4c,0x8b,0x25};
    static const unsigned char getter[] = {0x55,0x48,0x89,0xe5};
    if (size < 0xc08300 || memcmp(image + 0xc08280, prologue, sizeof prologue) ||
        memcmp(image + 0x9b9ec0, getter, sizeof getter)) {
        puts("Hero guard: not this game's code; left out");
        return 1; /* not fatal */
    }
#ifdef _WIN32
    unsigned char *stub = VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    unsigned char *stub = mmap(NULL, 64, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) stub = NULL;
#endif
    if (!stub || !writable(image + 0xc08280, 14)) return 0;
    memcpy(stub, image + 0xc08280, 18);
    jump(stub + 18, image + 0xc08280 + 18);
    native_hero_name = (HeroName)stub;
    system_data = (SystemData)(image + 0x9b9ec0);
    jump(image + 0xc08280, (void *)hero_name);
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
#endif
    return 1;
}
