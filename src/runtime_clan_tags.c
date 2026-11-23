#define _POSIX_C_SOURCE 200809L
#include "runtime_clan_tags.h"
#include "runtime_name_colors.h"
#include "runtime_np_online.h"
#include "runtime_http.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define CACHE_SIZE 64
struct entry {
    char name[17], tag[CLAN_TAG_CAP], tiles[CLAN_TILES_CAP];
    int known, queued, swatch;
    unsigned long long used, refresh;
};
static struct entry entries[CACHE_SIZE];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static int started, save_swatch;
static const struct { const char *label; unsigned rgb; } swatches[] = {
    {"Default", 0},
#define SWATCH(label, rgb) {label, rgb},
#include "../scripts/tacho_pack/name_swatches.inc"
#undef SWATCH
};
_Static_assert(sizeof swatches / sizeof swatches[0] == NAME_SWATCH_COUNT, "palette mismatch");
_Static_assert(NAME_SWATCH_FIRST + (NAME_SWATCH_COUNT - 1)*NAME_SWATCH_STRIDE <= CLAN_TILE_FIRST &&
               CLAN_TILE_ADVANCE <= 0xf8ff, "native glyph ranges overlap or exceed private use");
const char *runtime_swatch_label(int id) {
    return id >= 0 && id < NAME_SWATCH_COUNT ? swatches[id].label : "Default";
}
unsigned runtime_swatch_rgb(int id) {
    return id > 0 && id < NAME_SWATCH_COUNT ? swatches[id].rgb : 0;
}
static const char nick_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";

static enum ClanSave status;
static char save_name[17], save_tag[CLAN_TAG_CAP];

