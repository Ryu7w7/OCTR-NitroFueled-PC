/* The nicknames with a color of their own: GET /ryunet/names about once a minute, so that the list changes without a
 * new client, even during a game. "ok", then <nick> TAB <color>, where a color is #rrggbb or rainbow:<seconds of a whole
 * turn>. Until the server answers, the administrator's name turns through the colors as before.
 *
 * The game's threads read the list on every frame, in places that must not wait, so it is never changed: a new one
 * replaces it through one pointer. The old one stays in memory, since a reader may still be on it; it takes a person
 * changing a color to make another. */
#define _POSIX_C_SOURCE 200809L
#include "runtime_name_colors.h"
#include "runtime_http.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define NAMES_MAX 64
#define NICK_BYTES 17
#define POLL_SECONDS 60
#define POLL_FAILED_SECONDS 300
#define MAX_REPLY (8u * 1024)
#define TURN_MIN_MS 3000u
#define TURN_MAX_MS 120000u
#define RAINBOW_FLOOR 0.25f         /* never darker: blue stays readable on the dark rows */

typedef struct {
    char nick[NICK_BYTES];
    float rgb[3];
    unsigned turn_ms;               /* one turn through the colors; 0: always rgb */
} NameColor;

typedef struct {
    int count;
    NameColor names[NAMES_MAX];
} NameList;

static const NameList builtin = {1, {{"Ryu7w7", {0, 0, 0}, 12000}}};
static const NameList *current = &builtin;

static uint64_t monotonic_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

static void rainbow_at(uint64_t ms, unsigned turn_ms, float rgb[3]) {
    const float turn = (float)(ms % turn_ms) * 6.0f / (float)turn_ms;
    const int sector = (int)turn;
    const float up = turn - (float)sector, down = 1.0f - up;
    const float hues[6][3] = {{1, up, 0}, {down, 1, 0}, {0, 1, up}, {0, down, 1}, {up, 0, 1}, {1, 0, down}};
    for (int i = 0; i < 3; ++i) rgb[i] = RAINBOW_FLOOR + (1.0f - RAINBOW_FLOOR) * hues[sector][i];
}

int runtime_name_color(const char *nick, float rgb[3]) {
    if (!nick || !*nick) return 0;
    const NameList *list = __atomic_load_n(&current, __ATOMIC_ACQUIRE);
    for (int i = 0; i < list->count; ++i) {
        const NameColor *name = &list->names[i];
        if (strcasecmp(name->nick, nick)) continue;
        if (name->turn_ms) rainbow_at(monotonic_ms(), name->turn_ms, rgb);
        else memcpy(rgb, name->rgb, sizeof name->rgb);
        return 1;
    }
    return 0;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_color(const char *text, NameColor *name) {
    if (!strncmp(text, "rainbow:", 8)) {
        char *end = NULL;
        const unsigned long seconds = strtoul(text + 8, &end, 10);
        if (end == text + 8 || *end || seconds * 1000 < TURN_MIN_MS || seconds * 1000 > TURN_MAX_MS) return 0;
        name->turn_ms = (unsigned)seconds * 1000u;
        return 1;
    }
    if (text[0] != '#' || strlen(text) != 7) return 0;
    for (int i = 0; i < 3; ++i) {
        const int high = hex_digit(text[1 + i * 2]), low = hex_digit(text[2 + i * 2]);
        if (high < 0 || low < 0) return 0;
        name->rgb[i] = (float)(high * 16 + low) / 255.0f;
    }
    return 1;
}

/* A server's answer as a list; a line it cannot read is left out, an answer that is not one leaves the list alone. */
static int parse_names(char *reply, NameList *out) {
    char *line = reply, *end = strchr(line, '\n');
    if (!end || strncmp(line, "ok", 2) || (line[2] != '\n' && line[2] != '\r')) return 0;
    for (line = end + 1; out->count < NAMES_MAX && (end = strchr(line, '\n')) != NULL; line = end + 1) {
        *end = 0;
        if (end > line && end[-1] == '\r') end[-1] = 0;
        char *color = strchr(line, '\t');
        if (!color) continue;
        *color++ = 0;
        const size_t length = strlen(line);
        if (length < 3 || length >= NICK_BYTES) continue;
        NameColor *name = &out->names[out->count];
        memset(name, 0, sizeof *name);
        if (!parse_color(color, name)) continue;
        memcpy(name->nick, line, length + 1);
        out->count++;
    }
    return 1;
}

static int fetch_names(void) {
    char *reply = NULL;
    size_t size = 0;
    const int status = runtime_ryunet_transfer("GET", "/ryunet/names", NULL, 0, &reply, &size, MAX_REPLY);
    NameList *list = status == 200 ? calloc(1, sizeof *list) : NULL;
    char *text = list ? malloc(size + 1) : NULL;
    int ok = 0;
    if (text) {
        memcpy(text, reply, size);
        text[size] = 0;
        ok = parse_names(text, list);
    }
    free(text);
    free(reply);
    if (ok && memcmp(list, __atomic_load_n(&current, __ATOMIC_ACQUIRE), sizeof *list)) {
        __atomic_store_n(&current, list, __ATOMIC_RELEASE);
        printf("Name colors: %d from the server\n", list->count);
    } else {
        free(list);
    }
    return ok;
}

static void *worker(void *unused) {
    (void)unused;
    for (;;) {
        struct timespec wait = {fetch_names() ? POLL_SECONDS : POLL_FAILED_SECONDS, 0};
        nanosleep(&wait, NULL);
    }
    return NULL;
}

void runtime_name_colors_start(void) {
    static int started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_RELAXED)) return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL) == 0) pthread_detach(thread);
}
