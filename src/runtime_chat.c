/* The text chat of a room, never during a race: a private room's in the lobby and on the results, a public room's in the
 * lobby only (with its own option in the menu: there the others are strangers).
 *
 * Through the RyuNET server (GET and POST /ryunet/chat, signed, about once a second while in a room), not the game's
 * own traffic: the race code is not touched. Only the host knows who is in a private room: its polls carry the
 * players' nicknames (the ping list, runtime_net_ping.h), and the server lets only them in. A public room's players are
 * the server's own matchmaking: the game only says public=1 and the panel waits for its yes. The menu
 * (bbport_overlay.cpp) draws it all. */
#define _POSIX_C_SOURCE 200809L
#include "runtime_chat.h"
#include "runtime.h"
#include "runtime_http.h"
#include "runtime_net_ping.h"
#include "runtime_np_online.h"
#include "runtime_replay_list.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define OUTBOX 8
#define MAX_REPLY (64u * 1024)
#define MUTED_MAX 64

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
#define PUBLIC_ROOM "*public"     /* never a nickname */
static int started, enabled = 1, public_enabled = 1;
static int public_joined;           /* the server has this player in a public room */
static unsigned serial = 1;
static RcLine lines[RC_LINES];
static int line_count, status_now;
static char outbox[OUTBOX][RC_SEND];
static int outbox_count;
static char muted[MUTED_MAX][RC_NICK];
static int muted_count, muted_loaded;
/* worker thread */
static char room[24];
static unsigned long long since;
static int have_since;

