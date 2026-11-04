/* Spectator build (experimental): friends watch each other's races.
 *
 * The Replay Mode mod (patches/ReplayMode.xml) saves every race it records in the save folder
 * savedata/1/<title>/RaceReplay: A<number>.crr (the last 50), K<number>.crr (kept). Any other *.crr there
 * is listed too, newest first, the next time the main menu's REPLAYS list opens.
 *
 * Sharing (bbport.ini spectate_share=1, in-game menu > Friends > Options): a race saved after the
 * game started is sent to the RyuNET server (POST /ryunet/spectate/upload), which keeps a player's
 * last few races for their friends.
 * Watching (Friends > Spectate): GET /ryunet/spectate/latest?host=<friend> downloads the friend's newest
 * race into RaceReplay as <friend>_<hhmmss>.crr. */
#include "runtime_spectate.h"
#include "runtime.h"
#include "runtime_http.h"
#include "runtime_np_online.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_RACE_BYTES (48u * 1024 * 1024)
#define SCAN_SECONDS 3

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static int started, share, state;
static unsigned serial;
static char watch_name[32], status_name[32];
static time_t since;                 /* races saved before this are not shared */
static char sent_name[64];           /* the newest race sent: its name and time */
static time_t sent_time;

static void race_dir(char *out, size_t size) {
    snprintf(out, size, "%s/savedata/1/%s/RaceReplay", runtime_file_progress_dir(), runtime_savedata_title());
}

static int valid_nick(const char *s) {
    size_t n = strlen(s);
    if (n < 3 || n > 16) return 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

/* A<digits>.crr or K<digits>.crr: a race this player recorded. */
static int own_race(const char *name) {
    size_t n = strlen(name);
    if (n < 6 || n > 40 || (name[0] != 'A' && name[0] != 'K') || strcmp(name + n - 4, ".crr")) return 0;
    for (size_t i = 1; i < n - 4; ++i)
        if (name[i] < '0' || name[i] > '9') return 0;
    return 1;
}

static char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *data = NULL;
    if (!fseek(f, 0, SEEK_END)) {
        const long n = ftell(f);
        if (n > 8 && (unsigned long)n <= MAX_RACE_BYTES && !fseek(f, 0, SEEK_SET) && (data = malloc((size_t)n))) {
            if (fread(data, 1, (size_t)n, f) != (size_t)n) {
                free(data);
                data = NULL;
            } else {
                *size = (size_t)n;
            }
        }
    }
    fclose(f);
    return data;
}

static void set_state(int value, const char *who) {
    pthread_mutex_lock(&lock);
    state = value;
    if (who) snprintf(status_name, sizeof status_name, "%s", who);
    ++serial;
    pthread_mutex_unlock(&lock);
}

/* The newest own race saved since the game started and at least 2 s ago (written whole), if it is
 * newer than the last one sent. */
static int newest_unsent(char *name, size_t size, time_t *when) {
    char dir[700];
    race_dir(dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return 0;
    const time_t now = time(NULL);
    int found = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!own_race(e->d_name)) continue;
        char path[800];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) || st.st_mtime < since || now - st.st_mtime < 2) continue;
        if (st.st_mtime < sent_time || (st.st_mtime == sent_time && !strcmp(e->d_name, sent_name))) continue;
        if (!found || st.st_mtime > *when) {
            snprintf(name, size, "%s", e->d_name);
            *when = st.st_mtime;
            found = 1;
        }
    }
    closedir(d);
    return found;
}

