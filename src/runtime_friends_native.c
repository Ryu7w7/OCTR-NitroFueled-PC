/* Invalidate the native friend cache on its own update thread after RyuNET changes. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_friends.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef void (ABI *FriendUpdate)(unsigned char *);
static FriendUpdate native_update;
static unsigned char **users;
static unsigned seen_serial;

static void ABI friends_update(unsigned char *self) {
    const unsigned serial = runtime_friends_names_serial();
    void *pending;
    memcpy(&pending, self + 0x88, sizeof pending);
    /* Let an older GetFriendsSequence finish before scheduling the new snapshot. */
    if (serial != seen_serial && !pending && *users) {
        int player;
        memcpy(&player, *users + 0x78, sizeof player);
        if ((unsigned)player < 8) {
            self[0x50 + player] = 1;
            memset(self + 0x58 + player * 4, 0, 4);
            self[0x78 + player] = 1;
            seen_serial = serial;
        }
    }
    native_update(self);
}

int runtime_friends_native_start(unsigned char *image, size_t size) {
    /* USA/EU 1.21: CPS4FriendManager inherits CFriendManager::update (+0xc0).
     * Its dirty flags are the same ones the native user-change handler sets. */
    const char *live = getenv("BB_FRIENDS_LIVE");
    if (live && live[0] == '0') { /* to rule it out in a test */
        puts("Friends live refresh: off (BB_FRIENDS_LIVE=0)");
        return 1;
    }
    if (size < 0x2ac2e98 ||
        memcmp(image + 0xab55b0, "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x48\x89\xfb\x80\x7b\x50\x00", 18) ||
        memcmp(image + 0xab5a56, "\xc6\x44\x07\x50\x01\xc7\x44\x87\x58\x00\x00\x00\x00\xc6\x44\x07\x78\x01", 18) ||
        memcmp(image + 0xab59a3, "\x48\x8b\x0d\xe6\xd4\x00\x02", 7)) {
        puts("Friends live refresh: unsupported executable; keeping native behavior");
        return 1;
    }
    FriendUpdate *slot = (FriendUpdate *)(image + 0x255e468);
    if (*slot != (FriendUpdate)(image + 0xab55b0)) {
        puts("Friends live refresh: unrecognized vtable; keeping native behavior");
        return 1;
    }
    native_update = *slot;
    users = (unsigned char **)(image + 0x2ac2e90);
#ifdef _WIN32
    DWORD old, ignored;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return 0;
    *slot = friends_update;
    if (!VirtualProtect(slot, sizeof *slot, old, &ignored)) return 0;
#else
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *start = (void *)((uintptr_t)slot & ~(uintptr_t)(page - 1));
    if (mprotect(start, page, PROT_READ | PROT_WRITE)) return 0;
    *slot = friends_update;
    if (mprotect(start, page, PROT_READ)) return 0;
#endif
    puts("Friends live refresh: RyuNET changes invalidate the native friend cache");
    return 1;
}
