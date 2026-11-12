/* Time Trial runs of the leaderboards (OCTR v0.8.4): the RyuNET server keeps the run of each record in a
 * board's top places (its tt_runs.go), for watching them later.
 *
 * A Time Trial personal best is saved as a replay (runtime_replay.c, K<number>.crr). The server is asked first
 * whether it wants a run of that time (GET /ryunet/ttrun?name=&time=&want=1: the player has a record with that
 * time in a board's top places), and only then gets the file (POST, signed): a run that does not place costs
 * no upload. The record reaches the server when the race ends and the run is saved when the level ends, so a
 * run it does not want yet is asked about again a few times.
 *
 * The records set before: a minute after the game starts (online is up by then), the fastest finished Time
 * Trial of each track in RaceReplay goes the same way. Each file is asked about once: RaceReplay/ttrun-sent.txt
 * lists those the server took or did not want (a record only gets faster, so a run it did not want never
 * becomes a record of its own); one asked while the server could not be reached is asked again next time. */
#include "runtime_replay.h"
#include "runtime_replay_internal.h"
#include "runtime_replay_list.h"
#include "runtime_http.h"
#include "runtime_np_online.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define WANT_TRIES 6
#define WANT_WAIT_S 10
#define RUN_MAX (4u << 20)
#define BACKFILL_MAX 64

static void sleep_s(int s) {
#ifdef _WIN32
    Sleep((DWORD)s * 1000);
#else
    sleep((unsigned)s);
#endif
}

/* The local racer's finish time of a finished Time Trial replay, 0 when it is not one. */
static float run_time(const struct rr_header *h) {
    if (memcmp(h->magic, RR_MAGIC, 8) || h->mode != 2) return 0.0f;
    for (int k = 0; k < RR_RACERS; k++)
        if (h->racer[k].kind == RR_LOCAL && h->racer[k].finished && h->racer[k].finish_time > 0.0f) return h->racer[k].finish_time;
    return 0.0f;
}

static char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = n > 0 && (unsigned long)n <= RUN_MAX ? malloc((size_t)n) : NULL;
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) free(data), data = NULL;
    fclose(f);
    *size = data ? (size_t)n : 0;
    return data;
}

/* The server's answer for one run: 200 kept, 409 not wanted (now), 0 not reached, else another status. */
static int offer(const char *me, const char *path, float seconds) {
    char target[128];
    snprintf(target, sizeof target, "/ryunet/ttrun?name=%s&time=%.4f&want=1", me, seconds);
    char *reply = NULL;
    size_t size = 0;
    int status = runtime_ryunet_transfer("GET", target, NULL, 0, &reply, &size, 64);
    free(reply);
    if (status != 200) return status;
    char *data = read_file(path, &size);
    if (!data) return -1;
    snprintf(target, sizeof target, "/ryunet/ttrun?name=%s", me);
    status = runtime_ryunet_transfer("POST", target, data, size, &reply, &size, 256);
    free(reply);
    free(data);
    return status;
}

static const char *my_name(void) {
    const char *me = runtime_np_get_online_name();
    return getenv("BB_ONLINE_SERVER") && me && *me && strlen(me) < 40 ? me : NULL;
}

struct saved {
    char path[800];
    float seconds;
};

static void *saved_thread(void *arg) {
    struct saved *s = arg;
    const char *me = my_name();
    for (int tries = 0; me && tries < WANT_TRIES; tries++) {
        if (tries) sleep_s(WANT_WAIT_S);
        const int status = offer(me, s->path, s->seconds);
        if (status == 200) printf("Time Trial run: kept by the server for the leaderboard (%.3f s)\n", s->seconds);
        if (status != 409 && status != 0) break;
    }
    fflush(stdout);
    free(s);
    return NULL;
}

/* A Time Trial personal best was saved at path (runtime_replay.c). */
void runtime_ttrun_saved(const char *path) {
    struct rr_header h;
    FILE *f = fopen(path, "rb");
    const int ok = f && fread(&h, 1, sizeof h, f) == sizeof h;
    if (f) fclose(f);
    struct saved *s = ok && run_time(&h) > 0.0f && my_name() ? calloc(1, sizeof *s) : NULL;
    if (!s) return;
    snprintf(s->path, sizeof s->path, "%s", path);
    s->seconds = run_time(&h);
    pthread_t thread;
    if (pthread_create(&thread, NULL, saved_thread, s)) free(s);
    else pthread_detach(thread);
}

static void *backfill_thread(void *unused) {
    (void)unused;
    sleep_s(60);
    const char *me = my_name();
    if (!me) return NULL;
    char dir[700], list[800];
    rr_race_dir(dir, sizeof dir);
    snprintf(list, sizeof list, "%s/ttrun-sent.txt", dir);
    size_t sent_size = 0;
    char *sent_raw = read_file(list, &sent_size);
    char *sent = calloc(1, sent_size + 2);
    if (sent) {
        sent[0] = '\n';
        if (sent_raw) memcpy(sent + 1, sent_raw, sent_size);
    }
    free(sent_raw);
    struct best { char level[64], file[64]; float seconds; } *best = calloc(BACKFILL_MAX, sizeof *best);
    RrListEntry *entries = calloc(RR_LIST_MAX, sizeof *entries);
    int count = 0;
    const int n = entries && best && sent ? runtime_replay_list(entries, RR_LIST_MAX) : 0;
    for (int i = 0; i < n; i++) {
        if (entries[i].mode != 2) continue;
        char path[800], key[80];
        snprintf(key, sizeof key, "\n%s\n", entries[i].file);
        if (strstr(sent, key)) continue;
        snprintf(path, sizeof path, "%s/%s", dir, entries[i].file);
        struct rr_header h;
        FILE *r = fopen(path, "rb");
        const int ok = r && fread(&h, 1, sizeof h, r) == sizeof h;
        if (r) fclose(r);
        const float seconds = ok ? run_time(&h) : 0.0f;
        if (seconds <= 0.0f) continue;
        h.level[sizeof h.level - 1] = 0;
        int j = 0;
        while (j < count && strcmp(best[j].level, h.level)) j++;
        if (j == count) {
            if (count == BACKFILL_MAX) continue;
            count++;
        } else if (best[j].seconds <= seconds) {
            continue;
        }
        snprintf(best[j].level, sizeof best[j].level, "%s", h.level);
        snprintf(best[j].file, sizeof best[j].file, "%s", entries[i].file);
        best[j].seconds = seconds;
    }
    int kept = 0;
    for (int j = 0; j < count; j++) {
        char path[800];
        snprintf(path, sizeof path, "%s/%s", dir, best[j].file);
        const int status = offer(me, path, best[j].seconds);
        if (status != 200 && status != 409) break;   /* not reached, or a server without this: next time */
        kept += status == 200;
        FILE *w = fopen(list, "ab");
        if (w) {
            fprintf(w, "%s\n", best[j].file);
            fclose(w);
        }
    }
    if (count) printf("Time Trial runs: %d of this PC's best runs offered, %d kept for the leaderboards\n", count, kept);
    fflush(stdout);
    free(entries);
    free(best);
    free(sent);
    return NULL;
}

/* At startup (runtime_replay.c): the Time Trials recorded before. */
void runtime_ttrun_backfill(void) {
    static int started;
    if (started++) return;
    pthread_t thread;
    if (!pthread_create(&thread, NULL, backfill_thread, NULL)) pthread_detach(thread);
}
