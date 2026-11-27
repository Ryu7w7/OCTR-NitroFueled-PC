/* The start screen's racers read before the game's hero data exists.
 *
 * At start, 0x9f1960 reads "numHerosToSpawn" and asks 0xc08280 for each racer's name ("Name%d"),
 * whose default comes from the system data's hero table: [systemData (0x9b9ec0) + 0x10] + 0x98.
 * When that table is not there yet, the game reads NULL + 0x98 and stops (production, v0.8.7/v0.8.8:
 * 96 crashes at start, mostly with custom racer mods; guest 0xc082c0). Its callers (0x9f1960 and
 * 0xc2e400) already treat a NULL name as "no name", so then the name is NULL instead.
 * The same function at the same address in the USA and European 1.21 games.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _WIN32
#define _GNU_SOURCE   /* process_vm_readv */
#endif
#include "runtime.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

typedef void (ABI *HeroName)(void **out, int index);
typedef unsigned char *(ABI *SystemData)(void);

static HeroName native_hero_name;
static void jump(unsigned char *at, const void *target);
static int writable(void *address, size_t length);
void runtime_type_name_guard_start(unsigned char *image, size_t size);
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
    /* its own check, whatever the hero guard finds (26/11: the hero guard left itself out on players' games
     * and took this one with it) */
    runtime_type_name_guard_start(image, size);
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

/* ---- Type lookup by name hash with a registered type whose name is gone (26/11, Cloven's log) ----
 * 0x4481b0 looks a type up by the hash of its name; on a cache miss it walks every registered type (0x14f54e0 the
 * count, 0x14f54f0 the i-th) and hashes each one's name (+0x10) with 0x1574050 (FNV-1a, seed 0x811c9dc5). Back in
 * the lobby after a race on Neo Bowser City (the port's new track) one of them named memory the level's unload had
 * given back ("reserved, not committed"), and the game stopped reading it (guest 0x1574050 from 0x448259). The
 * 14 bytes at 0x44824b (mov esi,0x811c9dc5; mov rdi,[r14+0x10]; call hash) go through type_name_hash: a name that
 * is not mapped gets a hash no lookup matches, and the walk goes on. USA and EU 1.21: the same site (EU's hash is
 * 0x10 lower, read from the call). */
typedef uint32_t (ABI *NameHash)(const char *, uint32_t);
static NameHash native_name_hash;
static int type_names_skipped;

/* While on (runtime_host_readable_cache, this thread only): the readable regions already found, so a look at
 * many objects asks the system once per region. VirtualQuery is slow in this game's address space (~0.1 ms):
 * the host handoff's diagnostics asked it ~1000 times a snapshot and froze the race (26/11: 37 FPS in the
 * seconds with a snapshot, 56 without). */
static _Thread_local int readable_cache_on, readable_cache_next;
static _Thread_local const char *readable_cache_lo[16], *readable_cache_hi[16];

void runtime_host_readable_cache(int on) {
    readable_cache_on = on;
    readable_cache_next = 0;
    memset(readable_cache_lo, 0, sizeof readable_cache_lo);
    memset(readable_cache_hi, 0, sizeof readable_cache_hi);
}

