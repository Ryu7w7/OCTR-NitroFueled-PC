/* Loading-screen tips from RyuNET (v0.8.6): the administrator writes extra tips in the admin page
 * (server loading_tips.go) and every port shows them among the game's own, without a new release.
 *
 * How the game picks one (USA and EU 1.21, read 2026-11-12): CGuiBehaviorOctaneLoadingScreenHint's
 * handler (0x9d9470) takes a random CGuiOctaneLoadingScreenHint of GuiSystemData's list (+0x290, or the
 * race mode's own list from +0x428) and writes its two texts through the game's text setter 0x9d9910
 * (text, placeable): the title (+0x10) at 0x9d95a3, then the tip (+0x18) at 0x9d95b4. Those two calls
 * come here: one loading screen in `every` (the server's number) gets one of the server's tips instead,
 * in the game's language (BB_LANGUAGE, the PS4 language id, sent as ?lang=en/es/fr...: the server
 * answers in it, else in English, else in Spanish).
 *
 * GET /ryunet/tips?lang=xx: "ok\nevery=3\n" then one line per tip, "title\ttext\ttitle\ttext" (the
 * same twice: servers before the translations sent Spanish then English, picked here by language;
 * empty fields allowed). Asked when the game starts online and every 5 minutes. BB_TIPS=0: off. */
#include "runtime_replay_internal.h"
#include "runtime_http.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define TIPS_MAX 64
#define TIP_TEXT 600
#define TIPS_BODY (64 * 1024)

typedef void ABI set_text_fn(const char *text, void *placeable);
static set_text_fn *set_text;

typedef struct { char title[2][160], text[2][TIP_TEXT]; } Tip;
static struct {
    pthread_mutex_t lock;
    Tip tips[TIPS_MAX];
    int count, every, asked, spanish;
    unsigned seed;
    int pending;                /* the title went: the tip's text goes next */
    char pending_text[TIP_TEXT];
    int shown;
} T = {.lock = PTHREAD_MUTEX_INITIALIZER, .every = 3, .spanish = -1};

int runtime_net_is_online(void);               /* runtime_net.h (winsock headers) */

static int spanish(void) {
    if (T.spanish < 0) {
        const char *v = getenv("BB_LANGUAGE");
        const int id = v ? atoi(v) : 1;
        T.spanish = id == 3 || id == 20;
    }
    return T.spanish;
}

/* field `n` (0-based) of a tab-separated line, copied into out */
static void field(const char *line, int n, char *out, size_t size) {
    const char *p = line;
    for (int i = 0; i < n && p; i++) {
        p = strchr(p, '\t');
        if (p) p++;
    }
    size_t len = 0;
    if (p) {
        const char *end = strchr(p, '\t');
        len = end ? (size_t)(end - p) : strlen(p);
    }
    if (len >= size) len = size - 1;
    if (p) memcpy(out, p, len);
    out[len] = 0;
}

static void parse(const char *body) {
    if (strncmp(body, "ok\n", 3)) return;
    Tip *tips = calloc(TIPS_MAX, sizeof *tips);
    if (!tips) return;
    int count = 0, every = 3;
    const char *line = body + 3;
    while (*line && count < TIPS_MAX) {
        const char *end = strchr(line, '\n');
        const size_t len = end ? (size_t)(end - line) : strlen(line);
        char buf[2 * (160 + TIP_TEXT) + 8];
        const size_t n = len < sizeof buf - 1 ? len : sizeof buf - 1;
        memcpy(buf, line, n);
        buf[n] = 0;
        if (!strncmp(buf, "every=", 6)) {
            every = atoi(buf + 6);
        } else if (n) {
            Tip *t = &tips[count];
            field(buf, 0, t->title[0], sizeof t->title[0]);
            field(buf, 1, t->text[0], sizeof t->text[0]);
            field(buf, 2, t->title[1], sizeof t->title[1]);
            field(buf, 3, t->text[1], sizeof t->text[1]);
            if (t->text[0][0] || t->text[1][0]) count++;
        }
        if (!end) break;
        line = end + 1;
    }
    pthread_mutex_lock(&T.lock);
    const int changed = count != T.count;
    memcpy(T.tips, tips, sizeof T.tips);
    T.count = count, T.every = every < 1 ? 1 : every > 20 ? 20 : every;
    pthread_mutex_unlock(&T.lock);
    if (changed) {
        printf("Loading tips: %d from RyuNET, one loading screen in %d\n", count, T.every);
        fflush(stdout);
    }
    free(tips);
}

