/* N.Sane Mode (private rooms): a second stored item.
 *
 * With an item in hand (or the roulette spinning), a crate the game would ignore stores a second item; on the
 * Grand Prix tracks, which have no extra crates (scripts/nsane_crates.py), one crate gives both. The second uses
 * the game's own position odds, spins in the small slot and never repeats the first. Using the first hands it over.
 * A small second slot next to the item HUD shows it (scripts/speed_mode.py adds the hidden slot to
 * ui/GuiSharedAssets/Octane_PowerUp.igz in every view; this file shows/hides its icons).
 * Everything here is gated by runtime_nsane_race_active(); outside an N.Sane race the game is untouched.
 *
 * CVehiclePowerUpControllerComponent: +0x18 kart entity, +0x30 _storedPowerUp (handle),
 * +0x40 _isShuffling. Native functions (USA 1.21):
 *   0xe36230 crate collected (ctrl, ?, message): message +0x38 type (-1 = random)
 *   0xeb10d0 item used/finished (ctrl, item): clears +0x30 when item is the stored one
 *   0xe366b0 give item (float 0, ctrl, type, 0, 0, 0)
 *   0x9b9ec0 +0xf8 weights; 0xe38030 table(weights, position, mode, flag); 0xe38190 / 0xe38280 type;
 *   0xc02470 local player flag; 0xfddb30 statistics; 0xfd4230 racer of kart; 0x3dbf90 component.
 * Item types: 0 Bomb .. 12 Invisibility (systemData _powerUpNamesTable).
 * HUD: CRacerHudComponent (meta [0x2ad4940]) +0x38 project; project +0x10 root, +0xa0 instance map;
 * placeable +0x10 instance map, +0x18 source, +0x74 pivot z (marker 1000+type, 1013 = slot background),
 * +0xc flags (0x400 hidden, 0x1000 redraw), +0xe0 alpha. Instance map: +0x10/+0x20 byte sizes,
 * +0x18/+0x28 entries. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef void (ABI *CrateCollected)(void *, void *, unsigned char *);
typedef void (ABI *ItemFinished)(unsigned char *, void *);
typedef void (ABI *GiveItem)(float, void *, int, int, int, int);
typedef void *(ABI *GetComponent)(void *, void *, int);
typedef void (ABI *ResolveHandle)(void *);
typedef unsigned char *(ABI *NoArgs)(void);
typedef unsigned char *(ABI *RacerOf)(void *, void *);
typedef unsigned char (ABI *IsLocal)(void *);
typedef void *(ABI *OddsTable)(void *, int, int, int);
typedef void (ABI *PrepareOdds)(void *, void *);
typedef int (ABI *RollType)(void *, void *);
typedef void (ABI *Statistic)(void *, int, int);
typedef int (ABI *TypeOf)(void *);

static CrateCollected native_crate;
static ItemFinished native_finished;
static GiveItem give_item;
static GetComponent get_component;
static ResolveHandle resolve_handle;
static NoArgs odds_owner;
static RacerOf racer_of;
static IsLocal is_local;
static OddsTable odds_table;
static PrepareOdds prepare_odds;
static RollType roll_type;
static Statistic statistic;
static void **block_meta, **hud_meta, **racers, **game_manager;
static int available;

#define SLOTS 8
/* spin: the slot shows a roulette (icons cycling) until this time (ms); shown: the icon on screen */
static struct { void *controller; int type; unsigned long long spin; int shown; } second[SLOTS];
static int marked;
static int grand_prix_track;   /* no extra crates here: one crate gives both items */

#define SPIN_MS 1500ull   /* the second item's roulette */
#define STEP_MS 70ull     /* one icon per step */

static unsigned long long now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
#endif
}

static void *ptr(const void *p, size_t off) {
    void *v;
    memcpy(&v, (const unsigned char *)p + off, sizeof v);
    return v;
}

static int u32(const void *p, size_t off) {
    int v;
    memcpy(&v, (const unsigned char *)p + off, sizeof v);
    return v;
}

/* Guest heap objects of the HUD (instance maps hold empty slots and other values too). */
static int heap_object(const void *p) {
    const uintptr_t v = (uintptr_t)p;
    return v >= 0x1000000000ull && v < 0x2000000000ull && !(v & 7);
}