/* Whether the byte at p can be read (the host's own view: any memory, the game's image included). */
static int host_readable(const void *p) {
#ifdef _WIN32
    if (readable_cache_on && p)
        for (int i = 0; i < 16; ++i)
            if (readable_cache_lo[i] && (const char *)p >= readable_cache_lo[i] && (const char *)p < readable_cache_hi[i])
                return 1;
    MEMORY_BASIC_INFORMATION m;
    if (!p || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT) return 0;
    if (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    const int readable = (m.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                       PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
    if (readable && readable_cache_on) {
        readable_cache_lo[readable_cache_next] = m.BaseAddress;
        readable_cache_hi[readable_cache_next] = (const char *)m.BaseAddress + m.RegionSize;
        readable_cache_next = (readable_cache_next + 1) & 15;
    }
    return readable;
#else
    char byte;
    struct iovec local = {&byte, 1}, remote = {(void *)p, 1};
    return p && process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == 1;
#endif
}

/* p .. p+n-1 can be read (the host handoff's diagnostics read records whose peers may be gone). */
int runtime_host_readable(const void *p, size_t n) {
    return host_readable(p) && (n < 2 || host_readable((const char *)p + n - 1));
}

/* The type name hash runs for every registered type on each lookup miss (thousands of names): asking the system
 * for each (host_readable, ~0.1 ms) took the game's thread for hundreds of ms in the lobbies (27/11, "se traba
 * en el online": the stutter log found it there). The regions found readable are kept, per thread, until the
 * guest's mappings change (runtime_memory_changes, counted before each unmap, map and protection change), when
 * a name's memory can go. */
extern volatile unsigned runtime_memory_changes;   /* runtime_memory.c */
static _Thread_local unsigned names_seen_changes;
static _Thread_local const char *names_lo[16], *names_hi[16];
static _Thread_local int names_next;
static int name_readable(const char *p) {
    if (!p) return 0;
#ifdef _WIN32
    const unsigned changes = __atomic_load_n(&runtime_memory_changes, __ATOMIC_SEQ_CST);
    if (changes != names_seen_changes) {
        names_seen_changes = changes;
        memset(names_lo, 0, sizeof names_lo);
        memset(names_hi, 0, sizeof names_hi);
    }
    for (int i = 0; i < 16; ++i)
        if (names_lo[i] && p >= names_lo[i] && p < names_hi[i]) return 1;
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        !(m.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                       PAGE_EXECUTE_WRITECOPY)))
        return 0;
    names_lo[names_next] = m.BaseAddress;
    names_hi[names_next] = (const char *)m.BaseAddress + m.RegionSize;
    names_next = (names_next + 1) & 15;
    return 1;
#else
    return host_readable(p);
#endif
}

static uint32_t ABI type_name_hash(const char *name, uint32_t seed) {
    if (!name_readable(name)) {
        if (!type_names_skipped++)
            printf("Type guard: a registered type's name is gone (%p, a level's unloaded data); skipped in the lookup\n",
                   (const void *)name);
        return ~seed;
    }
    return native_name_hash(name, seed);
}

/* ---- Diagnostics: the game's registered types around each level change (26/11) ----
 * Neo Bowser City stops the game the second time it is loaded after the main menu: applyKartBoost_VSC
 * (0x112dd80) reads the turbo pad script's data through memory another object took (0x1504095, the
 * object a vtable entry). The registered types (0x14f54e0 the count, 0x14f54f0 the i-th, +0x10 the name),
 * looked at when the game says "**push load", "LOADING LEVEL" and "level finished loading": the types
 * added and gone since the last look, those whose memory now holds something else (a registration left
 * behind by an unloaded level), and names registered twice. Only with BB_TYPE_DIAG=1 (the look at every
 * level change: one question to the system per type made each load several times slower, 26/11).
 *
 * What it showed (26/11): leaving Sewer Speedway unregisters every type it registered; leaving Neo leaves 6
 * of its scripts' data types registered (Scripts.Graph.common_TurboPadData, ..._Delay_AnimationData, the
 * Sewer barrel and tunnel camera ones: the types its design file instances), their memory given back with
 * the level. The registration (0x14a9650) does not register a type whose name is already there, so the
 * next load of Neo used the old one, and its turbo pads read their data through it.
 * So, at "LOADING LEVEL" right after a level was left (its types already unregistered, their memory not
 * taken yet), the script types ("Scripts.") the left level registered and that are still there are
 * unregistered with the game's own function (0x14a9400, the type's own unregistration: its place in the
 * list, its parent's list of children). Only for the port's own tracks (BB_CONTENT_FILES, run.py): a game
 * level that left any is only told in the log. BB_TYPE_RELEASE=0 turns it off. */
typedef int (ABI *TypeCount)(void);
typedef void *(ABI *TypeAt)(int);
typedef void (ABI *TypeUnregister)(void *type);
static TypeCount type_count;
static TypeAt type_at;
static TypeUnregister type_unregister;
static const unsigned char *type_image;   /* the game's image: a type's vtable is in it */
static size_t type_image_size;
typedef struct { const void *type; const char *name_at; char name[72]; } TypeSeen;
static TypeSeen *types_seen, *types_leaving, *types_loading;
static int types_seen_count, types_leaving_count, types_loading_count, type_diag_round;
static const char *type_last_moment;
static char type_level[128];    /* the level loaded last ("LOADING LEVEL") */
static pthread_mutex_t type_diag_lock = PTHREAD_MUTEX_INITIALIZER;

