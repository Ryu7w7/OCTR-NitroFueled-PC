/* The launchers' state (src/runtime_state.c): ryunet_state.txt next to bbport.ini carries
 * online, the nickname and the room's peers, written when they change. The online answer and
 * the nickname (runtime_net.c, runtime_np_online.c) are stubbed, as
 * tests/test_gpu_runtime_stubs.cpp stubs their parts. */
#define _GNU_SOURCE
#include "runtime_net_ping.h"
#include "runtime_state.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int runtime_net_is_online(void) {
    const char *v = getenv("BB_ONLINE");
    return v && strcmp(v, "1") == 0;
}
const char *runtime_np_get_online_name(void) { return "Alice"; }

/* The clock the ping's live window runs on: a peer lives 5000 ms of it. */
static unsigned long long real_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000u + (unsigned long long)ts.tv_nsec / 1000000u;
}

/* The room the test keeps, and a thread that explores it every 200 ms (as the game's own
 * packets keep the peers live): without it the states' 5-second ticks can run after the
 * room's 5-second life and the writer turns the room off before this test reads it. */
#define MAX_ROOM 8
static struct {
    uint32_t ip[MAX_ROOM];
    uint16_t port[MAX_ROOM];
    const char *name[MAX_ROOM];
    int count;
} room;
static int stopped;

static void *room_packets(void *arg) {
    (void)arg;
    uint8_t in[600], reply[600];
    size_t reply_len = 0;
    while (!stopped) {
        for (int i = 0; i < room.count; i++) {
            const size_t n = strlen(room.name[i]);
            memcpy(in, PING_MAGIC, PING_MAGIC_LEN);
            in[6] = 1; /* kind: a ping */
            memset(in + 7, 0, 4);
            in[11] = (uint8_t)n;
            memcpy(in + 12, room.name[i], n);
            ping_on_packet(in, 12 + n, room.ip[i], room.port[i], real_ms(), reply, sizeof reply,
                           &reply_len);
            ping_on_game_send(room.ip[i], room.port[i], real_ms());
        }
        usleep(200 * 1000);
    }
    return NULL;
}

/* The room is this from then on: named peers (who ping back) and `unnamed` more, too old for a
 * name. */
static void room_set(uint32_t first_ip, uint16_t first_port, const char *const *names, int named,
                     int unnamed) {
    room.count = 0;
    for (int i = 0; i < named && i < MAX_ROOM; i++) {
        room.ip[i] = first_ip + (uint32_t)i;
        room.port[i] = (uint16_t)(first_port + i);
        room.name[i] = names[i];
        room.count++;
    }
    for (int i = 0; i < unnamed && room.count < MAX_ROOM; i++) {
        room.ip[room.count] = first_ip + 100u + (uint32_t)i;
        room.port[room.count] = first_port;
        room.name[room.count] = "";
        room.count++;
    }
}

/* The file body (empty when no file states at all; the writer works on its thread). */
static void read_body(const char *dir, char *out, size_t size) {
    char path[400];
    snprintf(path, sizeof path, "%s/ryunet_state.txt", dir);
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    const size_t got = fread(out, 1, size - 1, f);
    out[got] = '\0';
    fclose(f);
}

/* The first body that matches (the states turn over every 5 seconds; 60 s is plenty). */
static void wait_body(const char *dir, const char *about, char *out, size_t size) {
    out[0] = '\0';
    for (int tries = 0; tries < 60 * 10; tries++) {
        usleep(100 * 1000);
        char body[1024];
        read_body(dir, body, sizeof body);
        if (strstr(body, about)) { snprintf(out, size, "%s", body); return; }
    }
    CHECK(0 && "a body with the awaited state never arrived");
    read_body(dir, out, size);
}

static void put_pointer(unsigned char *at, const void *value) { memcpy(at, &value, sizeof value); }
static void put_int(unsigned char *at, int value) { memcpy(at, &value, sizeof value); }