static unsigned char *handle_object(void **slot) {
    for (int depth = 0; depth < 16 && (uintptr_t)*slot >= 0x10000; ++depth) {
        unsigned flags;
        memcpy(&flags, *slot, 4);
        if (flags & 0x10000000) resolve_handle(slot);
        if (!*slot) return NULL;
        memcpy(&flags, *slot, 4);
        unsigned char *object = ptr(*slot, 0x28);
        if (!(flags & 0x20000000) || !object) return object;
        slot = (void **)(object + 0x10);
    }
    return NULL;
}

/* ---- HUD: the hidden second slot ---------------------------------------------------------- */
static void *placeable_vtable, *map_vtable;
static void *seen[128];
static int seen_count, visits;

static void scan_map(unsigned char *map, int want, int depth);

static void visit(unsigned char *p, int want, int depth) {
    if (!heap_object(p) || ptr(p, 0) != placeable_vtable || ++visits > 6000) return;
    unsigned char *source = ptr(p, 0x18);
    if (!heap_object(source)) source = p;
    float z;
    memcpy(&z, source + 0x74, 4);
    const int index = (int)(z - 1000.0f);
    if (index >= 0 && index <= 13 && z == 1000.0f + (float)index) {
        const int show = index == 13 ? want >= 0 : index == want;
        unsigned flags;
        memcpy(&flags, p + 0x0c, 4);
        flags = show ? (flags & ~0x400u) | 0x1000u : flags | 0x1400u;
        memcpy(p + 0x0c, &flags, 4);
        const float alpha = show ? 1.0f : 0.0f;
        memcpy(p + 0xe0, &alpha, 4);
        ++marked;
    }
    scan_map(ptr(p, 0x10), want, depth + 1);
}

static void scan_map(unsigned char *map, int want, int depth) {
    if (depth > 12 || !heap_object(map) || ptr(map, 0) != map_vtable) return;
    for (int i = 0; i < seen_count; ++i) if (seen[i] == map) return;
    if (seen_count < (int)(sizeof seen / sizeof *seen)) seen[seen_count++] = map;
    for (int list = 0; list < 2; ++list) {
        const int count = u32(map, 0x10 + 0x10 * list) / 8;
        void **items = ptr(map, 0x18 + 0x10 * list);
        if (count <= 0 || count > 4096 || !heap_object(items)) continue;
        for (int i = 0; i < count; ++i) visit(items[i], want, depth);
    }
}

/* want: the stored item's type, or -1 to hide the slot. Only the local players' HUD. */
static void show_second(unsigned char *controller, int want) {
    void *kart = ptr(controller, 0x18);
    if (!kart || !is_local(kart)) return;
    unsigned char *hud = get_component(kart, *hud_meta, 0);
    unsigned char *project = hud ? handle_object((void **)(hud + 0x38)) : NULL;
    unsigned char *root = project ? ptr(project, 0x10) : NULL;
    if (!heap_object(root)) return;
    placeable_vtable = ptr(root, 0);
    unsigned char *map = ptr(root, 0x10);
    if (!heap_object(map)) return;
    map_vtable = ptr(map, 0);
    seen_count = visits = marked = 0;
    scan_map(map, want, 0);
    scan_map(ptr(project, 0xa0), want, 0);
}

/* ---- items -------------------------------------------------------------------------------- */
static int has_item(unsigned char *controller) {
    if (controller[0x40]) return 1; /* the roulette is spinning */
    void **stored = (void **)(controller + 0x30);
    if (!*stored) return 0;
    unsigned flags;
    memcpy(&flags, *stored, 4);
    if (flags & 0x20000000 && !(flags & 0x10000000)) return 1; /* chained: the game keeps it as full */
    return handle_object(stored) != NULL;
}

static int find_slot(void *controller) {
    for (int i = 0; i < SLOTS; ++i) if (second[i].controller == controller) return i;
    return -1;
}

/* The same choice as the end of the roulette (0xe37e00), without creating the item. */
static int roll(void *kart) {
    unsigned char *owner = odds_owner();
    void *weights = owner ? ptr(owner, 0xf8) : NULL;
    unsigned char *racer = weights && *racers ? racer_of(*racers, kart) : NULL;
    if (!racer) return -1;
    const int flag = is_local(kart) ? 1 : 0;
    const int position = u32(racer, 0x58);
    void *table = odds_table(weights, position, u32(*game_manager, 0xd4c), flag);
    if (!table) table = odds_table(weights, position, 1, flag);
    if (!table) return -1;
    prepare_odds(table, racer);
    const int type = roll_type(table, racer);
    statistic(*game_manager, type, 1);
    return type;
}

