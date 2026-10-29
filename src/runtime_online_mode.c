/* Native Normal / Speed / Battle selector; Speed uses the game's race event. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

extern void runtime_speed_mode_set(int enabled);
typedef void (ABI *EventHandler)(unsigned char *, void *, void *);
typedef void (ABI *EmitChoice)(void *, int);
typedef void (ABI *SetSeason)(void *, int);
typedef void *(ABI *GetType)(void *);
typedef void (ABI *PrivateState)(unsigned char *, unsigned char *);
static EventHandler original_event, base_event;
static EmitChoice original_choice;
static SetSeason original_season;
static PrivateState private_state;
static void **initialize_type;
static unsigned char **network;
static _Thread_local int customized_selector;
static int private_speed_selected;
/* Every pick in the selector (set by runtime_room_mode.c; NULL in the unit test, which links this
 * file alone): in a private match the host's choice goes to RyuNET for the players who join, and
 * a player picking a mode stops following another host's room. */
void (*runtime_online_mode_chosen)(int speed, int private_room);

/* A joining player takes the room's mode from its host (runtime_room_mode.c): Speed or not,
 * whatever this player picked in the selector. Their own choice stays for their own room. */
void runtime_online_mode_adopt(int speed) {
    runtime_speed_mode_set(speed != 0);
    printf("Native Online mode: %s (the host's private room)\n", speed ? "Speed Mode" : "Normal Mode");
}

int runtime_online_mode_private_speed(void) { return private_speed_selected; }

static void ABI set_season(void *manager, int mode) {
    /* Evaluated on each level load; retain the incoming theme for normal races. */
    original_season(manager, runtime_speed_race_active() ? 0 : mode);
}

static int is_private(void) {
    unsigned char yes = 0, no = 0;
    private_state(&yes, &no);
    return yes != 0;
}

/* The game's own private-match flag (0 before the selector hook is installed). */
int runtime_online_mode_in_private(void) { return private_state ? is_private() : 0; }

static void ABI select_mode(void *sender, int index) {
    if (!customized_selector) {
        original_choice(sender, index);
        return;
    }
    const int private = is_private();
    /* PUBLIC_SPEED_STEP_2: integration sketch, NOT implemented protocol APIs.
     * After STEP_1 and authenticated queue separation are implemented:
     *
     *   if (!private && index == 1 && !runtime_public_speed_available()) return;
     *   runtime_public_speed_request(!private && index == 1);
     *   const int speed = index == 1 && (private || runtime_public_speed_available());
     *
     * Submit that request when Go To Lobby starts the actual search, not here.
     * Keep original_choice's Race mapping: this is a local game type, NOT a
     * separate server queue. Reject a failed Speed search instead of falling
     * back to Normal. See docs/SPEED_MODE_MATCHMAKING_PROPOSAL.md.
     */
    const int speed = index == 1 && private;
    if (private) private_speed_selected = speed;
    if (runtime_online_mode_chosen) runtime_online_mode_chosen(speed, private);
    runtime_speed_mode_set(speed);
    printf("Native Online mode: %s\n", speed ? "Speed Mode" : index == 2 ? "Battle Mode" : "Normal Mode");
    original_choice(sender, index == 2 ? 1 : 0);
}

static void ABI selector_event(unsigned char *self, void *event, void *owner) {
    unsigned char *options;
    memcpy(&options, self + 0x1a8, sizeof options);
    int count;
    memcpy(&count, options + 0x0c, sizeof count);
    GetType *vtable = *(GetType **)event;
    if (count == 3 && vtable[0](event) == *initialize_type) {
        unsigned char *available, *items;
        memcpy(&available, self + 0x1b0, sizeof available);
        memcpy(&items, available + 0x20, sizeof items);
        /* PUBLIC_SPEED_STEP_1: once the negotiated server capability AND client
         * queue integration exist, this becomes:
         *   items[1] = is_private() || runtime_public_speed_available();
         * That API is proposed, not implemented. Keep separate private/public
         * remembered choices; private_speed_selected must not initialize a public
         * Speed search merely because its option has become available.
         */
        items[1] = (unsigned char)is_private();
        int type;
        memcpy(&type, *network + 0x13c, sizeof type);
        const int index = type == 2 ? 2 : (items[1] && private_speed_selected ? 1 : 0);
        memcpy(self + 0x190, &index, sizeof index);
        runtime_speed_mode_set(index == 1);
        /* The asset already supplies all options. Keep the base selector initialization. */
        base_event(self, event, owner);
    } else {
        const int previous = customized_selector;
        customized_selector = count == 3;
        original_event(self, event, owner);
        customized_selector = previous;
    }
}