static int by_type(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const TypeSeen *)a)->type, y = (uintptr_t)((const TypeSeen *)b)->type;
    return x < y ? -1 : x > y;
}
static int by_name(const void *a, const void *b) {
    return strcmp(((const TypeSeen *)a)->name, ((const TypeSeen *)b)->name);
}
/* Readable memory regions seen during one look at the types: thousands of types and names live in a few
 * regions, so the system is asked once per region, not once per type (VirtualQuery is slow in this game's
 * address space). Valid for one look only. */
typedef struct { const char *lo[8], *hi[8]; int next; } ReadCache;
static int readable_in(ReadCache *c, const void *p, size_t n) {
    const char *a = p, *b = a + (n ? n : 1);
    if (!a) return 0;
    for (int i = 0; i < 8; ++i)
        if (c->lo[i] && a >= c->lo[i] && b <= c->hi[i]) return 1;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION m;
    if (!host_readable(a) || !VirtualQuery(a, &m, sizeof m)) return 0;
    const char *lo = m.BaseAddress, *hi = lo + m.RegionSize;
    c->lo[c->next] = lo, c->hi[c->next] = hi, c->next = (c->next + 1) & 7;
    return b <= hi || host_readable(b - 1);
#else
    (void)c;
    return host_readable(a) && host_readable(b - 1);
#endif
}

static void type_name_copy(ReadCache *c, const char *name, char *out, size_t n) {
    out[0] = 0;
    if (!name || !readable_in(c, name, 1)) { snprintf(out, n, "(gone)"); return; }
    /* to the end of its page when the bytes after it are not there */
    size_t limit = n - 1;
    if (!readable_in(c, name, limit)) limit = 4096 - ((uintptr_t)name & 4095) < limit ? 4096 - ((uintptr_t)name & 4095) : limit;
    size_t i = 0;
    for (; i < limit && name[i]; ++i)
        out[i] = name[i] >= 0x20 && name[i] < 0x7f ? name[i] : '?';
    out[i] = 0;
}
static int interesting_type(const char *name) {
    return strstr(name, "TurboPad") || strstr(name, "KartBoost") || strstr(name, "NeoBowser");
}

/* The registered types now, sorted by address (NULL when the list is not usable). */
static TypeSeen *read_types(int *count, int *unreadable) {
    int n = type_count();
    *count = n;
    *unreadable = 0;
    if (n < 0 || n > 200000) return NULL;
    TypeSeen *now = calloc((size_t)n + 1, sizeof *now);
    if (!now) return NULL;
    ReadCache cache = {0};
    for (int i = 0; i < n; ++i) {
        const unsigned char *type = type_at(i);
        now[i].type = type;
        if (type && readable_in(&cache, type, 0x18)) memcpy(&now[i].name_at, type + 0x10, sizeof(char *));
        type_name_copy(&cache, now[i].name_at, now[i].name, sizeof now[i].name);
        *unreadable += !strcmp(now[i].name, "(gone)");
    }
    qsort(now, (size_t)n, sizeof *now, by_type);
    return now;
}
static TypeSeen *copy_types(const TypeSeen *from, int n) {
    TypeSeen *copy = malloc(((size_t)n + 1) * sizeof *copy);
    if (copy && n) memcpy(copy, from, (size_t)n * sizeof *copy);
    return copy;
}
static const TypeSeen *find_type(const TypeSeen *list, int n, const TypeSeen *key) {
    return list ? bsearch(key, list, (size_t)n, sizeof *list, by_type) : NULL;
}

/* Whether level (octane/t929_neo_bowser_city/t929_neo_bowser_city) is one of the port's tracks: its archive
 * archives/<last part>.pak is in BB_CONTENT_FILES. */
static int port_track_level(const char *level) {
    const char *files = getenv("BB_CONTENT_FILES"), *name = strrchr(level, '/');
    name = name ? name + 1 : level;
    if (!files || !*name) return 0;
    char wanted[160];
    snprintf(wanted, sizeof wanted, "archives/%s.pak=", name);
    for (const char *p = files; *p;) {
        if (!strncasecmp(p, wanted, strlen(wanted))) return 1;
        const char *bar = strchr(p, '|');
        if (!bar) break;
        p = bar + 1;
    }
    return 0;
}