static void test_game_snapshot(void) {
    const size_t size = 0x2ad4e38;
    unsigned char *image = calloc(1, size);
    CHECK(image != NULL);
    if (!image) return;
    char body[512];
    runtime_state_bind(image, size);
    runtime_state_game(body, sizeof body);
    CHECK(!*body); /* unsupported executable */
    memcpy(image+0x94efa0, "\x48\x8b\x05\x31\x4d\x16\x02", 7);
    memcpy(image+0x10552a4, "\x8b\x05\xde\xd5\xa3\x01", 6);
    memcpy(image+0x10ddd66, "\x8b\x90\x44\x01\x00\x00", 6);
    memcpy(image+0x1d424aa, "CRaceSettingsManager", 20);
    unsigned char manager[0x148] = {0}, octane[0xd50] = {0}, rm[0x18] = {0};
    unsigned char list[0x28] = {0}, settings[0x10] = {0}, racer[0xb8] = {0}, ai[0xb8] = {0};
    void *entries[] = {ai, racer};
    put_pointer(image+0x2ab3cd8, manager);
    put_pointer(image+0x2acde48, octane);
    put_pointer(image+0x2ad4b70, rm);
    put_pointer(image+0x2ad4e30, settings);
    put_pointer(rm+0x10, list);
    put_pointer(list+0x20, entries);
    put_int(manager+0x144, 8);
    put_int(octane+0xd4c, 2);
    put_int(settings+0xc, 3);
    put_pointer(racer, image+0x26d5f78);
    put_pointer(ai, image+0x26d5f78);
    ai[0x40] = ai[0xb0] = racer[0xb0] = 1;
    put_int(racer+0x4c, 2);
    runtime_state_bind(image, size);
    runtime_state_game(body, sizeof body);
    CHECK(strstr(body, "phase=Menus / loading\n") && !strstr(body, "race="));
    put_int(list+0xc, 2);
    runtime_state_game(body, sizeof body);
    CHECK(strstr(body, "mode=Local Arcade\n") && strstr(body, "race=Time Trial\n"));
    CHECK(strstr(body, "lap=2\nlaps=3\n") && !strstr(body, "race_seconds="));

    unsigned char pool[0x30] = {0}, generations[0x28] = {0}, clock[0x50] = {0};
    unsigned char timer[0x20] = {0}, data[0x10] = {0}, time[0x30] = {0};
    uint16_t generation = 0x8000;
    unsigned char entity[0x20] = {0}, components[0x28] = {0};
    void *entities[] = {entity}, *clocks[] = {clock};
    put_pointer(entity+0x18, components);
    put_int(components+0xc, 1);
    put_pointer(components+0x20, clocks);
    put_int(image+0x2a92888, 0x8000);
    put_pointer(image+0x2a92880, pool);
    put_pointer(pool+0x28, generations);
    put_pointer(pool+0x20, entities);
    put_int(generations+0xc, 1);
    put_pointer(generations+0x20, &generation);
    put_pointer(clock, image+0x2592f98);
    put_pointer(clock+0x28, timer);
    put_pointer(clock+0x48, data);
    put_pointer(image+0x2ab4100, time);
    put_int(timer+0xc, 8192);
    put_int(time+0x20, 84*8192);
    timer[0x14] = 1;
    runtime_state_game(body, sizeof body);
    CHECK(strstr(body, "race_seconds=83\n"));
    time[0x28] = 1;
    runtime_state_game(body, sizeof body);
    CHECK(strstr(body, "phase=Paused\n") && strstr(body, "race_seconds=83\n"));
    time[0x28] = 0;
    timer[0x15] = 1;
    put_int(timer+0x10, 43*8192);
    runtime_state_game(body, sizeof body);
    CHECK(strstr(body, "race_seconds=42\n")); /* stopped timer uses game stop time */
    generation = 0;
    runtime_state_game(body, sizeof body);
    CHECK(!strstr(body, "race_seconds=")); /* stale component handle */
    generation = 0x8000;
    float remaining = 12.75f;
    put_int(data+0xc, 1);
    memcpy(clock+0x3c, &remaining, sizeof remaining);
    put_int(octane+0xd4c, 9);
    runtime_state_game(body, sizeof body);
    CHECK(strstr(body, "race_seconds=12\n") && !strstr(body, "lap="));
    racer[0x8c] = 1;
    put_int(racer+0x6c, 723615); /* engine timestamp, not floating-point race seconds */
    runtime_state_game(body, sizeof body);
    CHECK(strstr(body, "phase=Finished\n") && !strstr(body, "race_seconds="));
    entries[1] = (void *)(uintptr_t)0xdead0000;
    runtime_state_game(body, sizeof body); /* unmapped guest object must not crash */
    CHECK(strstr(body, "phase=Menus / loading\n"));
    runtime_state_bind(NULL, 0);
    free(image);
}

int main(void) {
    test_game_snapshot();
    char pattern[] = "/tmp/ryunet-state-test-XXXXXX";
    char *dir = mkdtemp(pattern);
    if (!dir) { perror("mkdtemp"); return 1; }
    /* BB_CONFIG is bbport.ini's whole path: the state is written next to it */
    char cfg[400];
    snprintf(cfg, sizeof cfg, "%s/bbport.ini", dir);
    setenv("BB_CONFIG", cfg, 1);
    setenv("BB_ONLINE", "1", 1);

    pthread_t packets;
    pthread_create(&packets, NULL, room_packets, NULL);

    runtime_state_start();

    /* An online game with a room of three: two named, one whose port is too old for a name. */
    const char *two[] = {"Andy", "Carol"};
    room_set(0x0A000001, 7000, two, 2, 1);
    char body[1024];
    wait_body(dir, "peers=Andy,Carol", body, sizeof body);
    CHECK(strstr(body, "online=1\n") && strstr(body, "nick=Alice\n") && strstr(body, "room=3\n"));
    long long at = 0;
    (void)sscanf(strstr(body, "at="), "at=%lld", &at);
    CHECK(at > 0 && (long long)time(NULL) - at <= 10);

    /* Offline again, one of the room (who the game left it with): the body changes over. */
    setenv("BB_ONLINE", "0", 1);
    const char *bob[] = {"Bob"};
    room_set(0x0A000004, 7003, bob, 1, 0);
    char body2[1024];
    wait_body(dir, "peers=Bob\n", body2, sizeof body2);
    CHECK(strstr(body2, "online=0\n") && strstr(body2, "room=1\n"));

    /* A room of six: all six counted, at most four named, the rest "+how many more". */
    setenv("BB_ONLINE", "1", 1);
    const char *six[] = {"P1", "P2", "P3", "P4", "P5", "P6"};
    room_set(0x0B000001, 7100, six, 6, 0);
    char body3[1024];
    wait_body(dir, "peers=P1,P2,P3,P4\n", body3, sizeof body3);
    CHECK(strstr(body3, "online=1\n") && strstr(body3, "room=6\n") && strstr(body3, "extra=2\n"));

    stopped = 1;
    pthread_join(packets, NULL);
    printf("%s\n", failures ? "FAILURES" : "test_state: OK");
    return failures ? 1 : 0;
}