/* the game's language as a code for the server (PS4 system language ids, BB_LANGUAGE) */
static const char *language_code(void) {
    static const char *const CODES[] = {
        "ja", "en", "fr", "es", "de", "it", "nl", "pt", "ru", "ko", "zh", "zh",
        "fi", "sv", "da", "no", "pl", "pt", "en", "tr", "es", "ar", "fr"};
    const char *v = getenv("BB_LANGUAGE");
    const int id = v ? atoi(v) : 1;
    return id >= 0 && id < (int)(sizeof CODES / sizeof *CODES) ? CODES[id] : "en";
}

static void *ask(void *unused) {
    (void)unused;
    char *body = malloc(TIPS_BODY), target[64];
    /* the server answers in this language (else English, else Spanish), in both columns */
    snprintf(target, sizeof target, "/ryunet/tips?lang=%s", language_code());
    for (;;) {
        if (body && runtime_ryunet_request("GET", target, body, TIPS_BODY) == 200) parse(body);
#ifdef _WIN32
        Sleep(300000);
#else
        sleep(300);
#endif
    }
    return NULL;
}

static unsigned next_random(void) {
    T.seed = T.seed * 1103515245u + 12345u;
    return T.seed >> 16;
}

/* the hint's title: the game's, or now and then a server tip's (whose text then goes next) */
static void ABI tip_title(const char *text, void *placeable) {
    pthread_mutex_lock(&T.lock);
    T.pending = 0;
    if (T.count && next_random() % (unsigned)T.every == 0) {
        const Tip *tip = &T.tips[next_random() % (unsigned)T.count];
        const int want = spanish() ? 0 : 1, other = !want;
        const int lang = tip->text[want][0] ? want : other;
        static char title[160];
        snprintf(title, sizeof title, "%s", tip->title[lang][0] ? tip->title[lang] : "RyuNET");
        snprintf(T.pending_text, sizeof T.pending_text, "%s", tip->text[lang]);
        T.pending = 1, T.shown++;
        text = title;
    }
    pthread_mutex_unlock(&T.lock);
    set_text(text, placeable);
}

static void ABI tip_text(const char *text, void *placeable) {
    char mine[TIP_TEXT];
    pthread_mutex_lock(&T.lock);
    const int ours = T.pending;
    if (ours) memcpy(mine, T.pending_text, sizeof mine);
    T.pending = 0;
    pthread_mutex_unlock(&T.lock);
    set_text(ours ? mine : text, placeable);
}

/* every frame (runtime_replay_tick): the first ask once the game is online */
void runtime_tips_tick(void) {
    if (T.asked || !set_text || !runtime_net_is_online()) return;
    T.asked = 1;
    pthread_t thread;
    if (pthread_create(&thread, NULL, ask, NULL) == 0) pthread_detach(thread);
}

void runtime_tips_install(int eu) {
    (void)eu;   /* the same addresses in both editions */
    const char *off = getenv("BB_TIPS");
    if (off && *off == '0') return;
    unsigned char *setter = rr_game(0x9d9910), *title_at = rr_game(0x9d95a3), *text_at = rr_game(0x9d95b4);
    T.seed = (unsigned)time(NULL);
    /* both calls or neither: a title of ours with the game's text would make no sense */
    int found = 1;
    for (unsigned char *at = title_at; at; at = at == title_at ? text_at : 0) {
        int32_t d;
        memcpy(&d, at + 1, 4);
        found &= at[0] == 0xe8 && at + 5 + d == setter;
    }
    set_text = (set_text_fn *)setter;
    const int hooked = found && rr_redirect_call(title_at, setter, (void (*)(void))tip_title) &&
                       rr_redirect_call(text_at, setter, (void (*)(void))tip_text);
    if (!hooked) set_text = 0;
    printf("Loading tips: %s\n", hooked ? "the loading screen's hint can come from RyuNET" : "hint calls not found");
}
