/* ryunet_state.txt beside bbport.ini: network peers plus a bounded game snapshot.
 * Replaced atomically every second, with a Unix timestamp for stale-file detection.
 * Game layout evidence and remaining live checks: docs/sacredfire/discord-game-state.md. */
#define _GNU_SOURCE
#include <pthread.h>
#include <math.h>
#ifndef _WIN32
#include <sys/uio.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#define fsync _commit
#endif
#include "runtime_net.h"
#include "runtime_net_ping.h"
#include "runtime_np_online.h"
#include "runtime_state.h"

#define STATE_SECONDS 1
#define STATE_NAMES 4
#define STATE_NAME "ryunet_state.txt"

/* EU 1.21: see docs/sacredfire/discord-game-state.md for the disassembly evidence.
 * Never call guest methods from this polling thread or dereference a guest pointer directly. */
static uintptr_t game_image;
static int read_game(uintptr_t address, void *out, size_t size) {
    if (address < 4096 || address > UINTPTR_MAX - size) return 0;
#ifdef _WIN32
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (void *)address, out, size, &got) && got == size;
#else
    struct iovec local = {out, size}, remote = {(void *)address, size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
#endif
}
static uintptr_t pointer_at(uintptr_t at) {
    uintptr_t value = 0;
    return read_game(at, &value, sizeof value) ? value : 0;
}
static int int_at(uintptr_t at, int fallback) {
    int value;
    return read_game(at, &value, sizeof value) ? value : fallback;
}

void runtime_state_bind(const unsigned char *image, size_t size) {
    /* These unmodified getter instructions identify the singleton layout, not just the title ID. */
    const unsigned char manager[] = {0x48,0x8b,0x05,0x31,0x4d,0x16,0x02};
    const unsigned char clock[] = {0x8b,0x05,0xde,0xd5,0xa3,0x01};
    const unsigned char mode[] = {0x8b,0x90,0x44,0x01,0x00,0x00};
    uintptr_t base = 0;
    if (size >= 0x2ad4e38 && !memcmp(image+0x94efa0, manager, sizeof manager) &&
        !memcmp(image+0x10552a4, clock, sizeof clock) &&
        !memcmp(image+0x10ddd66, mode, sizeof mode) &&
        !memcmp(image+0x1d424aa, "CRaceSettingsManager", 20)) base = (uintptr_t)image;
    __atomic_store_n(&game_image, base, __ATOMIC_RELEASE);
    printf("Discord game state: %s\n", base ? "EU 1.21 reader enabled" : "unsupported executable; basic presence only");
}

static uintptr_t race_clock(uintptr_t base) {
    /* getRaceClockTime_VSC's cached Alchemy handle, including its generation check. */
    int handle = int_at(base+0x2a92888, 0), index = handle & 0x7fff;
    if (!(handle & 0x8000) || handle == int_at(base+0x2a922d0, handle)) return 0;
    uintptr_t pool = pointer_at(base+0x2a92880), generations = pointer_at(pool+0x28);
    if (!pool || index >= int_at(generations+0xc, 0)) return 0;
    uint16_t generation;
    if (!read_game(pointer_at(generations+0x20)+index*2, &generation, sizeof generation) ||
        generation != (index | 0x8000)) return 0;
    uintptr_t entity = pointer_at(pointer_at(pool+0x20)+index*8);
    uintptr_t components = pointer_at(entity+0x18), entries = pointer_at(components+0x20);
    int count = int_at(components+0xc, -1);
    if (count < 0 || count > 256) return 0;
    /* The handle resolves the clock entity; getRaceClockTime then calls getComponent (0x3dbf90). */
    for (int i = 0; i < count; i++) {
        uintptr_t clock = pointer_at(entries+i*8);
        unsigned char flags;
        if (pointer_at(clock) == base+0x2592f98 && read_game(clock+0x20, &flags, 1) && !(flags & 0x10))
            return clock;
    }
    return 0;
}