static int write_code(void *target, const void *data, size_t length) {
#ifdef _WIN32
    DWORD old, ignored;
    if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(target, data, length);
    FlushInstructionCache(GetCurrentProcess(), target, length);
    return VirtualProtect(target, length, old, &ignored) != 0;
#else
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    void *first = (void *)((uintptr_t)target & ~(uintptr_t)(page - 1));
    const size_t span = (size_t)((unsigned char *)target + length - (unsigned char *)first);
    if (mprotect(first, span, PROT_READ | PROT_WRITE | PROT_EXEC)) return 0;
    memcpy(target, data, length);
    __builtin___clear_cache(target, (char *)target + length);
    return mprotect(first, span, PROT_READ | PROT_EXEC) == 0;
#endif
}

static void jump_to(unsigned char *code, const void *target) {
    memcpy(code, "\xff\x25\0\0\0\0", 6);
    memcpy(code + 6, &target, sizeof target);
}

int runtime_online_mode_start(unsigned char *image, size_t size) {
    const char *setting = getenv("BB_ONLINE_MODE_HOOK");
    if (!setting || !*setting) return 1;
    /* These sites belong to the supported USA 1.21 executable; fail before guest entry. */
    if (strcmp(setting, "1277e00") || size < 0x2af1fb0) return 0;
    static const unsigned char prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x53,0x50,0x48,0x8d,0x05,0x97,0xa1,0x87,0x01};
    static const unsigned char season_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,
        0x41,0x54,0x53,0x48,0x83,0xec,0x20};
    if (memcmp(image + 0x1277e00, prefix, sizeof prefix) ||
        memcmp(image + 0xfb7710, season_prefix, sizeof season_prefix) ||
        memcmp(image + 0x125d570, "\x55\x48\x89\xe5\x41\x57\x41\x56", 8) ||
        memcmp(image + 0xf50350, "\x55\x48\x89\xe5\x41\x57\x41\x56", 8)) return 0;
    uintptr_t handler;
    memcpy(&handler, image + 0x2773b40, sizeof handler);
    if (handler != (uintptr_t)(image + 0x1277c70)) return 0;
#ifdef _WIN32
    /* Executable from the start: write_code() puts back the protection it found, and a
     * PAGE_READWRITE trampoline crashed the first call to the original season setter (code 3). */
    unsigned char *trampoline = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    unsigned char *trampoline = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) return 0;
#endif
    if (!trampoline) return 0;
    /* Relocate the stolen RIP-relative LEA as MOVABS; preserve the original prologue. */
    memcpy(trampoline, prefix, 10);
    trampoline[10] = 0x48; trampoline[11] = 0xb8;
    const uintptr_t global = (uintptr_t)(image + 0x2af1fa8);
    memcpy(trampoline + 12, &global, sizeof global);
    jump_to(trampoline + 20, image + 0x1277e11);
    memcpy(trampoline + 64, season_prefix, sizeof season_prefix);
    jump_to(trampoline + 64 + sizeof season_prefix, image + 0xfb771f);
    if (!write_code(trampoline, trampoline, 64 + sizeof season_prefix + 14)) return 0;
    original_choice = (EmitChoice)trampoline;
    original_season = (SetSeason)(trampoline + 64);
    original_event = (EventHandler)(image + 0x1277c70);
    base_event = (EventHandler)(image + 0x125d570);
    private_state = (PrivateState)(image + 0xf50350);
    initialize_type = (void **)(image + 0x2a21ee8);
    network = (unsigned char **)(image + 0x2acb3d8);
    unsigned char jump[14];
    jump_to(jump, (void *)select_mode);
    const EventHandler replacement = selector_event;
    if (!write_code(image + 0x1277e00, jump, sizeof jump) ||
        !write_code(image + 0x2773b40, &replacement, sizeof replacement)) return 0;
    jump_to(jump, (void *)set_season);
    if (!write_code(image + 0xfb7710, jump, sizeof jump)) return 0;
    puts("Native Online mode: Normal Mode / Speed Mode / Battle Mode selector installed");
    return 1;
}