/* The type of the item in hand, or -1 (the item's vtable +0xf8; 13 = a multi item, its real type at +0x54). */
static int hand_type(unsigned char *controller) {
    if (!ptr(controller, 0x30)) return -1;
    unsigned char *item = handle_object((void **)(controller + 0x30));
    if (!item) return -1;
    unsigned char *vtable = ptr(item, 0);
    const int type = ((TypeOf)ptr(vtable, 0xf8))(item);
    return type == 13 ? u32(item, 0x54) : type;
}

/* The two slots never hold the same item: roll again with the same odds. A position whose odds leave only
 * that item keeps it after a few tries, rather than getting an item its odds do not allow. */
static void differ_from(int slot, int first) {
    if (first < 0 || second[slot].type != first) return;
    void *kart = ptr(second[slot].controller, 0x18);
    for (int tries = 0; kart && tries < 24; ++tries) {
        const int type = roll(kart);
        if (type >= 0 && type <= 12 && type != first) {
            second[slot].type = type;
            return;
        }
    }
}

static void ABI crate_collected(void *controller, void *arg, unsigned char *message) {
    unsigned char *c = controller;
    void *kart = c ? ptr(c, 0x18) : NULL;
    if (runtime_nsane_race_active() && kart && (((unsigned char *)kart)[0x3b] & 1) &&
        !get_component(kart, *block_meta, 0) && (has_item(c) || grand_prix_track) && find_slot(c) < 0) {
        int type = u32(message, 0x38);
        if (type == -1) type = roll(kart);
        if (type >= 0 && type <= 12) {
            int slot = find_slot(NULL);
            if (slot < 0) slot = 0;
            second[slot].controller = c;
            second[slot].type = type;
            second[slot].spin = now_ms() + SPIN_MS;   /* runtime_nsane_tick spins, then shows `type` */
            second[slot].shown = (type + 7) % 13;
            show_second(c, second[slot].shown);
        }
    }
    native_crate(controller, arg, message);
}

static void ABI item_finished(unsigned char *controller, void *item) {
    void *before = controller ? ptr(controller, 0x30) : NULL;
    /* used while the small roulette still spins: what it shows next must differ from this one */
    const int spinning = controller && before && runtime_nsane_race_active() ? find_slot(controller) : -1;
    if (spinning >= 0 && second[spinning].spin) differ_from(spinning, hand_type(controller));
    native_finished(controller, item);
    if (!controller || !before || ptr(controller, 0x30)) return; /* the hand was not emptied */
    const int slot = find_slot(controller);
    if (slot < 0) return;
    const int type = second[slot].type;
    second[slot].controller = NULL;
    second[slot].spin = 0;
    show_second(controller, -1);
    if (runtime_nsane_race_active() && !controller[0x40]) give_item(0.0f, controller, type, 0, 0, 0);
}

/* Every frame, on the game's thread (runtime_replay_tick): the second slot's roulette. The item is already
 * chosen when the crate is taken; the icons cycle for SPIN_MS and then stop on it. */
void runtime_nsane_tick(void) {
    if (!available) return;
    const unsigned long long now = now_ms();
    for (int i = 0; i < SLOTS; ++i) {
        if (!second[i].controller || !second[i].spin) continue;
        if (!runtime_nsane_race_active()) { second[i].spin = 0; continue; }
        unsigned char *c = second[i].controller;
        int icon;
        if (now >= second[i].spin && c[0x40]) second[i].spin = now + STEP_MS;   /* waits for the first roulette */
        if (now >= second[i].spin) {
            differ_from(i, hand_type(c));
            second[i].spin = 0;
            icon = second[i].type;
        } else {
            icon = (int)((now / STEP_MS * 5 + 7) % 13);           /* 5 and 13 coprime: every icon shows */
        }
        if (icon == second[i].shown && second[i].spin) continue;
        second[i].shown = icon;
        show_second(second[i].controller, icon);
    }
}

/* A new track or the lobby: no second item carries over. */
void runtime_nsane_map_loading(const char *map) {
    memset(second, 0, sizeof second);
    /* octane/t502_.. to t508_..: the Grand Prix tracks, archives nested in update2.pak */
    grand_prix_track = map && !strncasecmp(map, "octane/t50", 10) && map[10] >= '2' && map[10] <= '8';
}