/* At "LOADING LEVEL" after a level was left: its script types still registered, unregistered. 1 when any. */
static int release_left_types(const TypeSeen *now, int n, const char *left_level) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("BB_TYPE_RELEASE"); off = e && e[0] == '0'; }
    if (!types_leaving || !types_loading) return 0;
    const TypeSeen *left[64];
    int count = 0;
    for (int i = 0; i < n; ++i) {
        const TypeSeen *was = find_type(types_leaving, types_leaving_count, &now[i]);
        if (!was || find_type(types_loading, types_loading_count, &now[i])) continue;   /* not the left level's */
        if (strncmp(now[i].name, "Scripts.", 8) || was->name_at != now[i].name_at || strcmp(was->name, now[i].name))
            continue;
        const unsigned char *type = now[i].type;
        if (!host_readable(type) || !host_readable(type + 0x80)) continue;
        if (count == 64) {
            printf("Type guard: more than 64 script types left by the level that was left; none unregistered\n");
            return 0;
        }
        left[count++] = &now[i];
    }
    if (!count) return 0;
    const int release = !off && type_unregister && port_track_level(left_level);
    int done = 0;
    for (int i = 0; i < count; ++i) {
        unsigned char *type = (unsigned char *)left[i]->type;
        const char *why = NULL;
        if (release) {
            /* The game's unregistration (0x14a9400) leaves a type that still has instances (0x14a9c90: +0x30 and
             * its subtypes' through +0x48): these do, the objects the level never gave back (Neo: "GlobalChunk
             * ... block(s) still allocated"), and they go with its memory. So: no instances, and one more
             * reference (+8) so the unregistration's release does not destroy it inside memory being given
             * back. Only when it is still whole (its vtable in the game's image), and not with subtypes. */
            const unsigned char *vtable, *subtypes;
            memcpy(&vtable, type, sizeof vtable);
            memcpy(&subtypes, type + 0x48, sizeof subtypes);
            if (!vtable || (type_image && (vtable < type_image || vtable >= type_image + type_image_size)))
                why = "its memory is already cleared: left";
            else if (subtypes)
                why = "it has subtypes: left";
            else {
                __atomic_add_fetch((int *)(type + 8), 1, __ATOMIC_ACQ_REL);
                int instances;
                memcpy(&instances, type + 0x30, sizeof instances);
                const int none = 0;
                memcpy(type + 0x30, &none, sizeof none);
                type_unregister(type);
                why = instances ? "unregistered (its instances stayed in the level's memory)" : "unregistered";
                ++done;
            }
        }
        printf("Type guard: \"%s\" (%p) was left registered by %s, which unloaded (its memory is given back)%s%s\n",
               left[i]->name, (void *)type, *left_level ? left_level : "the level", why ? "; " : "", why ? why : "");
    }
    if (done) {   /* the list again: each one really gone */
        int n2, unreadable;
        TypeSeen *after = read_types(&n2, &unreadable);
        for (int i = 0; after && i < count; ++i)
            if (find_type(after, n2, left[i]))
                printf("Type guard: \"%s\" (%p) is still registered\n", left[i]->name, left[i]->type);
        free(after);
    }
    return done > 0;
}

