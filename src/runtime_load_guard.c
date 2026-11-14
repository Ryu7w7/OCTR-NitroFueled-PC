/* Load guard (OCTR v0.8.5): a loading screen that can never end restarts the game instead.
 *
 * When a level's chunk cannot load (a file the package requires is missing or cannot be opened),
 * the engine prints "ERROR: [Streaming] Failed to load chunk '<chunk>'" and the level never
 * finishes loading: the player waits on the loading screen forever (v0.8.4: the Online lobby, in
 * game folders whose paths passed the Windows limit). The game's own lines reach the host through
 * its writes to stdout/stderr (runtime_file.c do_write), and this watches them:
 *   "LOADING LEVEL: '<level>'"        a level starts loading
 *   "level finished loading"          it ended (anything armed is cleared)
 *   "Failed to load chunk '<chunk>'"  while a level loads, arms the guard
 * Armed for LOAD_GUARD_SECONDS without the level finishing, the guard says why in the log and
 * restarts the game (runtime_restart: run.py starts it again, at the title screen). A chunk that
 * fails outside a level load (a mod racer's archive in a lobby) changes nothing, and neither does
 * one whose level still finishes. BB_LOAD_GUARD=0 turns it off; BB_LOAD_GUARD=<seconds> sets the wait. */
#include "runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOAD_GUARD_SECONDS 45

static pthread_mutex_t guard_lock = PTHREAD_MUTEX_INITIALIZER;
static int guard_loading;          /* a level is loading */
static time_t guard_failed_at;     /* a chunk failed during this load (0: none) */
static char guard_level[128], guard_chunk[128], guard_file[256];
static int guard_seconds = -1;
static int guard_started;

static const char *find(const char *text, size_t size, const char *needle) {
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= size; ++i)
        if (text[i] == needle[0] && !memcmp(text + i, needle, n)) return text + i;
    return NULL;
}

/* The text after needle up to the closing quote (or the end of the line), into out. */
static void quoted_after(const char *text, size_t size, const char *needle, char close, char *out, size_t out_size) {
    const char *at = find(text, size, needle);
    if (!at) return;
    at += strlen(needle);
    size_t n = 0;
    while (at + n < text + size && at[n] != close && at[n] != '\n' && n + 1 < out_size) ++n;
    memcpy(out, at, n);
    out[n] = 0;
}

static void *guard_thread(void *unused) {
    (void)unused;
    for (;;) {
        sleep(1);
        pthread_mutex_lock(&guard_lock);
        int expired = guard_loading && guard_failed_at && time(NULL) - guard_failed_at >= guard_seconds;
        char level[128], chunk[128], file[256];
        snprintf(level, sizeof(level), "%s", guard_level);
        snprintf(chunk, sizeof(chunk), "%s", guard_chunk);
        snprintf(file, sizeof(file), "%s", guard_file);
        if (expired) guard_failed_at = 0;
        pthread_mutex_unlock(&guard_lock);
        if (!expired) continue;
        printf("Load guard: '%s' cannot finish loading: chunk '%s' failed%s%s%s. "
               "Restarting the game instead of waiting on the loading screen.\n",
               level, chunk, *file ? " (it needs \"" : "", file, *file ? "\")" : "");
        fflush(NULL);
        const char *code = getenv("BB_RESTART_EXIT_CODE"), *command = getenv("BB_RESTART_COMMAND");
        if ((code && *code) || (command && *command)) runtime_restart();
        puts("Load guard: no restart is set up for this launch (run.py sets it); the game stays as it is");
        fflush(stdout);
    }
    return NULL;
}

static void note_line(const char *text, size_t size) {
    /* Quick test: every line this cares about has "oad" ("LOAD", "load") in it. */
    if (!find(text, size, "OAD") && !find(text, size, "oad")) return;
    int loading = find(text, size, "LOADING LEVEL: '") != NULL;
    int finished = find(text, size, "level finished loading") != NULL;
    int failed = find(text, size, "Failed to load chunk '") != NULL;
    int required = find(text, size, "Required file \"") != NULL;
    if (!loading && !finished && !failed && !required) return;
    pthread_mutex_lock(&guard_lock);
    if (loading) {
        guard_loading = 1;
        guard_failed_at = 0;
        guard_chunk[0] = guard_file[0] = 0;
        quoted_after(text, size, "LOADING LEVEL: '", '\'', guard_level, sizeof(guard_level));
        const char *trace = getenv("BB_LOAD_GUARD_TRACE");
        if (trace && *trace == '1') printf("Load guard: watching the load of '%s'\n", guard_level);
    }
    if (finished) {
        guard_loading = 0;
        guard_failed_at = 0;
    }
    int armed = 0;
    if (failed && guard_loading) {
        if (!guard_failed_at) { guard_failed_at = time(NULL); armed = 1; }
        if (!guard_chunk[0]) quoted_after(text, size, "Failed to load chunk '", '\'', guard_chunk, sizeof(guard_chunk));
    }
    if (required && guard_loading && !guard_file[0])
        quoted_after(text, size, "Required file \"", '"', guard_file, sizeof(guard_file));
    int start = armed && !guard_started;
    if (start) guard_started = 1;
    char level[128], chunk[128];
    snprintf(level, sizeof(level), "%s", guard_level);
    snprintf(chunk, sizeof(chunk), "%s", guard_chunk);
    pthread_mutex_unlock(&guard_lock);
    if (start) {
        pthread_t thread;
        if (!pthread_create(&thread, NULL, guard_thread, NULL)) pthread_detach(thread);
    }
    if (armed)
        printf("Load guard: chunk '%s' failed while '%s' loads; the game restarts if it has not "
               "finished in %d s\n", chunk, level, guard_seconds);
}

/* The game's writes, whole lines or pieces of one: each line is looked at once it is complete. */
static pthread_mutex_t line_lock = PTHREAD_MUTEX_INITIALIZER;
static char line_text[512];
static size_t line_size;

void runtime_load_guard_note(const void *buffer, size_t size) {
    const char *text = buffer;
    if (!text || !size) return;
    if (guard_seconds < 0) {
        const char *setting = getenv("BB_LOAD_GUARD");
        guard_seconds = setting && *setting ? atoi(setting) : LOAD_GUARD_SECONDS;
        if (setting && *setting == '0') guard_seconds = 0;
    }
    if (!guard_seconds) return;
    pthread_mutex_lock(&line_lock);
    for (size_t i = 0; i < size; ++i) {
        if (text[i] == '\n') {
            note_line(line_text, line_size);
            line_size = 0;
        } else if (line_size < sizeof(line_text)) {
            line_text[line_size++] = text[i];
        }
    }
    pthread_mutex_unlock(&line_lock);
}