static int nick_ok(const char *s) {
    const size_t n = strlen(s);
    if (n < 3 || n > 16) return 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int in_room(char host[24]) {
    if (runtime_online_mode_room_on()) {
        runtime_room_mode_host_name(host);
        return nick_ok(host);
    }
    if (!runtime_online_mode_public_on()) return 0;
    pthread_mutex_lock(&lock);
    const int allowed = enabled && public_enabled;
    pthread_mutex_unlock(&lock);
    if (allowed) snprintf(host, 24, "%s", PUBLIC_ROOM);
    return allowed;
}

int runtime_chat_available(void) {
    char host[24];
    if (!in_room(host) || !runtime_replay_chat_allowed()) return 0;
    if (strcmp(host, PUBLIC_ROOM)) return 1;
    pthread_mutex_lock(&lock);
    const int joined = public_joined;
    pthread_mutex_unlock(&lock);
    return joined;
}

/* What a request names: the host's room, or the public room the server has this player in. */
static int room_query(char *out, size_t size) {
    if (!strcmp(room, PUBLIC_ROOM)) return snprintf(out, size, "public=1");
    return snprintf(out, size, "room=%s", room);
}

/* holds lock */
static void add_line(unsigned id, const char *nick, const char *text) {
    if (line_count == RC_LINES) memmove(lines, lines + 1, sizeof lines[0] * --line_count);
    RcLine *l = &lines[line_count++];
    l->id = id;
    snprintf(l->nick, sizeof l->nick, "%s", nick);
    snprintf(l->text, sizeof l->text, "%s", text);
}

/* "ok <last>" and a line for each message: <id> TAB <nick> TAB <text>. */
static void apply_reply(char *reply) {
    char *line = reply, *end = strchr(line, '\n');
    if (!end || strncmp(line, "ok ", 3)) return;
    const unsigned long long last = strtoull(line + 3, NULL, 10);
    pthread_mutex_lock(&lock);
    int added = 0;
    for (line = end + 1; (end = strchr(line, '\n')) != NULL; line = end + 1) {
        *end = 0;
        char *nick = strchr(line, '\t'), *text = nick ? strchr(nick + 1, '\t') : NULL;
        if (!text) continue;
        *nick++ = 0, *text++ = 0;
        if (nick_ok(nick)) add_line((unsigned)strtoul(line, NULL, 10), nick, text), added = 1;
    }
    if (added) ++serial;
    pthread_mutex_unlock(&lock);
    since = last;           /* (lower after a server restart: from there) */
    have_since = 1;
}

static void set_status(int value) {
    pthread_mutex_lock(&lock);
    if (status_now != value) status_now = value, ++serial;
    pthread_mutex_unlock(&lock);
}

static void flush_outbox(const char *me) {
    for (;;) {
        char text[RC_SEND];
        pthread_mutex_lock(&lock);
        if (!outbox_count) {
            pthread_mutex_unlock(&lock);
            return;
        }
        memcpy(text, outbox[0], sizeof text);
        memmove(outbox, outbox + 1, sizeof outbox[0] * --outbox_count);
        pthread_mutex_unlock(&lock);
        char target[120], where[40];
        room_query(where, sizeof where);
        snprintf(target, sizeof target, "/ryunet/chat?name=%s&%s", me, where);
        char *reply = NULL;
        size_t size = 0;
        const int status = runtime_ryunet_transfer("POST", target, text, strlen(text), &reply, &size, 256);
        free(reply);
        set_status(status == 200 ? RC_STATUS_OK : status == 429 ? RC_STATUS_WAIT : status == 403 ? RC_STATUS_REFUSED : RC_STATUS_FAILED);
    }
}

static void poll_room(const char *me) {
    char target[420], where[40];
    const int public_room = !strcmp(room, PUBLIC_ROOM);
    room_query(where, sizeof where);
    int used = snprintf(target, sizeof target, "/ryunet/chat?name=%s&%s", me, where);
    if (have_since) used += snprintf(target + used, sizeof target - (size_t)used, "&since=%llu", since);
    if (!public_room && !runtime_room_mode_following()) {   /* the host: who is in the room */
        PingPeer peers[PING_MAX_PEERS];
        const int count = runtime_net_ping_view(peers, PING_MAX_PEERS);
        used += snprintf(target + used, sizeof target - (size_t)used, "&roster=");
        for (int i = 0, first = 1; i < count; i++) {
            if (!nick_ok(peers[i].name) || (size_t)used + 20 >= sizeof target) continue;
            used += snprintf(target + used, sizeof target - (size_t)used, "%s%s", first ? "" : ",", peers[i].name);
            first = 0;
        }
    }
    char *reply = NULL;
    size_t size = 0;
    const int status = runtime_ryunet_transfer("GET", target, NULL, 0, &reply, &size, MAX_REPLY);
    if (public_room && status) {        /* (unreachable for a moment: as it was) */
        pthread_mutex_lock(&lock);
        const int was = public_joined;
        public_joined = status == 200;
        if (was != public_joined) ++serial;
        if (was && !public_joined) line_count = 0;
        pthread_mutex_unlock(&lock);
        if (status != 200) have_since = 0;
    }
    if (status == 200 && reply) {
        char *text = malloc(size + 1);
        if (text) {
            memcpy(text, reply, size);
            text[size] = 0;
            apply_reply(text);
            free(text);
        }
    }
    free(reply);
}

static void leave_room(const char *host) {
    snprintf(room, sizeof room, "%s", host);
    since = 0;
    have_since = 0;
    pthread_mutex_lock(&lock);
    line_count = outbox_count = 0;
    public_joined = 0;
    status_now = RC_STATUS_OK;
    ++serial;
    pthread_mutex_unlock(&lock);
}

static void *worker(void *unused) {
    (void)unused;
    for (;;) {
        char host[24] = "";
        const int inside = in_room(host);
        if (!inside) host[0] = 0;
        if (strcasecmp(host, room)) leave_room(host);
        const char *me = runtime_np_get_online_name();
        pthread_mutex_lock(&lock);
        const int on = enabled;
        pthread_mutex_unlock(&lock);
        /* during the race nothing is asked: what was said comes after it */
        const int guest = runtime_room_mode_following() || !strcmp(host, PUBLIC_ROOM);
        if (inside && me && nick_ok(me) && runtime_replay_chat_allowed() && (on || !guest)) {
            flush_outbox(me);
            poll_room(me);
        }
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 1;
        pthread_mutex_lock(&lock);
        if (!outbox_count) pthread_cond_timedwait(&wake, &lock, &until);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

static void muted_path(char *out, size_t size) { snprintf(out, size, "%s/chat_muted.txt", runtime_file_progress_dir()); }

/* holds lock */
static void load_muted(void) {
    if (muted_loaded) return;
    muted_loaded = 1;
    char path[800], line[64];
    muted_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (muted_count < MUTED_MAX && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (nick_ok(line)) memcpy(muted[muted_count++], line, strlen(line) + 1);   /* 16 letters at most */
    }
    fclose(f);
}

/* holds lock */
static int muted_index(const char *nick) {
    load_muted();
    for (int i = 0; i < muted_count; i++)
        if (!strcasecmp(muted[i], nick)) return i;
    return -1;
}

void runtime_chat_mute(const char *nick, int mute) {
    if (!nick || !nick_ok(nick)) return;
    pthread_mutex_lock(&lock);
    const int at = muted_index(nick);
    if (mute && at < 0 && muted_count < MUTED_MAX) {
        snprintf(muted[muted_count++], RC_NICK, "%s", nick);
    } else if (!mute && at >= 0) {
        memmove(muted[at], muted[at + 1], sizeof muted[0] * (size_t)(--muted_count - at));
    }
    char path[800];
    muted_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) {
        for (int i = 0; i < muted_count; i++) fprintf(f, "%s\n", muted[i]);
        fclose(f);
    }
    ++serial;
    pthread_mutex_unlock(&lock);
}

int runtime_chat_muted(const char *nick) {
    pthread_mutex_lock(&lock);
    const int at = nick ? muted_index(nick) : -1;
    pthread_mutex_unlock(&lock);
    return at >= 0;
}

unsigned runtime_chat_serial(void) {
    pthread_mutex_lock(&lock);
    const unsigned s = serial;
    pthread_mutex_unlock(&lock);
    return s;
}

unsigned runtime_chat_view(RcLine *out, int max, int *count, int *status) {
    pthread_mutex_lock(&lock);
    int n = 0;
    for (int i = line_count - 1; i >= 0 && n < max; i--)
        if (muted_index(lines[i].nick) < 0) out[n++] = lines[i];
    for (int i = 0; i < n / 2; i++) {       /* newest first until here */
        RcLine swap = out[i];
        out[i] = out[n - 1 - i];
        out[n - 1 - i] = swap;
    }
    *count = n;
    if (status) *status = status_now;
    const unsigned s = serial;
    pthread_mutex_unlock(&lock);
    return s;
}

int runtime_chat_send(const char *text) {
    if (!text || !*text || strlen(text) >= RC_SEND) return 0;
    pthread_mutex_lock(&lock);
    const int ok = enabled && outbox_count < OUTBOX;
    if (ok) {
        snprintf(outbox[outbox_count++], RC_SEND, "%s", text);
        pthread_cond_signal(&wake);
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

void runtime_chat_set_enabled(int on) {
    pthread_mutex_lock(&lock);
    enabled = on != 0;
    pthread_mutex_unlock(&lock);
}

void runtime_chat_set_public(int on) {
    pthread_mutex_lock(&lock);
    public_enabled = on != 0;
    pthread_mutex_unlock(&lock);
}

void runtime_chat_start(void) {
    pthread_mutex_lock(&lock);
    const int first = !started;
    started = 1;
    pthread_mutex_unlock(&lock);
    if (!first) return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL) == 0) pthread_detach(thread);
}