static unsigned long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int alnum_ascii(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
static int valid_nick(const char *s) {
    if (!s || strlen(s) < 3 || strlen(s) > 16) return 0;
    for (; *s; ++s)
        if (!alnum_ascii(*s) && *s != '-' && *s != '_') return 0;
    return 1;
}

/* Validate the UTF-8 wire envelope locally. The server validates character
 * classes and counts grapheme clusters, including multi-codepoint letters. */
static int valid_tag(const char *s, size_t n) {
    if (n >= CLAN_TAG_CAP) return 0;
    int ascii = 1;
    for (size_t i = 0; i < n;) {
        unsigned c = (unsigned char)s[i++];
        if (c < 0x80) {
            if (!alnum_ascii((char)c)) return 0;
            continue;
        }
        ascii = 0;
        if (c < 0xc2 || c > 0xf4) return 0;
        unsigned more = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        unsigned rune = c & (0x7f >> (more + 1));
        if (more > n - i) return 0;
        for (unsigned j = 0; j < more; ++j) {
            unsigned next = (unsigned char)s[i++];
            if ((next & 0xc0) != 0x80) return 0;
            rune = (rune << 6) | (next & 0x3f);
        }
        if ((more == 2 && rune < 0x800) || (more == 3 && rune < 0x10000) ||
            (rune >= 0xd800 && rune <= 0xdfff) || rune > 0x10ffff ||
            rune < 0xa0 || (rune >= 0x2028 && rune <= 0x202e) ||
            (rune >= 0x2066 && rune <= 0x2069)) return 0;
    }
    return !ascii || n <= 6;
}

static void encode_tag(const char *tag, char encoded[3 * CLAN_TAG_CAP]) {
    static const char hex[] = "0123456789ABCDEF";
    char *out = encoded;
    for (; *tag; ++tag) {
        unsigned char c = (unsigned char)*tag;
        if (alnum_ascii(*tag)) *out++ = *tag;
        else {
            *out++ = '%';
            *out++ = hex[c >> 4];
            *out++ = hex[c & 15];
        }
    }
    *out = 0;
}
static int tile_text(const char *text, size_t n) {
    if (!n || n >= CLAN_TILES_CAP || n % 3) return 0;
    const unsigned char *p = (const unsigned char *)text;
    for (size_t i = 0; i < n; i += 3) {
        if ((p[i] != 0xee && p[i] != 0xef) || (p[i+1] & 0xc0) != 0x80 || (p[i+2] & 0xc0) != 0x80) return 0;
        unsigned code = ((p[i] & 15) << 12) + ((p[i+1] & 63) << 6) + (p[i+2] & 63);
        if (code < CLAN_TILE_FIRST || code > CLAN_TILE_ADVANCE) return 0;
    }
    return 1;
}
const char *runtime_clan_identity(const char *label, char account[17]) {
    if (!label) return NULL;
    // Native display glyphs are reversible; transport, rank and voice use the plain nickname.
    const unsigned char *p = (const unsigned char *)label;
    size_t letters = 0;
    while (letters < 16 && p[0] == 0xee && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        unsigned code = 0xe000 | ((p[1] & 63) << 6) | (p[2] & 63);
        if (code < NAME_SWATCH_FIRST || code >= NAME_SWATCH_FIRST + (NAME_SWATCH_COUNT-1)*NAME_SWATCH_STRIDE) break;
        account[letters++] = nick_alphabet[(code - NAME_SWATCH_FIRST) % NAME_SWATCH_STRIDE];
        p += 3;
    }
    if (letters >= 3 && (!*p || !strncmp((const char *)p, " [", 2) ||
                         !strcmp((const char *)p, RUNTIME_NAME_SPEAKER_SUFFIX))) {
        account[letters] = 0;
        return account;
    }
    const char *suffix = strstr(label, " [");
    if (!suffix) return label;
    size_t n = (size_t)(suffix - label);
    if (n < 3 || n > 16) return label;
    const char *tag = suffix + 2;
    size_t bytes = strlen(tag);
    if (bytes < 2 || tag[bytes-1] != ']' || (!valid_tag(tag, bytes-1) && !tile_text(tag, bytes-1))) return label;
    memcpy(account, label, n);
    account[n] = 0;
    return valid_nick(account) ? account : label;
}
/* Each draw samples the account color once for the entire label. The native
 * race marker may already include the voice icon when its tint is refreshed. */
int runtime_clan_color(const char *label, float rgb[3]) {
    char account[17], marker[CLAN_MARKER_CAP];
    if (!label) return 0;
    const size_t length = strlen(label), suffix = sizeof RUNTIME_NAME_SPEAKER_SUFFIX - 1;
    if (length > suffix && length < sizeof marker &&
        !strcmp(label + length - suffix, RUNTIME_NAME_SPEAKER_SUFFIX)) {
        memcpy(marker, label, length - suffix);
        marker[length - suffix] = 0;
        label = marker;
    }
    return runtime_name_color(runtime_clan_identity(label, account), rgb);
}

static int parse_reply(char *body, char tag[CLAN_TAG_CAP], int *swatch) {
    if (strncmp(body, "ok\t", 3)) return 0;
    char *end = strchr(body + 3, '\n');
    if (!end || end[1]) return 0;
    *end = 0;
    *swatch = -1; /* an older server still supports tag-only edits */
    char *field = strchr(body + 3, '\t');
    if (field) {
        *field++ = 0;
        if (*field < '0' || *field > '9') return 0;
        int id = *field++ - '0';
        if (*field) {
            if (!id || *field < '0' || *field > '9') return 0;
            id = id * 10 + (*field++ - '0');
        }
        if (*field || id >= NAME_SWATCH_COUNT) return 0;
        *swatch = id;
    }
    if (!valid_tag(body + 3, strlen(body + 3))) return 0;
    strcpy(tag, body + 3);
    return 1;
}
static struct entry *find(const char *name, int create) {
    struct entry *oldest = &entries[0];
    for (int i = 0; i < CACHE_SIZE; ++i) {
        if (!strcasecmp(entries[i].name, name)) return &entries[i];
        if (entries[i].used < oldest->used) oldest = &entries[i];
    }
    if (!create) return NULL;
    memset(oldest, 0, sizeof *oldest);
    strcpy(oldest->name, name);
    return oldest;
}
static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&lock);
    for (;;) {
        const int saving = status == CLAN_SAVING;
        char name[17], requested[CLAN_TAG_CAP] = "", tag[CLAN_TAG_CAP], target[64 + 3 * CLAN_TAG_CAP], body[CLAN_TAG_CAP + 8];
        int swatch = -1;
        const int choice = saving ? save_swatch : -1;
        if (saving) {
            strcpy(name, save_name);
            strcpy(requested, save_tag);
            char encoded[3 * CLAN_TAG_CAP];
            encode_tag(requested, encoded);
            if (choice < 0)
                snprintf(target, sizeof target, "/ryunet/clan-tag?name=%s&v=2&palette=18&tag=%s", name, encoded);
            else
                snprintf(target, sizeof target, "/ryunet/clan-tag?name=%s&v=2&palette=18&swatch=%d&tag=%s", name, choice, encoded);
        } else {
            int i = 0;
            while (i < CACHE_SIZE && !entries[i].queued) ++i;
            if (i == CACHE_SIZE) { pthread_cond_wait(&wake, &lock); continue; }
            entries[i].queued = 0;
            strcpy(name, entries[i].name);
            snprintf(target, sizeof target, "/ryunet/clan-tag?name=%s&v=2&palette=18", name);
        }
        pthread_mutex_unlock(&lock);
        int http = runtime_ryunet_request(saving ? "POST" : "GET", target, body, sizeof body);
        int ok = http == 200 && parse_reply(body, tag, &swatch) && (choice < 0 || swatch == choice) && (!saving || !strcmp(requested, tag));
        pthread_mutex_lock(&lock);
        struct entry *e = find(name, saving && ok);
        if (ok && e && strcmp(e->tag, tag)) {
            char tiles[CLAN_TILES_CAP] = "";
            pthread_mutex_unlock(&lock);
            bbgpu_clan_tiles(tag, tiles);
            pthread_mutex_lock(&lock);
            e = find(name, saving);
            if (e) strcpy(e->tiles, tiles);
        }
        if (e) {
            e->refresh = now_ms() + (ok ? 5000 : 15000);
            if (ok) {
                strcpy(e->tag, tag);
                e->known = 1;
                e->swatch = swatch;
                e->used = now_ms();
                e->queued = 0;
            }
        }
        if (saving) {
            status = ok ? CLAN_SAVED : http == 400 ? CLAN_INVALID :
                     http == 403 ? CLAN_OWNER : CLAN_UNAVAILABLE;
        }
    }
    return NULL;
}
static int start_locked(void) {
    if (!started) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, worker, NULL)) return 0;
        pthread_detach(thread);
        started = 1;
    }
    return 1;
}
int runtime_clan_lookup(const char *name, char tag[CLAN_TAG_CAP]) {
    tag[0] = 0;
    if (!valid_nick(name)) return 0;
    pthread_mutex_lock(&lock);
    struct entry *e = find(name, 1);
    e->used = now_ms();
    if (e->used >= e->refresh && start_locked()) {
        e->refresh = e->used + 15000;
        e->queued = 1;
        pthread_cond_signal(&wake);
    }
    strcpy(tag, e->tag);
    int known = e->known;
    pthread_mutex_unlock(&lock);
    return known;
}
int runtime_clan_swatch(const char *name) {
    if (!valid_nick(name)) return -1;
    pthread_mutex_lock(&lock);
    struct entry *entry = find(name, 0);
    int id = entry && entry->known ? entry->swatch : -1;
    pthread_mutex_unlock(&lock);
    return id;
}
int runtime_player_swatch(const char *name) {
    float rgb[3];
    return name && !runtime_name_color(name, rgb) ? runtime_clan_swatch(name) : 0;
}
void runtime_clan_label(const char *name, char *label, size_t cap) {
    char account[17];
    name = runtime_clan_identity(name, account);
    char tag[CLAN_TAG_CAP];
    if (runtime_clan_lookup(name, tag) && tag[0] && strlen(name) + strlen(tag) + 4 <= cap) snprintf(label, cap, "%s [%s]", name, tag);
    else snprintf(label, cap, "%s", name ? name : "");
}
/* Native and overlay fonts draw the same already-shaped bitmap. Keep the
 * original Unicode for editing, network proofs, persistence and clipboard. */