int runtime_nsane_available(void) { return available; }

/* ---- install ------------------------------------------------------------------------------ */
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

int runtime_nsane_start(unsigned char *image, size_t size) {
    /* push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; mov r12,rdi */
    static const unsigned char crate_code[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x49,0x89,0xfc};
    /* push rbp; mov rbp,rsp; push r15; push r14; push rbx; push rax; mov rbx,rsi; mov r14,rdi */
    static const unsigned char used_code[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x48,0x89,0xf3,0x49,0x89,0xfe};
    /* the call that tells the HUD the hand is empty (0xeb11c3 -> 0xe368a0) */
    static const unsigned char notify_call[] = {0xe8,0xd8,0x56,0xf8,0xff};
    if (size < 0x2ad4b78) return 1;
    unsigned char *crate = image + 0xe36230, *used = image + 0xeb10d0;
    /* Native replays (runtime_replay.c hook_bytes, installed before this) put "jmp rel32" to a
     * trampoline ("jmp [rip]" + address) over the crate function's first 6 bytes. Then N.Sane goes
     * in that trampoline and calls the replay's handler as the game's function (it runs the
     * prologue and goes on at crate + 6). */
    unsigned char *replay_trampoline = NULL;
    void *replay_handler = NULL;
    if (crate[0] == 0xe9 && !memcmp(crate + 6, crate_code + 6, sizeof crate_code - 6)) {
        int32_t rel;
        memcpy(&rel, crate + 1, sizeof rel);
        unsigned char *t = crate + 5 + rel;
        if (!memcmp(t, "\xff\x25\0\0\0\0", 6)) {
            replay_trampoline = t;
            memcpy(&replay_handler, t + 6, sizeof replay_handler);
        }
    }
    if ((!replay_handler && memcmp(crate, crate_code, sizeof crate_code)) || memcmp(used, used_code, sizeof used_code) ||
        memcmp(image + 0xeb11c3, notify_call, sizeof notify_call)) {
        puts("Native N.SANE: this game version is not supported (USA 1.21 only); mode unavailable");
        return 1; /* not fatal: the selector hides N.Sane */
    }
#ifdef _WIN32
    unsigned char *stub = VirtualAlloc(NULL, 256, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    unsigned char *stub = mmap(NULL, 256, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) stub = NULL;
#endif
    if (!stub || !writable(replay_handler ? replay_trampoline + 6 : crate, replay_handler ? 8 : 14) ||
        !writable(used, 14)) return 0;
    memcpy(stub, crate_code, sizeof crate_code);
    jump(stub + sizeof crate_code, crate + sizeof crate_code);
    memcpy(stub + 64, used_code, sizeof used_code);
    jump(stub + 64 + sizeof used_code, used + sizeof used_code);
    native_crate = replay_handler ? (CrateCollected)replay_handler : (CrateCollected)stub;
    native_finished = (ItemFinished)(stub + 64);
    give_item = (GiveItem)(image + 0xe366b0);
    get_component = (GetComponent)(image + 0x3dbf90);
    resolve_handle = (ResolveHandle)(image + 0x149fca0);
    odds_owner = (NoArgs)(image + 0x9b9ec0);
    racer_of = (RacerOf)(image + 0xfd4230);
    is_local = (IsLocal)(image + 0xc02470);
    odds_table = (OddsTable)(image + 0xe38030);
    prepare_odds = (PrepareOdds)(image + 0xe38190);
    roll_type = (RollType)(image + 0xe38280);
    statistic = (Statistic)(image + 0xfddb30);
    block_meta = (void **)(image + 0x2ac40f0);
    hud_meta = (void **)(image + 0x2ad4940);
    racers = (void **)(image + 0x2ad4b70);
    game_manager = (void **)(image + 0x2acde48);
    if (replay_handler) {
        void *handler = (void *)crate_collected;
        memcpy(replay_trampoline + 6, &handler, sizeof handler);
    } else {
        jump(crate, (void *)crate_collected);
    }
    jump(used, (void *)item_finished);
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
#endif
    available = 1;
    puts("Native N.SANE: second stored item and its HUD slot (private N.Sane races only)");
    return 1;
}