void runtime_type_diag(const char *tag, const char *level) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("BB_TYPE_DIAG"); off = !(e && e[0] == '1'); }
    if (!type_count || !type_at) return;
    /* without the diagnostics only the two looks the release needs: leaving a level and the next one's start */
    if (off && strcmp(tag, "leaving the level") && strcmp(tag, "the next level loads") && strcmp(tag, "memory given back")) {
        type_last_moment = tag;
        return;
    }
    pthread_mutex_lock(&type_diag_lock);
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    int n, unreadable;
    TypeSeen *now = read_types(&n, &unreadable);
    {
        struct timespec done;
        clock_gettime(CLOCK_MONOTONIC, &done);
        long ms = (long)(done.tv_sec - started.tv_sec) * 1000 + (done.tv_nsec - started.tv_nsec) / 1000000;
        if (ms >= 50) printf("Type guard: looking at the %d registered types took %ld ms (%s)\n", n, ms, tag);
    }
    if (!now) {
        printf("Type diag (%s): %d registered types?\n", tag, n);
        pthread_mutex_unlock(&type_diag_lock);
        return;
    }
    const int loading = !strcmp(tag, "the next level loads");
    /* a level change ("**push load" then "LOADING LEVEL"), not a race restarted in its level */
    const int changed_level = loading && type_last_moment && !strcmp(type_last_moment, "leaving the level");
    /* The left level's memory given back with objects still in it ("... still allocated.", written by the
     * thread giving it back, before it does): its types are released now, while their memory is still
     * theirs; at "LOADING LEVEL" another thread may already have walked the list (26/11: the second time
     * Neo was left, the streaming thread reached a left type first, its memory already cleared). */
    const int given_back = !strcmp(tag, "memory given back") && type_last_moment &&
                           !strcmp(type_last_moment, "leaving the level");
    if ((changed_level || given_back) && release_left_types(now, n, type_level)) {
        free(now);
        now = read_types(&n, &unreadable);
        if (!now) { pthread_mutex_unlock(&type_diag_lock); return; }
    }
    if (strcmp(tag, "memory given back")) type_last_moment = tag;   /* the level change goes on */
    if (loading && level && *level) snprintf(type_level, sizeof type_level, "%s", level);
    if (!strcmp(tag, "leaving the level")) {
        free(types_leaving);
        types_leaving = copy_types(now, n);
        types_leaving_count = types_leaving ? n : 0;
    } else if (loading && (changed_level || !types_loading)) {   /* the types before the new level's */
        free(types_loading);
        types_loading = copy_types(now, n);
        types_loading_count = types_loading ? n : 0;
    }
    if (off) {
        free(types_seen);
        types_seen = now;
        types_seen_count = n;
        pthread_mutex_unlock(&type_diag_lock);
        return;
    }
    int added = 0, gone = 0, changed = 0;
    for (int i = 0; i < n; ++i) {
        TypeSeen *was = types_seen ? bsearch(&now[i], types_seen, (size_t)types_seen_count, sizeof *now, by_type) : NULL;
        if (!was) { ++added; continue; }
        if (was->name_at != now[i].name_at || strcmp(was->name, now[i].name)) ++changed;
    }
    for (int i = 0; types_seen && i < types_seen_count; ++i)
        if (!bsearch(&types_seen[i], now, (size_t)n, sizeof *now, by_type)) ++gone;
    TypeSeen *names = malloc(((size_t)n + 1) * sizeof *names);
    int duplicates = 0;
    if (names) {
        memcpy(names, now, (size_t)n * sizeof *names);
        qsort(names, (size_t)n, sizeof *names, by_name);
        for (int i = 1; i < n; ++i)
            duplicates += !strcmp(names[i].name, names[i - 1].name) && strcmp(names[i].name, "(gone)");
    }
    int round = ++type_diag_round;
    printf("Type diag #%d (%s): %d registered types; %d added, %d gone since the last look; %d changed "
           "(their memory holds something else now); %d names unreadable; %d names registered twice\n",
           round, tag, n, types_seen ? added : n, gone, changed, unreadable, duplicates);
    int shown = 0;
    for (int i = 0; types_seen && i < n && shown < 30; ++i) {
        TypeSeen *was = bsearch(&now[i], types_seen, (size_t)types_seen_count, sizeof *now, by_type);
        if (was && (was->name_at != now[i].name_at || strcmp(was->name, now[i].name))) {
            printf("Type diag #%d:   changed %p: was \"%s\" (%p), now \"%s\" (%p)\n", round, now[i].type, was->name,
                   (const void *)was->name_at, now[i].name, (const void *)now[i].name_at);
            ++shown;
        }
    }
    shown = 0;
    for (int i = 1; names && i < n && shown < 30; ++i)
        if (!strcmp(names[i].name, names[i - 1].name) && strcmp(names[i].name, "(gone)")) {
            printf("Type diag #%d:   twice: \"%s\" at %p and %p\n", round, names[i].name, names[i - 1].type, names[i].type);
            ++shown;
        }
    shown = 0;
    for (int i = 0; types_seen && i < n && shown < 60; ++i)
        if (!bsearch(&now[i], types_seen, (size_t)types_seen_count, sizeof *now, by_type) &&
            !strncmp(now[i].name, "Scripts.", 8)) {
            printf("Type diag #%d:   added script type \"%s\" %p\n", round, now[i].name, now[i].type);
            ++shown;
        }
    shown = 0;
    for (int i = 0; types_seen && i < types_seen_count && shown < 60; ++i)
        if (!bsearch(&types_seen[i], now, (size_t)n, sizeof *now, by_type) && !strncmp(types_seen[i].name, "Scripts.", 8)) {
            printf("Type diag #%d:   gone script type \"%s\" %p\n", round, types_seen[i].name, types_seen[i].type);
            ++shown;
        }
    for (int i = 0; i < n; ++i)
        if (interesting_type(now[i].name))
            printf("Type diag #%d:   turbo type \"%s\" %p (name at %p)\n", round, now[i].name, now[i].type,
                   (const void *)now[i].name_at);
    free(names);
    free(types_seen);
    types_seen = now;
    types_seen_count = n;
    pthread_mutex_unlock(&type_diag_lock);
}