void runtime_clan_visual_label(const char *name, char *label, size_t cap) {
    char account[17], tag[CLAN_TAG_CAP];
    name = runtime_clan_identity(name, account);
    int known = runtime_clan_lookup(name, tag);
    char painted[3*16 + 1];
    const char *visible = name;
    int swatch = known ? runtime_player_swatch(name) : 0;
    if (swatch > 0) {
        char *out = painted;
        for (const char *p = name; *p; ++p) {
            unsigned code = NAME_SWATCH_FIRST + (swatch-1)*NAME_SWATCH_STRIDE +
                            (unsigned)(strchr(nick_alphabet,*p) - nick_alphabet);
            *out++ = (char)(0xe0 | (code >> 12));
            *out++ = (char)(0x80 | ((code >> 6) & 63));
            *out++ = (char)(0x80 | (code & 63));
        }
        *out = 0;
        visible = painted;
    }
    if (known && tag[0]) {
        pthread_mutex_lock(&lock);
        struct entry *e = find(name, 0);
        const char *text = e ? (e->tiles[0] ? e->tiles : e->tag) : tag;
        if (*text && strlen(visible) + strlen(text) + 4 <= cap) {
            snprintf(label, cap, "%s [%s]", visible, text);
            pthread_mutex_unlock(&lock);
            return;
        }
        pthread_mutex_unlock(&lock);
    }
    snprintf(label, cap, "%s", visible ? visible : "");
}
enum ClanSave runtime_clan_save(const char *tag) {
    return runtime_clan_save_profile(tag, -1);
}
enum ClanSave runtime_clan_save_profile(const char *tag, int swatch) {
    const char *name = runtime_np_get_online_name();
    pthread_mutex_lock(&lock);
    if (status != CLAN_SAVING) {
        if (swatch < -1 || swatch >= NAME_SWATCH_COUNT || !tag || !valid_tag(tag, strlen(tag))) status = CLAN_INVALID;
        else if (!valid_nick(name)) status = CLAN_OWNER;
        else if (!start_locked()) status = CLAN_UNAVAILABLE;
        else {
            strcpy(save_name, name);
            strcpy(save_tag, tag);
            save_swatch = swatch;
            status = CLAN_SAVING;
            pthread_cond_signal(&wake);
        }
    }
    enum ClanSave result = status;
    pthread_mutex_unlock(&lock);
    return result;
}
enum ClanSave runtime_clan_status(void) {
    pthread_mutex_lock(&lock);
    enum ClanSave result = status;
    pthread_mutex_unlock(&lock);
    return result;
}

static int editor_request;
void runtime_clan_request_editor(int open) {
    __atomic_store_n(&editor_request, open ? 1 : -1, __ATOMIC_RELEASE);
}
int runtime_clan_editor_pending(void) {
    return __atomic_load_n(&editor_request, __ATOMIC_ACQUIRE) != 0;
}
int runtime_clan_take_editor(void) {
    return __atomic_exchange_n(&editor_request, 0, __ATOMIC_ACQ_REL);
}