void runtime_state_game(char *out, size_t size) {
    if (!size) return;
    out[0] = 0;
    uintptr_t base = __atomic_load_n(&game_image, __ATOMIC_ACQUIRE);
    if (!base) return;
    uintptr_t manager = pointer_at(base+0x2ab3cd8);
    if (!manager) return;
    uintptr_t loading = pointer_at(manager+0x88);
    if (pointer_at(loading) != base+0x2540eb8) {
        snprintf(out, size, "phase=Starting game\n");
        return;
    }
    int loading_state = int_at(loading+0xc, -1);
    if (loading_state >= 1 && loading_state <= 4) {
        snprintf(out, size, "phase=Loading\n");
        return;
    }
    if (loading_state != 0) return;
    char track[160] = "";
    uintptr_t track_ptr = pointer_at(manager+0xc0);
    for (size_t i = 0; i < sizeof track-1; i++) {
        unsigned char c;
        if (!read_game(track_ptr+i, &c, 1) || (c && (c < 32 || c >= 127))) {
            track[0] = 0;
            break;
        }
        track[i] = (char)c;
        if (!c) break;
    }
    char level[160];
    snprintf(level, sizeof level, "%s", strrchr(track, '/') ? strrchr(track, '/')+1 : track);
    for (char *c = level; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 'a'-'A';
    if (!strcmp(level, "octane_startscreen") || !strcmp(level, "octane_onlinelobby") ||
        !strcmp(level, "octane_scrapbook") || !strcmp(level, "octane_credits")) {
        snprintf(out, size, "phase=In menus\n");
        return;
    }
    int game_mode = int_at(manager+0x144, 0);
    const char *mode = game_mode == 1 ? "Adventure" : game_mode == 8 ? "Local Arcade" :
                       game_mode == 16 ? "Online" : "";
    uintptr_t racers = pointer_at(pointer_at(base+0x2ad4b70)+0x10);
    int count = racers ? int_at(racers+0xc, -1) : 0;
    uintptr_t racer = 0, entries = pointer_at(racers+0x20);
    if (count < 0 || count > 16) return;
    for (int i = 0; i < count; i++) {
        uintptr_t candidate = pointer_at(entries+i*8);
        unsigned char ai = 1, local = 0;
        if (pointer_at(candidate) == base+0x26d5f78 &&
            read_game(candidate+0x40, &ai, 1) && read_game(candidate+0xb0, &local, 1) && !ai && local) {
            racer = candidate;
            break; /* first local human in split screen */
        }
    }
    if (!racer) {
        snprintf(out, size, "phase=In menus\n");
        return;
    }
    static const char *const races[] = {"", "Single Race", "Time Trial", "Relic Race", "Boss Race",
        "CTR Challenge", "Cup Race", "Adventure Hub", "Crystal Challenge", "Limit Battle",
        "Capture the Flag", "Last Kart Driving", "Steal the Bacon", "Crystal Grab", "Sports", "Ring Rally"};
    int race = int_at(pointer_at(base+0x2acde48)+0xd4c, 0);
    if (race <= 0 || race >= (int)(sizeof races / sizeof *races)) return;
    int lap = int_at(racer+0x4c, -1), laps = int_at(pointer_at(base+0x2ad4e30)+0xc, 0);
    unsigned char finished = 0;
    if (!read_game(racer+0x8c, &finished, 1)) return;
    const char *phase = race == 7 ? "Exploring" : finished ? "Finished" : "Racing";
    double seconds = -1;
    uintptr_t clock = race_clock(base), timer = pointer_at(clock+0x28);
    int clock_mode = int_at(pointer_at(clock+0x48)+0xc, -1);
    if (clock && timer) {
        if (clock_mode == 1) {
            float remaining;
            if (read_game(clock+0x3c, &remaining, sizeof remaining)) seconds = remaining;
        } else if (clock_mode == 0 && int_at(timer+0x18, -1) == 0) {
            unsigned char running = 0, stopped = 0;
            int start = int_at(timer+0xc, -1), end = int_at(timer+0x10, -1);
            if (read_game(timer+0x14, &running, 1) && read_game(timer+0x15, &stopped, 1)) {
                if (running && !stopped) end = int_at(pointer_at(base+0x2ab4100)+0x20, -1);
                if (start >= 0 && end >= start) seconds = (end-(double)start)/8192.0;
                if (!running && !finished) phase = "Starting race";
            }
        }
    }
    /* _endOfRaceTime is an engine timestamp, not a floating-point race duration. */
    if (finished) seconds = -1;
    unsigned char paused = 0;
    if (!finished && read_game(pointer_at(base+0x2ab4100)+0x28, &paused, 1) && paused == 1)
        phase = "Paused";
    char lap_text[48] = "", time_text[48] = "";
    if (race >= 1 && race <= 6 && lap >= 0 && laps > 0 && laps <= 99 && lap <= laps+1)
        snprintf(lap_text, sizeof lap_text, "lap=%d\nlaps=%d\n", lap < 1 ? 1 : lap > laps ? laps : lap, laps);
    if (race != 7 && isfinite(seconds) && seconds >= 0 && seconds < 86400)
        snprintf(time_text, sizeof time_text, "race_seconds=%d\n", (int)seconds);
    char timestamp[64] = "";
    unsigned char running = 0, stopped = 1;
    if (!strcmp(phase, "Racing") && isfinite(seconds) && seconds >= 0 && seconds < 86400 &&
        read_game(timer+0x14, &running, 1) && read_game(timer+0x15, &stopped, 1) && running && !stopped) {
        struct timespec now;
        timespec_get(&now, TIME_UTC);
        double epoch = now.tv_sec + now.tv_nsec / 1e9;
        snprintf(timestamp, sizeof timestamp, "%s=%lld\n", clock_mode == 1 ? "timer_end" : "timer_start",
                 (long long)((clock_mode == 1 ? epoch+seconds : epoch-seconds) + 0.5));
    }
    snprintf(out, size, "phase=%s\nmode=%s\nrace=%s\ntrack=%s\n%s%s%s", phase, mode, races[race], track, lap_text, time_text, timestamp);
}

/* The same clock the ping's views run on (runtime_net.c's now_ms, here the state's own). */
static uint64_t ms_now(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* ryunet_announce.txt's path in runtime_friends.c: a root of the BB_CONFIG (bbport.ini) path,
 * with no BB_CONFIG the launcher's working directory. */
static void state_path(char *out, size_t size) {
    const char *cfg = getenv("BB_CONFIG");
    const char *slash = cfg ? strrchr(cfg, '/') : NULL, *bslash = cfg ? strrchr(cfg, '\\') : NULL;
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash) snprintf(out, size, "%.*s/" STATE_NAME, (int)(slash - cfg), cfg);
    else snprintf(out, size, STATE_NAME);
}

static void write_state(const char *body) {
    char path[600], temp[640];
    state_path(path, sizeof path);
    snprintf(temp, sizeof temp, "%s.bbtmp", path);
    FILE *f = fopen(temp, "w");
    if (!f) return;
    int ok = fputs(body, f) >= 0 && !fflush(f) && !fsync(fileno(f));
    ok = !fclose(f) && ok;
#ifdef _WIN32
    /* The CRT's rename fails when the target exists; MoveFileEx replaces it. */
    if (!ok || !MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        unlink(temp);
#else
    if (!ok || rename(temp, path)) unlink(temp);
#endif
}

static void *state_thread(void *arg) {
    (void)arg;
    char last[1024] = "";
    for (;;) {
        PingPeer peers[PING_MAX_PEERS];
        const int room = ping_view(ms_now(), peers, PING_MAX_PEERS);
        int named = 0, listed = 0; /* at most STATE_NAMES' nicknames; a longer room gets extra= */
        char names[128] = "";
        for (int i = 0; i < room; i++) if (*peers[i].name) named++;
        for (int i = 0; i < room && listed < STATE_NAMES && listed < named; i++) {
            if (!*peers[i].name) continue;
            if (listed) strncat(names, ",", sizeof names - strlen(names) - 1);
            strncat(names, peers[i].name, sizeof names - strlen(names) - 1);
            listed++;
        }
        const int extra = named > listed ? named - listed : 0;
        const char *nick = runtime_np_get_online_name();
        char body[1024], game[512];
        runtime_state_game(game, sizeof game);
        if (extra > 0)
            snprintf(body, sizeof body,
                     "online=%d\nnick=%s\nroom=%d\npeers=%s\nextra=%d\nat=%lld\n%s",
                     runtime_net_is_online() ? 1 : 0, nick && *nick ? nick : "", room, names,
                     extra, (long long)time(NULL), game);
        else
            snprintf(body, sizeof body,
                     "online=%d\nnick=%s\nroom=%d\npeers=%s\nat=%lld\n%s",
                     runtime_net_is_online() ? 1 : 0, nick && *nick ? nick : "", room, names,
                     (long long)time(NULL), game);
        if (strcmp(body, last)) {
            snprintf(last, sizeof last, "%s", body);
            write_state(body);
        }
        const struct timespec wait = {STATE_SECONDS, 0};
        nanosleep(&wait, NULL);
    }
    return NULL;
}

void runtime_state_start(void) {
    static int started;
    if (started) return;
    started = 1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, state_thread, NULL) == 0) pthread_detach(thread);
    else started = 0;
}