/* The address a 5-byte call at `at` goes to, or NULL when it is not one. */
static unsigned char *call_target(unsigned char *at) {
    if (at[0] != 0xe8) return NULL;
    int32_t rel;
    memcpy(&rel, at + 1, sizeof rel);
    return at + 5 + rel;
}

void runtime_type_name_guard_start(unsigned char *image, size_t size) {
    static const unsigned char site[] = {0xbe,0xc5,0x9d,0x1c,0x81, 0x49,0x8b,0x7e,0x10, 0xe8};
    unsigned char *at = image + 0x44824b;
    if (size < 0x1574100 || memcmp(at, site, sizeof site)) {
        puts("Type guard: not this game's code; left out");
        return;
    }
    /* the type list's count and i-th, as the lookup calls them (0x448220, 0x448243: USA; EU from its calls) */
    {
        unsigned char *count = call_target(image + 0x448220), *item = call_target(image + 0x448243);
        if (count && item && count > image && count < image + size && item > image && item < image + size) {
            type_count = (TypeCount)count;
            type_image = image;
            type_image_size = size;
            type_at = (TypeAt)item;
        } else {
            puts("Type diag: the type list's calls are not where expected; left out");
        }
        /* the count is "lea rax,[registry]; mov rdi,[rax]; jmp list_count" (0x14f54e0 -> 0x14a9bc0); the
         * unregistration (0x14a9400) is 0x7c0 before list_count and starts with lea r14,[registry] */
        static const unsigned char unregister_start[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x4c,0x8d,0x35};
        if (type_count && !memcmp(count, "\x48\x8d\x05", 3) && !memcmp(count + 7, "\x48\x8b\x38\xe9", 4)) {
            int32_t r1, r2, r3;
            memcpy(&r1, count + 3, 4);
            memcpy(&r2, count + 11, 4);
            unsigned char *registry = count + 7 + r1, *list_count = count + 15 + r2;
            unsigned char *unregister = list_count - 0x7c0;
            if (unregister > image && unregister + 0x20 < image + size &&
                !memcmp(unregister, unregister_start, sizeof unregister_start)) {
                memcpy(&r3, unregister + 13, 4);
                if (unregister + 17 + r3 == registry) type_unregister = (TypeUnregister)unregister;
            }
        }
        if (!type_unregister) puts("Type guard: the type unregistration is not where expected; script types left by a level stay");
    }
    int32_t rel;
    memcpy(&rel, at + 10, sizeof rel);
    unsigned char *hash = at + 14 + rel;
    if (hash != image + 0x1574050 && hash != image + 0x1574040) {
        puts("Type guard: the name hash is not the expected one; left out");
        return;
    }
#ifdef _WIN32
    unsigned char *stub = VirtualAlloc(NULL, 64, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    unsigned char *stub = mmap(NULL, 64, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) stub = NULL;
#endif
    if (!stub || !writable(at, 14)) return;
    native_name_hash = (NameHash)hash;
    /* mov esi,0x811c9dc5; mov rdi,[r14+0x10]; mov rax,type_name_hash; call rax; jmp back (0x448259) */
    size_t k = 0;
    memcpy(stub + k, site, 9), k += 9;
    const void *guard = (const void *)type_name_hash;
    stub[k++] = 0x48, stub[k++] = 0xb8;
    memcpy(stub + k, &guard, 8), k += 8;
    stub[k++] = 0xff, stub[k++] = 0xd0;
    jump(stub + k, at + 14);
    jump(at, stub);   /* exactly the 14 bytes replaced */
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
#endif
    puts("Type guard: type lookups skip registered types whose names are gone");
}
