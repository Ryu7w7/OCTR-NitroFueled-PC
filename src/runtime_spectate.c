/* Spectator build (experimental): friends watch each other's races, live only.
 *
 * While a sharing player (bbport.ini spectate_share=1, in-game menu > Friends > Options) races, the race goes
 * to the RyuNET server as it is recorded (POST /ryunet/spectate/live, about twice a second; the answer says
 * how many friends watch: SPECTATOR (n)). Spectate (Friends) follows a friend: their live race plays a few
 * seconds behind (GET /ryunet/spectate/live, new frames as they come), and each next race of theirs loads by
 * itself until Stop spectating. Finished races are not sent or downloaded. */
#include "runtime_spectate.h"
#include "runtime.h"
#include "runtime_http.h"
#include "runtime_np_online.h"
#include "runtime_replay.h"
#include "runtime_replay_list.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_RACE_BYTES (48u * 1024 * 1024)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static int started, share, state;
static unsigned serial;
static char watch_name[32], status_name[32];
static int viewers;                  /* friends watching this player's live race (the server's answer) */
static time_t viewers_at;

static int valid_nick(const char *s) {
    size_t n = strlen(s);
    if (n < 3 || n > 16) return 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static void set_state(int value, const char *who) {
    pthread_mutex_lock(&lock);
    state = value;
    if (who) snprintf(status_name, sizeof status_name, "%s", who);
    ++serial;
    pthread_mutex_unlock(&lock);
}

/* ---- live: the race being recorded goes to the server while it is driven; a friend's comes back ---- */
#define LIVE_HEADER 1136
#define LIVE_MAX_FRAMES 600         /* at most 10 s of frames per request */
static uint32_t live_sent_race, live_sent;
static int live_sent_end, live_backoff;
static char live_host[32];          /* the friend watched live, "" none */
static uint32_t live_race;
static char follow_name[32];        /* Spectate on: this friend's next races load by themselves ("" off; lock) */
static uint32_t seen_race, seen_frames; /* the followed friend's last live race seen (worker thread) */

static void put32(uint8_t *at, uint32_t v) {
    at[0] = (uint8_t)v, at[1] = (uint8_t)(v >> 8), at[2] = (uint8_t)(v >> 16), at[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *at) {
    return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
}

static void share_live(void) {
    const char *me = runtime_np_get_online_name();
    if (!me || !valid_nick(me)) return;
    if (live_backoff > 0) {
        live_backoff--;
        return;
    }
    uint32_t race = 0, frames = 0;
    int ended = 0;
    struct rr_header header;
    if (!runtime_replay_live_state(&race, &header, &frames, &ended)) return;
    if (race != live_sent_race) live_sent_race = race, live_sent = 0, live_sent_end = 0;
    if (live_sent_end || (frames <= live_sent && !ended)) return;
    uint32_t n = frames > live_sent ? frames - live_sent : 0;
    if (n > LIVE_MAX_FRAMES) n = LIVE_MAX_FRAMES;
    const size_t cap = LIVE_HEADER + 4 + 4 * (size_t)n + (size_t)n * 2 * sizeof(struct rr_frame) + 64;
    uint8_t *body = malloc(cap);
    if (!body) return;
    memcpy(body, &header, LIVE_HEADER);
    put32(body + LIVE_HEADER, n);
    size_t used = LIVE_HEADER + 4 + 4 * (size_t)n;
    for (uint32_t i = 0; i < n; i++) {
        const size_t size = runtime_replay_live_pack(race, live_sent + i, live_sent + i + 1, body + used, cap - used);
        if (!size) {            /* the race is gone (a new one started) */
            free(body);
            return;
        }
        put32(body + LIVE_HEADER + 4 + 4 * i, (uint32_t)size);
        used += size;
    }
    const int last = ended && live_sent + n == frames;
    char target[160];
    snprintf(target, sizeof target, "/ryunet/spectate/live?name=%s&race=%u&from=%u&ended=%d", me, race, live_sent, last);
    char *reply = NULL;
    size_t reply_size = 0;
    const int status = runtime_ryunet_transfer("POST", target, body, used, &reply, &reply_size, 256);
    free(body);
    if (status == 200) {
        if (!live_sent) printf("Spectate: live race %u on the server\n", race);
        int watching = 0;
        if (reply && !strncmp(reply, "ok ", 3)) watching = atoi(reply + 3);
        pthread_mutex_lock(&lock);
        if (watching != viewers) printf("Spectate: %d friend(s) watching live\n", watching);
        viewers = watching > 0 ? watching : 0;
        viewers_at = time(NULL);
        pthread_mutex_unlock(&lock);
        live_sent += n;
        live_sent_end = last;
    } else if (status == 409 && reply && !strncmp(reply, "need ", 5)) {
        const uint32_t need = (uint32_t)strtoul(reply + 5, NULL, 10);
        live_sent = need <= frames ? need : 0;
    } else {
        live_backoff = 10;      /* the server did not take it: again in about 5 s */
    }
    free(reply);
    fflush(stdout);
}

/* GET of the friend's live race from frame `from`: 200 with the stream, 404 none, 0 unreachable */
static int live_get(const char *who, uint32_t race, uint32_t from, char **data, size_t *size) {
    const char *me = runtime_np_get_online_name();
    char target[160];
    if (race) snprintf(target, sizeof target, "/ryunet/spectate/live?name=%s&host=%s&from=%u&race=%u", me, who, from, race);
    else snprintf(target, sizeof target, "/ryunet/spectate/live?name=%s&host=%s&from=0", me, who);
    const int status = runtime_ryunet_transfer("GET", target, NULL, 0, data, size, MAX_RACE_BYTES);
    if (status == 200 && (*size < 24 + LIVE_HEADER || memcmp(*data, "RRLIVE1", 8))) return 0;
    return status;
}

/* Watching: the friend's live race if there is one (it starts once the race reached GO), else their last race.
 * only_new (following): a race other than the last one seen; the server sends only what is new of that one. */
static int watch_live(const char *who, int only_new) {
    char *data = NULL;
    size_t size = 0;
    const int status = only_new && seen_race ? live_get(who, seen_race, seen_frames, &data, &size) : live_get(who, 0, 0, &data, &size);
    if (status != 200) {
        free(data);
        return status;
    }
    const uint8_t *d = (const uint8_t *)data;
    struct rr_header header;
    memcpy(&header, d + 24, LIVE_HEADER);
    const uint32_t race = get32(d + 8), frames = get32(d + 12), ended = get32(d + 20);
    if (only_new && race == seen_race) {
        seen_frames = frames;
        free(data);
        return 404;                         /* the race already seen (or left): wait for the next one */
    }
    /* from the friend's level start (before GO too): the map loads while their race starts, the replay waits for GO */
    if (ended || frames < 2) {
        if (ended) seen_race = race, seen_frames = frames;  /* an ended race is not polled whole again */
        free(data);
        return ended ? 404 : 202;           /* over, or just started */
    }
    if (!runtime_replay_can_start() && !runtime_replay_can_chain()) {
        free(data);
        return 409;
    }
    const int ok = runtime_replay_play_live(&header, d + 24 + LIVE_HEADER, size - 24 - LIVE_HEADER, frames);
    free(data);
    if (!ok) return 500;
    pthread_mutex_lock(&lock);
    snprintf(live_host, sizeof live_host, "%s", who);
    live_race = race;
    pthread_mutex_unlock(&lock);
    seen_race = race, seen_frames = frames;
    printf("Spectate: watching %s live (race %u, %u frames)\n", who, race, frames);
    fflush(stdout);
    return 200;
}

/* While watching live: the new frames */
static void follow_live(void) {
    char who[32];
    pthread_mutex_lock(&lock);
    snprintf(who, sizeof who, "%s", live_host);
    pthread_mutex_unlock(&lock);
    if (!who[0]) return;
    const uint32_t have = runtime_replay_live_count();
    if (!have) {                            /* the viewer left the replay */
        pthread_mutex_lock(&lock);
        live_host[0] = 0;
        pthread_mutex_unlock(&lock);
        return;
    }
    char *data = NULL;
    size_t size = 0;
    const int status = live_get(who, live_race, have, &data, &size);
    if (status == 200) {
        const uint8_t *d = (const uint8_t *)data;
        struct rr_header header;
        memcpy(&header, d + 24, LIVE_HEADER);
        const uint32_t race = get32(d + 8), frames = get32(d + 12), from = get32(d + 16), ended = get32(d + 20);
        if (race == live_race) seen_frames = frames;
        if (race == live_race && from == have && frames >= have)
            runtime_replay_live_append(&header, d + 24 + LIVE_HEADER, size - 24 - LIVE_HEADER, frames, (int)ended);
        else if (race != live_race)         /* the friend's next race: this one is over */
            runtime_replay_live_append(&header, NULL, 0, have, 1);
        if (ended || race != live_race) {
            pthread_mutex_lock(&lock);
            live_host[0] = 0;
            pthread_mutex_unlock(&lock);
        }
    } else if (status == 404) {             /* the stream is gone: what we have is all */
        runtime_replay_live_append(NULL, NULL, 0, have, 1);
        pthread_mutex_lock(&lock);
        live_host[0] = 0;
        pthread_mutex_unlock(&lock);
    }
    free(data);
}

/* Following (Spectate on): about every second, the friend's next live race; it starts by itself from the
 * menus or in place of a replay that ended on screen. Quiet while the viewer races or watches. */
static void follow_next(void) {
    char who[32];
    pthread_mutex_lock(&lock);
    snprintf(who, sizeof who, "%s", follow_name);
    const int now = state;
    pthread_mutex_unlock(&lock);
    if (!who[0] || live_host[0] || (!runtime_replay_can_start() && !runtime_replay_can_chain())) return;
    const char *me = runtime_np_get_online_name();
    if (!me || !valid_nick(me)) return;
    const int live = watch_live(who, 1);
    if (live == 200) {
        set_state(RS_LIVE, who);
    } else if (live == 202) {
        if (now != RS_WAITING) set_state(RS_WAITING, who);
    } else if (live == 403) {
        pthread_mutex_lock(&lock);
        follow_name[0] = 0;
        pthread_mutex_unlock(&lock);
        set_state(RS_REFUSED, who);
    } else if (live == 404 && (now == RS_LIVE || now == RS_WAITING)) {
        set_state(RS_FOLLOW, who);
    }
}

static void *worker(void *unused) {
    (void)unused;
    for (unsigned turn = 0;; turn++) {
        char who[32] = "";
        pthread_mutex_lock(&lock);
        if (!watch_name[0]) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 500000000L;
            if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
            pthread_cond_timedwait(&wake, &lock, &until);
        }
        snprintf(who, sizeof who, "%s", watch_name);
        watch_name[0] = '\0';
        const int sharing = share;
        pthread_mutex_unlock(&lock);
        if (who[0]) {                        /* Spectate pressed: the live race now, else the last race */
            seen_race = seen_frames = 0;
            const char *me = runtime_np_get_online_name();
            const int live = me && valid_nick(me) && valid_nick(who) ? watch_live(who, 0) : 0;
            if (live == 200) {
                set_state(RS_LIVE, who);
            } else if (live == 202) {
                set_state(RS_WAITING, who);
            } else if (live == 409) {
                set_state(RS_BUSY, who);
            } else if (live == 403) {
                pthread_mutex_lock(&lock);
                follow_name[0] = 0;
                pthread_mutex_unlock(&lock);
                set_state(RS_REFUSED, who);
            } else {                        /* live only: no race now, the next one loads by itself */
                set_state(RS_FOLLOW, who);
            }
        } else if (turn % 2 == 0) {
            follow_next();
        }
        follow_live();
        if (sharing) {
            share_live();
        }
    }
    return NULL;
}

void runtime_spectate_start(void) {
    pthread_mutex_lock(&lock);
    const int first = !started;
    started = 1;
    pthread_mutex_unlock(&lock);
    if (!first) return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL) == 0) {
        pthread_detach(thread);
        printf("Spectate: ready (sharing races with friends: %s)\n", share ? "on" : "off");
    }
}

