/* A save whose equipped item is not in this installation (a DLC or a mod missing) stopped the game on entering a
 * room: the read of the item's name gets a check.
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

static int runtime_profile_lookup_guard_start(unsigned char *image, size_t size);

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
    return !mprotect(start, (unsigned char *)address + length - (unsigned char *)start, PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
}

/* 0xfdee00 (profile stats into the online player info): the item the profile has equipped (+0xdc0 -> +0x60 -> handle
 * +0x10) is read for its name (+0x58) without a check. With a save whose item is not in this installation (a DLC or
 * mod missing) the handle resolves to nothing and the game stops on entering a room (read at 0x58). The read gets the
 * check: no item, no name.
 *   0xfdeec0 (EU 0xfdeeb0): mov rbx,[r12+0x58]; test rbx,rbx; jz +4; lock inc dword [rbx-8]; mov rsi,[r13+0x50]  (18 bytes) */
int runtime_profile_item_guard_start(unsigned char *image, size_t size) {
    runtime_profile_lookup_guard_start(image, size);   /* its own check, below */
    static const unsigned char code[] = {0x49,0x8b,0x5c,0x24,0x58,0x48,0x85,0xdb,0x74,0x04,0xf0,0xff,0x43,0xf8,
                                         0x49,0x8b,0x75,0x50};
    /* USA (CUSA13795) 1.21 at 0xfdeec0; the EU build (CUSA14876) 1.21 has the same function 16 bytes earlier. */
    static const size_t places[] = {0xfdeec0, 0xfdeeb0};
    unsigned char *at = NULL;
    for (size_t i = 0; i < sizeof places / sizeof *places && !at; ++i)
        if (size >= places[i] + sizeof code && !memcmp(image + places[i], code, sizeof code)) at = image + places[i];
    if (!at) {
        puts("Profile item guard: unexpected code at 0xfdeec0 (USA) and 0xfdeeb0 (EU), not installed");
        return 1;
    }
#ifdef _WIN32
    unsigned char *stub = VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    unsigned char *stub = mmap(NULL, 64, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) stub = NULL;
#endif
    if (!stub || !writable(at, sizeof code)) return 1;
    static const unsigned char guarded[] = {
        0x31,0xdb,                    /* xor ebx,ebx */
        0x4d,0x85,0xe4,               /* test r12,r12 */
        0x74,0x05,                    /* jz +5 */
        0x49,0x8b,0x5c,0x24,0x58,     /* mov rbx,[r12+0x58] */
        0x48,0x85,0xdb,               /* test rbx,rbx */
        0x74,0x04,                    /* jz +4 */
        0xf0,0xff,0x43,0xf8,          /* lock inc dword [rbx-8] */
        0x49,0x8b,0x75,0x50};         /* mov rsi,[r13+0x50] */
    memcpy(stub, guarded, sizeof guarded);
    jump(stub + sizeof guarded, at + sizeof code);
    jump(at, stub);
    memset(at + 14, 0x90, sizeof code - 14);
    printf("Profile item guard: installed at image+%#lx (a missing equipped item no longer stops the game in a room)\n",
           (unsigned long)(at - image));
    return 1;
}

/* 0xbedae0 (the same address in the USA and EU 1.21 builds), run after a save loads (SL_LOAD_GAME, 14 callers):
 * it looks an entry up by key (0xbdc490) and calls the virtual of the object the entry holds (+0x20) with no check.
 * With some players' saves the entry is there and its object is not, and the game stopped on the way to the main
 * menu every time (null read at guest 0xbedb2c; 18 v0.9.0 reports on 20/11, two players' logs). The read gets the
 * check: no object takes the function's own "not that one" path (0xbedcbd: the entry released, 0 returned).
 *   0xbedb20: mov rbx,[rax+0x20]; mov rax,[rdi]; call [rax]; mov r12,rax; mov rax,[rbx]  (15 bytes) */
static int runtime_profile_lookup_guard_start(unsigned char *image, size_t size) {
    static const unsigned char code[] = {0x48,0x8b,0x58,0x20,0x48,0x8b,0x07,0xff,0x10,0x49,0x89,0xc4,0x48,0x8b,0x03};
    static const unsigned char refused[] = {0x31,0xdb,0x48,0x8b,0x7d,0xc8};   /* 0xbedcbd: xor ebx,ebx; mov rdi,[rbp-0x38] */
    const size_t place = 0xbedb20, no = 0xbedcbd;
    if (size < no + sizeof refused || memcmp(image + place, code, sizeof code) || memcmp(image + no, refused, sizeof refused)) {
        puts("Profile lookup guard: unexpected code at 0xbedb20, not installed");
        return 1;
    }
#ifdef _WIN32
    unsigned char *stub = VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    unsigned char *stub = mmap(NULL, 64, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) stub = NULL;
#endif
    if (!stub || !writable(image + place, sizeof code)) return 1;
    static const unsigned char guarded[] = {
        0x48,0x8b,0x58,0x20,          /* mov rbx,[rax+0x20] */
        0x48,0x8b,0x07,               /* mov rax,[rdi] */
        0xff,0x10,                    /* call [rax] */
        0x49,0x89,0xc4,               /* mov r12,rax */
        0x48,0x85,0xdb,               /* test rbx,rbx */
        0x74,0x11,                    /* jz +17: to the refusal below */
        0x48,0x8b,0x03};              /* mov rax,[rbx] */
    memcpy(stub, guarded, sizeof guarded);
    jump(stub + sizeof guarded, image + place + sizeof code);   /* back at 0xbedb2f */
    jump(stub + sizeof guarded + 14, image + no);               /* no object: 0xbedcbd */
    jump(image + place, stub);
    image[place + 14] = 0x90;
    puts("Profile lookup guard: installed at image+0xbedb20 (a save entry with no object no longer stops the game at start)");
    return 1;
}

