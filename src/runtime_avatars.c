/* Tacho: the players' Discord pictures for the menu (runtime_avatars.h). */
#include "runtime_avatars.h"
#include "runtime_http.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

const char *runtime_np_get_online_name(void);

#define MAX_KNOWN 256     /* names asked for this session */
#define MAX_READY 32      /* pictures waiting for the menu */
#define MAX_PNG (256 << 10)
#define RETRY_SECONDS 60  /* the server could not be reached, or Discord did not answer */

enum { WANTED, ASKING, DONE, RETRY };

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wake = PTHREAD_COND_INITIALIZER;
static int s_started;
static struct { char name[RA_NAME]; int state; time_t retry; } s_known[MAX_KNOWN];
static int s_known_count;
static struct { char name[RA_NAME]; unsigned char *png; size_t size; } s_ready[MAX_READY];
static int s_ready_count;

/* Nicknames are 3-16 of A-Z a-z 0-9 _ -, kept in lower case (safe in a URL). */
static int clean_lower(char *dst, const char *src) {
    size_t n = 0;
    for (; src && *src && n < RA_NAME - 1; src++) {
        char c = *src;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') dst[n++] = c;
    }
    dst[n] = '\0';
    return n >= 2;
}

static void *avatar_thread(void *arg) {
    (void)arg;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    for (;;) {
        char who[RA_NAME] = "";
        pthread_mutex_lock(&s_lock);
        for (;;) {
            const time_t now = time(NULL);
            time_t soonest = 0;
            for (int i = 0; i < s_known_count && !who[0]; i++) {
                if (s_known[i].state == RETRY && now >= s_known[i].retry) s_known[i].state = WANTED;
                if (s_known[i].state == RETRY && (!soonest || s_known[i].retry < soonest)) soonest = s_known[i].retry;
                if (s_known[i].state == WANTED && s_ready_count < MAX_READY) {
                    s_known[i].state = ASKING;
                    memcpy(who, s_known[i].name, RA_NAME);
                }
            }
            if (who[0]) break;
            if (soonest) {
                struct timespec until = {soonest, 0};
                pthread_cond_timedwait(&s_wake, &s_lock, &until);
            } else {
                pthread_cond_wait(&s_wake, &s_lock);
            }
        }
        pthread_mutex_unlock(&s_lock);

        char me[RA_NAME], target[96];
        int status = 0;
        char *reply = NULL;
        size_t size = 0;
        if (clean_lower(me, runtime_np_get_online_name())) {
            snprintf(target, sizeof target, "/ryunet/avatar?name=%s&who=%s", me, who);
            status = runtime_ryunet_transfer("GET", target, NULL, 0, &reply, &size, MAX_PNG);
        }
        const int picture = status == 200 && reply && size >= 8 && !memcmp(reply, "\x89PNG", 4);
        /* 404 unknown player, 403 not signed, 200 that is no picture (a server without this route): nothing
         * to show this session. 0 (unreachable) or 5xx (Discord did not answer): try again later. */
        const int final = status == 200 || (status >= 400 && status < 500);

        pthread_mutex_lock(&s_lock);
        for (int i = 0; i < s_known_count; i++) {
            if (strcmp(s_known[i].name, who)) continue;
            s_known[i].state = final ? DONE : RETRY;
            s_known[i].retry = time(NULL) + RETRY_SECONDS;
        }
        if (final && s_ready_count < MAX_READY) {
            memcpy(s_ready[s_ready_count].name, who, RA_NAME);
            s_ready[s_ready_count].png = picture ? (unsigned char *)reply : NULL;
            s_ready[s_ready_count].size = picture ? size : 0;
            s_ready_count++;
            if (picture) reply = NULL;
        }
        pthread_mutex_unlock(&s_lock);
        free(reply);
    }
    return NULL;
}

void runtime_avatar_want(const char *who) {
    char name[RA_NAME];
    if (!clean_lower(name, who)) return;
    pthread_mutex_lock(&s_lock);
    int known = 0;
    for (int i = 0; i < s_known_count && !known; i++) known = !strcmp(s_known[i].name, name);
    if (!known && s_known_count < MAX_KNOWN) {
        memcpy(s_known[s_known_count].name, name, RA_NAME);
        s_known[s_known_count].state = WANTED;
        s_known_count++;
        if (!s_started) {
            pthread_t thread;
            s_started = pthread_create(&thread, NULL, avatar_thread, NULL) == 0;
            if (s_started) pthread_detach(thread);
        }
        pthread_cond_signal(&s_wake);
    }
    pthread_mutex_unlock(&s_lock);
}

int runtime_avatar_take(char who[RA_NAME], unsigned char **png, size_t *size) {
    pthread_mutex_lock(&s_lock);
    const int any = s_ready_count > 0;
    if (any) {
        memcpy(who, s_ready[0].name, RA_NAME);
        *png = s_ready[0].png;
        *size = s_ready[0].size;
        memmove(s_ready, s_ready + 1, sizeof s_ready[0] * (size_t)--s_ready_count);
        pthread_cond_signal(&s_wake); /* room for the next one */
    }
    pthread_mutex_unlock(&s_lock);
    return any;
}

void runtime_avatar_free(unsigned char *png) { free(png); }