void runtime_spectate_set_share(int on) {
    pthread_mutex_lock(&lock);
    share = on != 0;
    pthread_mutex_unlock(&lock);
}

void runtime_spectate_watch(const char *friend_name) {
    if (!friend_name || !valid_nick(friend_name)) return;
    pthread_mutex_lock(&lock);
    snprintf(watch_name, sizeof watch_name, "%s", friend_name);
    snprintf(follow_name, sizeof follow_name, "%s", friend_name);
    state = RS_FETCHING;
    snprintf(status_name, sizeof status_name, "%s", friend_name);
    ++serial;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
    printf("Spectate: following %s (their next races load by themselves)\n", friend_name);
    fflush(stdout);
}

void runtime_spectate_stop(void) {
    pthread_mutex_lock(&lock);
    if (follow_name[0]) printf("Spectate: no longer following %s\n", follow_name);
    follow_name[0] = watch_name[0] = 0;
    state = RS_IDLE;
    ++serial;
    pthread_mutex_unlock(&lock);
    fflush(stdout);
}

int runtime_spectate_following(char *friend_name, int size) {
    pthread_mutex_lock(&lock);
    const int on = follow_name[0] != 0;
    if (friend_name && size > 0) snprintf(friend_name, (size_t)size, "%s", follow_name);
    pthread_mutex_unlock(&lock);
    return on;
}

int runtime_spectate_viewers(void) {
    pthread_mutex_lock(&lock);
    const int n = time(NULL) - viewers_at <= 3 ? viewers : 0;   /* no race sent lately: nobody */
    pthread_mutex_unlock(&lock);
    return n;
}

unsigned runtime_spectate_status(int *out_state, char *friend_name, int size) {
    pthread_mutex_lock(&lock);
    if (out_state) *out_state = state;
    if (friend_name && size > 0) snprintf(friend_name, (size_t)size, "%s", status_name);
    const unsigned s = serial;
    pthread_mutex_unlock(&lock);
    return s;
}