static void share_new_race(void) {
    const char *me = runtime_np_get_online_name();
    char name[64];
    time_t when = 0;
    if (!me || !valid_nick(me) || !newest_unsent(name, sizeof name, &when)) return;
    char dir[700], path[800], target[128];
    race_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t size = 0;
    char *data = read_file(path, &size);
    if (!data || memcmp(data, "CTRRPLAY", 8)) {
        free(data);
        sent_time = when; /* not a race file: skip it */
        snprintf(sent_name, sizeof sent_name, "%s", name);
        return;
    }
    snprintf(target, sizeof target, "/ryunet/spectate/upload?name=%s", me);
    char *reply = NULL;
    size_t reply_size = 0;
    const int status = runtime_ryunet_transfer("POST", target, data, size, &reply, &reply_size, 4096);
    free(data);
    free(reply);
    if (status == 200) {
        sent_time = when;
        snprintf(sent_name, sizeof sent_name, "%s", name);
        printf("Spectate: shared %s (%zu KB)\n", name, size / 1024);
    } else {
        printf("Spectate: sharing %s failed (HTTP %d)\n", name, status);
        sent_time = when; /* one try per race: a refused one is not sent again and again */
        snprintf(sent_name, sizeof sent_name, "%s", name);
    }
    fflush(stdout);
}

static void fetch(const char *who) {
    const char *me = runtime_np_get_online_name();
    if (!me || !valid_nick(me) || !valid_nick(who)) {
        set_state(RS_FAILED, who);
        return;
    }
    set_state(RS_FETCHING, who);
    char target[128];
    snprintf(target, sizeof target, "/ryunet/spectate/latest?name=%s&host=%s", me, who);
    char *data = NULL;
    size_t size = 0;
    const int status = runtime_ryunet_transfer("GET", target, NULL, 0, &data, &size, MAX_RACE_BYTES);
    if (status == 404) {
        free(data);
        set_state(RS_NONE, who);
        return;
    }
    if (status == 403) {
        free(data);
        set_state(RS_REFUSED, who);
        return;
    }
    if (status != 200 || !data || size < 8 || memcmp(data, "CTRRPLAY", 8)) {
        free(data);
        printf("Spectate: %s's race could not be downloaded (HTTP %d)\n", who, status);
        set_state(RS_FAILED, who);
        return;
    }
    char dir[700], path[800];
    race_dir(dir, sizeof dir);
    const time_t now = time(NULL);
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    snprintf(path, sizeof path, "%s/%s_%02d%02d%02d.crr", dir, who, t.tm_hour, t.tm_min, t.tm_sec);
    FILE *f = fopen(path, "wb");
    const int ok = f && fwrite(data, 1, size, f) == size;
    if (f) fclose(f);
    free(data);
    if (!ok) {
        printf("Spectate: could not write %s\n", path);
        set_state(RS_FAILED, who);
        return;
    }
    printf("Spectate: %s's race saved as %s (%zu KB)\n", who, path, size / 1024);
    fflush(stdout);
    set_state(RS_READY, who);
}

static void *worker(void *unused) {
    (void)unused;
    for (;;) {
        char who[32] = "";
        pthread_mutex_lock(&lock);
        if (!watch_name[0]) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_sec += SCAN_SECONDS;
            pthread_cond_timedwait(&wake, &lock, &until);
        }
        snprintf(who, sizeof who, "%s", watch_name);
        watch_name[0] = '\0';
        const int sharing = share;
        pthread_mutex_unlock(&lock);
        if (who[0]) fetch(who);
        if (sharing) share_new_race();
    }
    return NULL;
}

void runtime_spectate_start(void) {
    pthread_mutex_lock(&lock);
    const int first = !started;
    started = 1;
    if (first) since = time(NULL); /* sharing is set by the in-game menu (bbport.ini spectate_share) */
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
    state = RS_FETCHING;
    snprintf(status_name, sizeof status_name, "%s", friend_name);
    ++serial;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
}

unsigned runtime_spectate_status(int *out_state, char *friend_name, int size) {
    pthread_mutex_lock(&lock);
    if (out_state) *out_state = state;
    if (friend_name && size > 0) snprintf(friend_name, (size_t)size, "%s", status_name);
    const unsigned s = serial;
    pthread_mutex_unlock(&lock);
    return s;
}
