/* Public Itemless: authenticated search extension and lobby confirmation. */
#define _GNU_SOURCE
#ifndef SACREDFIRE_RELEASE
#define SACREDFIRE_RELEASE "unversioned"
#endif
#include "runtime.h"
#include "runtime_http.h"
#include "runtime_np_online.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <ctype.h>
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#include "compat_win.h"
#include <windows.h>
#else
#include <sys/random.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

static pthread_mutex_t mode_lock = PTHREAD_MUTEX_INITIALIZER;
static int installed, selected, ranked_selected, searching, allow_party;
static int confirmed_itemless, confirmed_ranked;
static uint64_t request_nonce, registration_nonce, confirmed_room;
static unsigned char *room_owner;
static uint64_t matchmaking_room;
static unsigned char *matchmaking_owner;
static int matchmaking_search;

typedef void *(ABI *Search)(void **, void *, const char *, unsigned, void *);
typedef void *(ABI *Register)(void **, void *, const char *, unsigned);
typedef void *(ABI *Cancel)(void **, void *, uint64_t, void *);
typedef void *(ABI *Leave)(void **, void *, uint64_t);
typedef void (ABI *Join)(void *, uint64_t, uint64_t, const char *, unsigned, const char *, unsigned);
typedef void (ABI *Create)(void *, uint64_t, uint64_t, uint64_t, const char *, unsigned, const char *, unsigned);
typedef void (ABI *Notice)(const char *, void *);
static Search native_search;
static Register native_register;
static Cancel native_cancel;
static Leave native_leave;
static Join native_join, native_update;
static Create native_create, native_merge;
static Notice native_notice;
static void (ABI *native_destroy)(void *);
static void (ABI *native_disbanded)(void *, uint64_t);
static void (ABI *native_kicked)(void *, uint64_t);
static char rejection[512];
static void **network_manager;
static void **participants;
static void **party_manager;
static void (ABI *native_placeable_event)(void *, void *);
static void (ABI *native_stop_matchmaking)(void *);
static void (ABI *native_return_to_lobby)(void);
static uint64_t session_generation;
static void (ABI *native_make_handle)(void **, void *);
static void (ABI *native_resolve_handle)(void **);
static void (ABI *native_destroy_handle)(void **);

static int itemless_client_available(void) {
    const char *assets = getenv("BB_PRIVATE_ARCHIVE_PATCHES");
    return installed && assets && *assets;
}

int runtime_public_itemless_available(void) {
    const char *v = getenv("BB_PUBLIC_ITEMLESS");
    return itemless_client_available() && v && !strcmp(v, "1");
}

static void reset_locked(void) {
    __atomic_add_fetch(&session_generation, 1, __ATOMIC_SEQ_CST);
    matchmaking_room = 0;
    matchmaking_owner = NULL;
    matchmaking_search = 0;
    confirmed_room = 0;
    confirmed_itemless = confirmed_ranked = 0;
    room_owner = NULL;
    request_nonce = 0;
    searching = 0;
    allow_party = 0;
}
void runtime_public_itemless_reset(void) {
    pthread_mutex_lock(&mode_lock);
    reset_locked();
    pthread_mutex_unlock(&mode_lock);
}
void runtime_public_itemless_party_ready(void) {
    pthread_mutex_lock(&mode_lock);
    allow_party = registration_nonce != 0;
    pthread_mutex_unlock(&mode_lock);
}
void runtime_public_itemless_select(int enabled) {
    runtime_public_itemless_reset();
    selected = !!enabled;
}
int runtime_public_itemless_selected(void) { return selected; }
int runtime_ranked_selected(void) { return ranked_selected; }
/* CPartyManager owns the private session across scene changes. The lobby
 * scene's COctaneLobby::_isPrivateGame disappears when that scene unloads. */
int runtime_private_match_active(void) {
    const unsigned char *party = party_manager ? *party_manager : NULL;
    return installed && party && party[0x6c] != 0;
}

int runtime_public_itemless_active(void) {
    pthread_mutex_lock(&mode_lock);
    uint64_t native_room = 0;
    if (room_owner) memcpy(&native_room, room_owner + 0x958, 8);
    const int active = confirmed_itemless && confirmed_room && native_room == confirmed_room;
    pthread_mutex_unlock(&mode_lock);
    return active;
}

int runtime_ranked_active(void) {
    pthread_mutex_lock(&mode_lock);
    uint64_t native_room = 0;
    if (room_owner) memcpy(&native_room, room_owner + 0x958, 8);
    const int active = confirmed_ranked && confirmed_room && native_room == confirmed_room;
    pthread_mutex_unlock(&mode_lock);
    return active;
}

/* Public Normal also overrides a party's lingering private-scene flag. */
int runtime_public_match_active(void) {
    pthread_mutex_lock(&mode_lock);
    uint64_t room = 0;
    if (matchmaking_owner) memcpy(&room, matchmaking_owner + 0x958, 8);
    const int active = matchmaking_search || (matchmaking_room && room == matchmaking_room);
    pthread_mutex_unlock(&mode_lock);
    return active;
}

static void notice(const char *message) {
    fprintf(stderr, "Public ITEMLESS: %s\n", message);
    /* Use the Online/offline error helper: one OK button and native modal input. */
    if (native_notice) native_notice(message, NULL);
}

static void reject_search(const char *message) {
    pthread_mutex_lock(&mode_lock);
    reset_locked();
    snprintf(rejection, sizeof rejection, "%s", message);
    pthread_mutex_unlock(&mode_lock);
}

void runtime_public_itemless_update(void) {
    char message[sizeof rejection];
    pthread_mutex_lock(&mode_lock);
    memcpy(message, rejection, sizeof message);
    rejection[0] = 0;
    pthread_mutex_unlock(&mode_lock);
    if (!message[0]) return;
    /* Unwind the native search task before cancelling it and notifying party peers. */
    if (network_manager && *network_manager) {
        native_stop_matchmaking(*network_manager);
        native_return_to_lobby();
    }
    notice(message);
}

static uint64_t nonce(void) {
    uint64_t value = 0;
#ifdef _WIN32
    if (compat_random(&value, sizeof value)) return 0;
#else
    if (getrandom(&value, sizeof value, 0) != sizeof value) return 0;
#endif
    return value;
}

int runtime_public_itemless_announce(void) {
    if (!itemless_client_available()) return 0;
    const char *name = runtime_np_get_online_name();
    if (!name || !*name) return 0;
    pthread_mutex_lock(&mode_lock);
    if (!registration_nonce) registration_nonce = nonce();
    uint64_t token = registration_nonce;
    pthread_mutex_unlock(&mode_lock);
    if (!token) return 0;
    char target[512], body[128];
    snprintf(target, sizeof target, "/ryunet/itemless-client?name=%s&version=1&nonce=%016" PRIx64 "&cup=1&teams=1&difficulty=1&ranked=1&release=%s", name, token, SACREDFIRE_RELEASE);
    int status = runtime_ryunet_request("POST", target, body, sizeof body);
    int accepted = status == 200 && !strcmp(body, "ok");
    pthread_mutex_lock(&mode_lock);
    if (accepted && registration_nonce == token) allow_party = 1;
    pthread_mutex_unlock(&mode_lock);
    return accepted;
}

/* Find an unescaped top-level JSON field. Skip nested and escaped strings; reject duplicates. */
static const char *field(const char *json, const char *key) {
    if (!json || *json != '{') return NULL;
    int depth = 0;
    const char *found = NULL;
    for (const char *p = json; *p; ++p) {
        if (*p == '"') {
            const char *start = ++p;
            int escaped = 0;
            while (*p && *p != '"') {
                if (*p == '\\') { escaped = 1; if (!*++p) return NULL; }
                ++p;
            }
            if (!*p) return NULL;
            const char *after = p + 1;
            while (isspace((unsigned char)*after)) ++after;
            if (depth == 1 && !escaped && *after == ':' &&
                (size_t)(p - start) == strlen(key) && !memcmp(start, key, strlen(key))) {
                if (found) return NULL;
                found = after + 1;
                while (isspace((unsigned char)*found)) ++found;
            }
        } else if (*p == '{' || *p == '[') ++depth;
        else if (*p == '}' || *p == ']') { if (--depth < 0) return NULL; }
    }
    return depth ? NULL : found;
}

/* Only immutable request text crosses to the HTTP worker. Native roster/UI
 * access stays on the game thread; exact roster+feature keys prevent reuse. */
static pthread_mutex_t check_lock=PTHREAD_MUTEX_INITIALIZER;
static char check_key[512], check_body[512];
static int check_running, check_done, check_status;
static uint64_t check_until, check_generation;
struct CheckRequest { uint64_t generation; char key[512]; };
static uint64_t check_clock(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void *check_worker(void *arg) {
    struct CheckRequest *request=arg;
    char *key=request->key, body[512]="";
    int status=runtime_public_itemless_announce() ? runtime_ryunet_request("GET",key,body,sizeof body) : 0;
    pthread_mutex_lock(&check_lock);
    if (request->generation == __atomic_load_n(&session_generation, __ATOMIC_SEQ_CST) &&
        request->generation == check_generation && !strcmp(key,check_key)) {
        check_status=status; memcpy(check_body,body,sizeof body);
        check_done=1; check_until=check_clock()+5000;
    }
    check_running=0;
    pthread_mutex_unlock(&check_lock);
    free(request); return NULL;
}
/* -1 means pending: never wait for a network operation on the game thread. */
static int compatibility_check(const char *target,char *body,size_t size) {
    pthread_mutex_lock(&check_lock);
    uint64_t generation=__atomic_load_n(&session_generation, __ATOMIC_SEQ_CST);
    if (check_generation != generation || strcmp(check_key,target)) {
        check_generation=generation;
        snprintf(check_key,sizeof check_key,"%s",target); check_done=0;
    }
    int result=-1;
    if (check_done && check_clock()<check_until) {
        result=check_status; snprintf(body,size,"%s",check_body);
    } else if (!check_running) {
        struct CheckRequest *request=malloc(sizeof *request); pthread_t worker;
        if (request) { request->generation=generation; snprintf(request->key,sizeof request->key,"%s",target); }
        check_done=0; check_running=1;
        if (!request || pthread_create(&worker,NULL,check_worker,request)) {
            free(request); check_running=0; result=0;
        } else pthread_detach(worker);
    }
    pthread_mutex_unlock(&check_lock);
    return result;
}

static int ranked_party_check(const char *doc) {
    const char *members = field(doc, "members");
    char ids[192], target[512], body[512];
    size_t n = 0;
    if (!members || *members++ != '[') return 0;
    while (*members && *members != ']') {
        if (isdigit((unsigned char)*members) || *members == ',') {
            if (n + 1 >= sizeof ids) return 0;
            ids[n++] = *members;
        } else if (!isspace((unsigned char)*members)) return 0;
        ++members;
    }
    ids[n] = 0;
    const char *name = runtime_np_get_online_name();
    if (*members != ']' || !n || !name) return 0;
    snprintf(target, sizeof target, "/ryunet/ranked-party?name=%s&members=%s&mode=%d", name, ids, selected);
    int status = compatibility_check(target, body, sizeof body);
    if (status == -1) {
        /* The check just started: wait for it instead of bouncing the player out with a
         * press-again notice. A healthy server answers in milliseconds; the notice stays
         * as the timeout fallback. */
        uint64_t deadline = check_clock() + 5000;
        do {
            usleep(50000);
            status = compatibility_check(target, body, sizeof body);
        } while (status == -1 && check_clock() < deadline);
    }
    if (status == -1) return -1;
    if (status != 200 || strcmp(body, "ok")) {
        reject_search(status == 409 ? body : "Cannot verify Ranked server and party compatibility. Please retry.");
        return 0;
    }
    return 1;
}

/* Check before private Ready/Start can run its VSC side effects.
 * Read the same participant IDs used by the native manager. */
static _Thread_local const char *dispatch_approved_party;
static _Thread_local char checked_party[512];
static int early_party_check(const char *feature, int notify) {
    char ids[192] = "", target[512], body[512] = "";
    checked_party[0]=0;
    const char *online_name=runtime_np_get_online_name();
    void *manager = participants ? *participants : NULL;
    void *list = manager ? *(void **)((char *)manager + 0x10) : NULL;
    int count = list ? *(int *)((char *)list + 0xc) : 0;
    void **items = list ? *(void ***)((char *)list + 0x20) : NULL;
    uint64_t seen[8];
    int total = 0;
    size_t length = 0;
    if (!itemless_client_available() || !online_name || !*online_name || !items || count < 1 || count > 8)
        goto unavailable;
    /* Native search constructor (a5d286) takes the primary local account. */
    for (int i = 0; i < count; ++i) {
        if (!items[i]) goto unavailable;
        unsigned kind;
        memcpy(&kind, (char *)items[i] + 0x20, sizeof kind);
        if (kind > 7) continue;
        memcpy(&seen[total++], (char *)items[i] + 0xa8, sizeof seen[0]);
        break;
    }
    if (!total || !seen[0]) goto unavailable;
    /* a5d329 / a5d51e append remote party accounts, not local participants. */
    void *party = party_manager ? *party_manager : NULL;
    void *roster = party ? *(void **)((char *)party + 0x30) : NULL;
    int remote_count = roster ? *(int *)((char *)roster + 0xc) : -1;
    void **remote = roster ? *(void ***)((char *)roster + 0x20) : NULL;
    if (remote_count < 0 || remote_count > 8 || (remote_count && !remote)) goto unavailable;
    for (int i = 0; i < remote_count; ++i) {
        if (!remote[i]) goto unavailable;
        if (*((unsigned char *)remote[i] + 0x2c)) continue;
        uint64_t pid;
        memcpy(&pid, (char *)remote[i] + 0x18, sizeof pid);
        if (!pid || total == 8) goto unavailable;
        for (int j = 0; j < total; ++j) if (seen[j] == pid) goto unavailable;
        seen[total++] = pid;
    }
    for (int i = 0; i < total; ++i)
        length += (size_t)snprintf(ids + length, sizeof ids - length,
                                  "%s%" PRIu64, i ? "," : "", seen[i]);
    if (notify) fprintf(stderr, "ITEMLESS: pre-transition roster members=%s local=1 remote=%d\n", ids, total-1);
    if (feature)
        snprintf(target, sizeof target, "/ryunet/private-party?name=%s&feature=%s&release=%s&members=%s",
                 online_name, feature, SACREDFIRE_RELEASE, ids);
    else
        snprintf(target, sizeof target, "/ryunet/itemless-party?name=%s&members=%s",
                 online_name, ids);
    snprintf(checked_party, sizeof checked_party, "%s", target);
    if (dispatch_approved_party && !strcmp(dispatch_approved_party, target)) return 1;
    int status = compatibility_check(target, body, sizeof body);
    if (status == 200 && !strcmp(body, feature ? "ok-private-release:" SACREDFIRE_RELEASE : "ok")) {
        if (notify) fprintf(stderr, "ITEMLESS: pre-transition gate accepted %d party members\n", total);
        return 1;
    }
    if (status == -1) return -1;
    if (notify) notice(status == 409 ? body : "Cannot verify party compatibility. Please retry before starting this race.");
    return 0;
unavailable:
    if (notify) notice("Cannot verify party compatibility. Please retry before starting this race.");
    return 0;
}

static const char *private_feature(void) {
    const int difficulty = runtime_private_difficulty_modified();
    if (runtime_online_mode_private_cup()) return difficulty ? "cup,difficulty" : "cup";
    if (runtime_online_mode_private_teams()) return difficulty ? "teams,difficulty" : "teams";
    return difficulty ? "difficulty" : runtime_speed_mode_enabled() ? "itemless" : NULL;
}

/* Recheck at native Start as well: the roster can change in team selection. */
int runtime_private_party_check(void) {
    if (!runtime_online_mode_in_private()) return 1;
    const char *feature = private_feature();
    if (!feature && !runtime_speed_mode_enabled()) return 1;
    return early_party_check(feature,1);
}

/* Native handles track object lifetime; never retain a raw UI pointer across frames.
 * FocusInputPressed is 0x28 bytes: sender handle at +0x10, signal at +0x18. */
static struct PendingPrivate {
    void *target;
    struct {
        void *vtable;
        uint64_t flags;
        void *sender;
        unsigned signal;
        unsigned char tail[12];
    } event;
    void (ABI *resume)(void *);
    uint64_t generation, deadline;
    char party[512];
} pending_private;

static void release_handle(void **slot) {
    void *handle=*slot;
    if (!handle) return;
    unsigned before=__atomic_fetch_sub((unsigned *)handle,1,__ATOMIC_SEQ_CST);
    if ((before & 0xffff) == ((before & 0x90000000) ? 3u : 2u)) native_destroy_handle(slot);
    *slot=NULL;
}
static void *resolve_private_target(void **slot) {
    for (int depth=0; depth<16 && *slot; ++depth) {
        unsigned flags=*(unsigned *)*slot;
        if (flags & 0x10000000) native_resolve_handle(slot);
        if (!*slot) return NULL;
        flags=*(unsigned *)*slot;
        void *object=*(void **)((char *)*slot+0x28);
        if (!(flags & 0x20000000) || !object) return object;
        slot=(void **)((char *)object+0x10);
    }
    return NULL;
}
static void clear_private_pending(void) {
    struct PendingPrivate cancelled=pending_private;
    memset(&pending_private,0,sizeof pending_private);
    release_handle(&cancelled.target);
    release_handle(&cancelled.event.sender);
}
void runtime_private_party_cancel(void) { clear_private_pending(); }

static void defer_private(void *self, void *event, void (ABI *resume)(void *)) {
    if (pending_private.target) return; /* Repeated confirm must not restart the deadline. */
    if (!native_make_handle || !checked_party[0]) return;
    native_make_handle(&pending_private.target,self);
    if (!pending_private.target) {
        notice("Cannot prepare the compatibility check. Please try again.");
        return;
    }
    if (event) {
        memcpy(&pending_private.event,event,sizeof pending_private.event);
        /* The copied event has no registered object identity of its own. */
        pending_private.event.flags &= ~UINT64_C(0x7ffff);
        void *sender=pending_private.event.sender;
        if (sender) __atomic_fetch_add((unsigned *)sender,1,__ATOMIC_SEQ_CST);
    }
    pending_private.resume=resume;
    pending_private.generation=__atomic_load_n(&session_generation,__ATOMIC_SEQ_CST);
    pending_private.deadline=check_clock()+15000;
    snprintf(pending_private.party,sizeof pending_private.party,"%s",checked_party);
    fputs("Private compatibility: waiting before Ready/Start\n",stderr);
}
void runtime_private_party_defer_start(void *self, void (ABI *resume)(void *)) {
    defer_private(self,NULL,resume);
}

/* Returns 1 after dispatch: callers must not touch the old scene afterward. */
int runtime_private_party_update(void) {
    const char *feature=runtime_online_mode_in_private() ? private_feature() : NULL;
    if (!feature) { clear_private_pending(); return 0; }
    if (!pending_private.target) { (void)early_party_check(feature,0); return 0; }
    void *target=resolve_private_target(&pending_private.target);
    if (!target || pending_private.generation != __atomic_load_n(&session_generation,__ATOMIC_SEQ_CST)) {
        clear_private_pending(); return 0;
    }
    int status=early_party_check(feature,0);
    if (strcmp(pending_private.party,checked_party)) { clear_private_pending(); return 0; }
    if (status == -1 && check_clock()<pending_private.deadline) return 0;
    if (status != 1) {
        clear_private_pending();
        if (status == -1) notice("Player compatibility check timed out. Please try again.");
        else (void)early_party_check(feature,1);
        return 0;
    }
    struct PendingPrivate ready=pending_private;
    memset(&pending_private,0,sizeof pending_private);
    const char *previous=dispatch_approved_party;
    dispatch_approved_party=ready.party;
    if (ready.resume) ready.resume(target);
    else native_placeable_event(target,&ready.event);
    dispatch_approved_party=previous;
    release_handle(&ready.target);
    release_handle(&ready.event.sender);
    return 1;
}

static const char *meta_name(void *object) {
    if (!object) return NULL;
    typedef void *(ABI *GetType)(void *);
    GetType type = (*(GetType **)object)[0];
    void *meta = type(object);
    return meta ? *(const char **)((char *)meta + 0x10) : NULL;
}
/* The navigation item resolves the shared prefab's label placeable at +0xf0 ('Text_LevelListName',
 * the RNEX reference); the button's own tree has no children (the visuals live in the shared
 * comp instance). Set the label on it and on the focused variant inside the same comp. */
static void ranked_text_walk(void *node, int depth) {
    if (!node || depth > 8) return;
    const char *name = *(const char **)((char *)node + 0x28);
    if (name && (!strcmp(name, "[LevelListName]") ||
                 !strcmp(name, "LevelListName_focused")))
        runtime_online_mode_set_text(node, "RANKED");
    void *list = *(void **)((char *)node + 0x48);
    int count = list ? *(int *)((char *)list + 0xc) : 0;
    void **children = list ? *(void ***)((char *)list + 0x20) : NULL;
    for (int i = 0; children && i < count && i < 40; ++i)
        ranked_text_walk(children[i], depth + 1);
}
static void ranked_label(void *button) {
    void *list = *(void **)((char *)button + 0xe8);
    int count = list ? *(int *)((char *)list + 0xc) : 0;
    void **items = list ? *(void ***)((char *)list + 0x20) : NULL;
    for (int i = 0; items && i < count && i < 16; ++i) {
        const char *name = meta_name(items[i]);
        if (!name || strcmp(name, "CGuiBehaviorOctaneNavigationItem")) continue;
        void *text = *(void **)((char *)items[i] + 0xf0);
        if (!text) continue;
        ranked_text_walk(text, 0);
        ranked_text_walk(*(void **)((char *)text + 0x40), 0);
    }
}

/* BB_GUI_DUMP=1: print the whole placeable tree above the ranked button: children tables and
 * sibling chains from the topmost ancestor, with names, sprite textures and behavior graphs. */
static void *gui_seen[4096];
static int gui_seen_count;
static int gui_seen_add(void *node) {
    for (int i = 0; i < gui_seen_count; ++i)
        if (gui_seen[i] == node) return 0;
    if (gui_seen_count < (int)(sizeof gui_seen / sizeof *gui_seen))
        gui_seen[gui_seen_count++] = node;
    return 1;
}
static void gui_dump_node(void *node, int depth) {
    if (!node || depth > 12 || !gui_seen_add(node)) return;
    const char *type = meta_name(node);
    const char *name = *(const char **)((char *)node + 0x28);
    fprintf(stderr, "GUI%d %*s%s \"%s\" [node=%p]", depth, depth * 2, "", type ? type : "?",
            name ? name : "", node);
    if (type && strstr(type, "Sprite")) {
        const char *texture = *(const char **)((char *)node + 0x10);
        fprintf(stderr, " texture=\"%s\"", texture ? texture : "");
    } else if (type && strstr(type, "Text")) {
        const char *text = *(const char **)((char *)node + 0x10);
        fprintf(stderr, " text=\"%s\"", text ? text : "");
    } else if (type && strstr(type, "VscBehavior")) {
        void *graph = *(void **)((char *)node + 0x20);
        fprintf(stderr, " graph=\"%s\"", graph ? *(const char **)((char *)graph + 0x10) : "");
    }
    fprintf(stderr, " asset=%p\n", *(void **)((char *)node + 0x38));
    void *list = *(void **)((char *)node + 0x48);
    int count = list ? *(int *)((char *)list + 0xc) : 0;
    void **children = list ? *(void ***)((char *)list + 0x20) : NULL;
    for (int i = 0; children && i < count && i < 40; ++i)
        gui_dump_node(children[i], depth + 1);
    gui_dump_node(*(void **)((char *)node + 0x58), depth); /* the next sibling */
}
static void gui_dump(void *button) {
    void *node = button, *top = button;
    for (int depth = 0; node && depth < 16; ++depth) {
        void *parent = *(void **)((char *)node + 0x40);
        if (!parent) break;
        node = parent;
        top = node;
    }
    gui_seen_count = 0;
    fprintf(stderr, "==== GUI DUMP (ranked button %p, top ancestor %p) ====\n", button, top);
    gui_dump_node(top, 0);
    fprintf(stderr, "==== GUI DUMP end (%d nodes) ====\n", gui_seen_count);
}
/* The Ranked button shares the matchmaking button's VSC behavior object (one graph instance,
 * refcounted by both behavior lists), so its Confirm runs the graph with the native owner
 * exactly as when Matchmaking itself is pressed; the hook only flags the ranked mode. */
static void ABI placeable_event(void *self, void *event) {
    char approval[sizeof checked_party] = "";
    int private_room = runtime_online_mode_in_private();
    const char *feature = private_room ? private_feature() : NULL;
    int private_gate = private_room && (runtime_speed_mode_enabled() || feature);
    const char *tag = self ? *(const char **)((char *)self + 0x28) : NULL;
    int ranked_button = tag && !strcmp(tag, "OCTR_RANKED_BUTTON");
    if (self && event) {
        const char *type = meta_name(event);
        int signal = 0;
        if (type && !strcmp(type, "igGuiEventFocusInputPressed")) {
            memcpy(&signal, (char *)event + 0x18, sizeof signal);
            if (signal != 8) clear_private_pending();
            else if (pending_private.target && resolve_private_target(&pending_private.target)==self) return;
        }
        if (type && pending_private.target &&
            ((!strcmp(type,"igGuiEventPlaceableLoseFocus") && resolve_private_target(&pending_private.target)==self) ||
             !strcmp(type,"igGuiEventProjectLoseFocus"))) clear_private_pending();
        if (type && (!strcmp(type,"igGuiEventProjectInputPressed") ||
                     !strcmp(type,"igGuiEventFocusInputRepeated") ||
                     !strcmp(type,"igGuiEventProjectInputRepeated"))) {
            memcpy(&signal,(char *)event+0x18,sizeof signal);
            if (signal != 8) clear_private_pending();
        }
        /* Confirm (8) is the input filter in both native button graphs. */
        if (type && !strcmp(type, "igGuiEventFocusInputPressed") && signal == 8) {
            void *list = *(void **)((char *)self + 0xe8);
            int count = list ? *(int *)((char *)list + 0xc) : 0;
            void **items = list ? *(void ***)((char *)list + 0x20) : NULL;
            for (int i = 0; items && i < count; ++i) {
                /* VSC instances can report their dynamic graph type directly. */
                const char *name = meta_name(items[i]);
                if (name && !strcmp(name, "igGuiVscBehavior")) {
                    void *graph = *(void **)((char *)items[i] + 0x20);
                    name = graph ? *(const char **)((char *)graph + 0x10) : NULL;
                }
                if (name && !strcmp(name, "Scripts.Graph.Octane_Lobby_Main_MatchmakingButton"))
                    ranked_selected = ranked_button;
                if (private_gate && name && !strcmp(name, "Scripts.Graph.Octane_Lobby_PrivateGame_ButtonReady")) {
                    fprintf(stderr, "ITEMLESS: private ready/start gate reached before native transition\n");
                    int status=early_party_check(feature,1);
                    if (status == -1) { defer_private(self,event,NULL); return; }
                    if (status != 1) return;
                    memcpy(approval, checked_party, sizeof approval);
                    break;
                }
            }
        }
    }
    const char *previous = dispatch_approved_party;
    if (approval[0]) dispatch_approved_party = approval;
    native_placeable_event(self, event);
    /* The label override touches the shared comp instance through the nav's resolved text
     * reference: safe while the scene is live (focus and input events), but the LoseFocus
     * events also fire while the project is tearing down and the comp may already be gone. */
    if (ranked_button && event) {
        const char *event_type = meta_name(event);
        if (!event_type || (strcmp(event_type, "igGuiEventPlaceableLoseFocus") &&
                            strcmp(event_type, "igGuiEventProjectLoseFocus"))) {
            ranked_label(self);
            if (getenv("BB_GUI_DUMP")) gui_dump(self);
        }
    }
    dispatch_approved_party = previous;
}

static char *extend(const char *raw, unsigned limit, uint64_t token, int mode, int ranked) {
    if (!raw || *raw != '{' || !limit || limit > 65536 || !token) return NULL;
    size_t n = strnlen(raw, limit);
    if (n == limit || field(raw, "octr_itemless") || field(raw, "octr_ranked")) return NULL;
    char *out = malloc(n + 480);
    if (out) {
        size_t used = 0;
        if (ranked) used = (size_t)snprintf(out, n + 480,
            "{\"octr_ranked\":{\"version\":1,\"mode\":%d,\"nonce\":\"%016" PRIx64 "\"},",
            mode, token);
        else { out[0] = '{'; out[1] = 0; used = 1; }
        if (mode || !ranked) used += (size_t)snprintf(out + used, n + 480 - used,
            "\"octr_itemless\":{\"version\":1,\"mode\":%d,\"ranked\":1,\"private_cup\":1,\"private_teams\":1,\"private_difficulty\":1,\"release\":\"%s\",\"nonce\":\"%016" PRIx64 "\"},",
            mode, SACREDFIRE_RELEASE, token);
        snprintf(out + used, n + 480 - used, "%s", raw + 1);
    }
    return out;
}

static void *ABI register_client(void **result, void *self, const char *raw, unsigned limit) {
    runtime_public_itemless_reset();
    uint64_t token = itemless_client_available() ? nonce() : 0;
    char *doc = token ? extend(raw, limit, token, 0, 0) : NULL;
    pthread_mutex_lock(&mode_lock);
    registration_nonce = doc ? token : 0;
    allow_party = registration_nonce != 0;
    pthread_mutex_unlock(&mode_lock);
    fprintf(stderr, "Public ITEMLESS: registering compatibility=%d\n", doc != NULL);
    void *r = native_register(result, self, doc ? doc : raw, doc ? (unsigned)strlen(doc)+1 : limit);
    free(doc);
    return r;
}
static void *ABI start_search(void **result, void *self, const char *raw, unsigned limit, void *response) {
    if (rejection[0]) { *result = NULL; return result; }
    if (ranked_selected) {
        int ready = ranked_party_check(raw);
        if (ready != 1) {
            *result = NULL;
            if (ready == -1) notice("Checking Ranked compatibility. Please press Play again shortly.");
            return result;
        }
    }
    runtime_public_itemless_reset();
    fprintf(stderr, "Public matchmaking: starting %s %s search\n", ranked_selected ? "RANKED" : "CASUAL", selected ? "ITEMLESS" : "NORMAL");
    if (!selected && !ranked_selected) {
        pthread_mutex_lock(&mode_lock);
        matchmaking_search = 1;
        pthread_mutex_unlock(&mode_lock);
        void *r = native_search(result, self, raw, limit, response);
        if (!*result) runtime_public_itemless_reset();
        return r;
    }
    if (!itemless_client_available() || (selected && !runtime_public_itemless_available()) ||
        !raw || strnlen(raw, limit) == limit) {
        *result = NULL;
        if (!rejection[0]) reject_search("Cannot verify party compatibility. Please retry before matchmaking.");
        return result;
    }
    /* Public startSearch validates the full party on the server. Let its native
     * async task own waiting/errors; an HTTP preflight can expire mid-transition. */
    uint64_t token = nonce();
    char *doc = extend(raw, limit, token, selected, ranked_selected);
    if (!doc) { *result = NULL; reject_search("Cannot prepare matchmaking search."); return result; }
    pthread_mutex_lock(&mode_lock);
    request_nonce = token;
    searching = 1;
    matchmaking_search = 1;
    pthread_mutex_unlock(&mode_lock);
    void *r = native_search(result, self, doc, (unsigned)strlen(doc)+1, response);
    free(doc);
    if (!*result) runtime_public_itemless_reset();
    return r;
}
static void *ABI cancel_search(void **result, void *self, uint64_t id, void *response) {
    runtime_public_itemless_reset();
    return native_cancel(result, self, id, response);
}
static void *ABI leave_lobby(void **result, void *self, uint64_t id) {
    runtime_public_itemless_reset();
    return native_leave(result, self, id);
}

/* Validate tokens against the authoritative confirmation document, never the host document. */
static int confirmation_mode(const char *confirmation_doc, const char *key, uint64_t room, uint64_t token, int mode) {
    const char *value = field(confirmation_doc, key);
    char prefix[80], wanted[17];
    snprintf(prefix, sizeof prefix, "\"1/%" PRIu64 "/%d/", room, mode);
    snprintf(wanted, sizeof wanted, "%016" PRIx64, token);
    if (!value || !token || strncmp(value, prefix, strlen(prefix))) return 0;
    value += strlen(prefix);
    int match = 0, count = 0;
    while (*value && *value != '"') {
        if (strnlen(value, 16) < 16) return 0;
        for (int i=0; i<16; ++i) if (!isxdigit((unsigned char)value[i])) return 0;
        if (!memcmp(value, wanted, 16)) match = 1;
        value += 16;
        if (++count > 16) return 0;
        if (*value == '"') break;
        if (*value++ != ',') return 0;
    }
    return *value == '"' && match;
}
static int confirmation(const char *doc, uint64_t room, uint64_t token) {
    return confirmation_mode(doc, "octr_itemless_v1", room, token, 1);
}
static int accept(void *owner, uint64_t room, const char *confirmation_doc, unsigned length) {
    if (!confirmation_doc || length > 65536 || strnlen(confirmation_doc, (size_t)length + 1) != length) {
        fprintf(stderr, "Public ITEMLESS: invalid confirmation length=%u room=%" PRIu64 "\n", length, room);
        return 0;
    }
    pthread_mutex_lock(&mode_lock);
    uint64_t token = searching ? request_nonce : allow_party ? registration_nonce : 0;
    int wants_itemless = searching && selected;
    int wants_ranked = searching && ranked_selected;
    int itemless_advertised = field(confirmation_doc, "octr_itemless_v1") != NULL;
    int ranked_advertised = field(confirmation_doc, "octr_ranked_v1") != NULL;
    int itemless_valid = runtime_public_itemless_available() && confirmation(confirmation_doc, room, token);
    int ranked_valid = itemless_client_available() &&
        (confirmation_mode(confirmation_doc, "octr_ranked_v1", room, token, 0) ||
         confirmation_mode(confirmation_doc, "octr_ranked_v1", room, token, 1));
    int ranked_mode_ok = !wants_ranked || confirmation_mode(confirmation_doc, "octr_ranked_v1", room, token, selected);
    int valid = ((!itemless_advertised && !wants_itemless) || itemless_valid) &&
                ((!ranked_advertised && !wants_ranked) || (ranked_valid && ranked_mode_ok));
    if (valid && (itemless_advertised || ranked_advertised)) {
        confirmed_room = room; room_owner = owner;
        confirmed_itemless = itemless_advertised;
        confirmed_ranked = ranked_advertised;
    } else if (confirmed_room == room || (!itemless_advertised && !ranked_advertised)) {
        confirmed_room = 0; room_owner = NULL;
        confirmed_itemless = confirmed_ranked = 0;
    }
    if (valid) {
        matchmaking_room = room;
        matchmaking_owner = owner;
        matchmaking_search = 0;
    }
    pthread_mutex_unlock(&mode_lock);
    if (!valid) {
        reject_search("Matchmaking lobby confirmation failed. Please search again.");
        return 0;
    }
    return 1;
}
static void *ptr(const void *p, size_t offset) { void *v; memcpy(&v,(const char *)p+offset,8); return v; }
static int replacement(uint64_t room) {
    pthread_mutex_lock(&mode_lock);
    int different = matchmaking_room && matchmaking_room != room;
    pthread_mutex_unlock(&mode_lock);
    return different;
}
static void ABI joined(void *self, uint64_t search, uint64_t room, const char *host, unsigned hn, const char *confirmation_doc, unsigned bn) {
    if (replacement(room)) return;
    if (accept(self, room, confirmation_doc, bn)) native_join(self, search, room, host, hn, confirmation_doc, bn);
}
static void ABI created(void *self, uint64_t search, uint64_t room, uint64_t version, const char *host, unsigned hn, const char *confirmation_doc, unsigned bn) {
    if (replacement(room)) return;
    if (accept(self, room, confirmation_doc, bn)) native_create(self, search, room, version, host, hn, confirmation_doc, bn);
}
static void ABI updated(void *self, uint64_t room, uint64_t version, const char *host, unsigned hn, const char *confirmation_doc, unsigned bn) {
    uint64_t current = 0;
    memcpy(&current, (unsigned char *)self + 0x958, 8);
    if (current != room) return; /* A delayed update must not replace a different lobby. */
    if (accept(self, room, confirmation_doc, bn)) native_update(self, room, version, host, hn, confirmation_doc, bn);
}
static void ABI merged(void *self, uint64_t search, uint64_t from, uint64_t to, const char *host, unsigned hn, const char *confirmation_doc, unsigned bn) {
    uint64_t current = 0;
    memcpy(&current, (unsigned char *)self + 0x958, 8);
    if (current != from) return;
    if (accept(self, to, confirmation_doc, bn)) native_merge(self, search, from, to, host, hn, confirmation_doc, bn);
}

/* A native lobby's lifetime, not map transitions, owns its confirmation. */
static void retire(void *owner, uint64_t room) {
    pthread_mutex_lock(&mode_lock);
    if ((room_owner == owner && (!room || room == confirmed_room)) ||
        (matchmaking_owner == owner && (!room || room == matchmaking_room))) reset_locked();
    pthread_mutex_unlock(&mode_lock);
}
static void ABI destroyed(void *self) {
    retire(self, 0);
    native_destroy(self);
}
static void ABI disbanded(void *self, uint64_t room) {
    retire(self, room);
    native_disbanded(self, room);
}
static void ABI kicked(void *self, uint64_t room) {
    retire(self, room);
    native_kicked(self, room);
}

static void jump(unsigned char *at, const void *target) { memcpy(at,"\xff\x25\0\0\0\0",6); memcpy(at+6,&target,8); }
static int writable(void *p, size_t n) {
#ifdef _WIN32
    DWORD old; return VirtualProtect(p,n,PAGE_EXECUTE_READWRITE,&old)!=0;
#else
    size_t page=(size_t)sysconf(_SC_PAGESIZE); void *start=(void *)((uintptr_t)p & ~(page-1));
    return !mprotect(start,(unsigned char *)p+n-(unsigned char *)start,PROT_READ|PROT_WRITE|PROT_EXEC);
#endif
}
int runtime_public_itemless_start(unsigned char *image, size_t size) {
    const char *hook=getenv("BB_ONLINE_MODE_HOOK");
    if (!hook || !*hook) return 1;
    if (strcmp(hook,"1277e00") && strcmp(hook,"1277df0")) return 0;
    if (size<0x2af2608) return 0;
    const size_t sites[]={0xda300,0xd8fb0,0xda4f0,0xda7f0,0xa14550,0x24de10};
    const size_t stolen[]={17,17,17,17,14,17};
    const unsigned char destroy_prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x89,0xfb};
    const unsigned char prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec};
    const size_t slots[]={0x2549ea8,0x2549ee0,0x2549ee8,0x2549ec0,0x2549eb0,0x2549eb8};
    const size_t targets[]={0xa14890,0xa14910,0xa14e20,0xa148c0,0xa148a0,0xa148b0};
    const unsigned char ui_prefix[]={0x55,0x48,0x89,0xe5,0x53,0x50};
    if (memcmp(image+0xa97a00,ui_prefix,6) || memcmp(image+0xa9cbe0,ui_prefix,6)) return 0;
    if (memcmp(image+sites[4],destroy_prefix,sizeof destroy_prefix)) return 0;
    if (memcmp(image+sites[5],prefix,16) || image[sites[5]+16]!=0x48) return 0;
    if (memcmp(image+0xa548e0,prefix,16) || image[0xa548f0]!=0x38) return 0;
    for (int i=0;i<4;++i) {
        if (memcmp(image+sites[i],prefix,16) || image[sites[i]+16]!=(i==0?0x68:0x58)) return 0;
    }
    for (int i=0;i<6;++i) if (ptr(image,slots[i])!=image+targets[i]) return 0;
#ifdef _WIN32
    unsigned char *stubs=VirtualAlloc(NULL,192,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
#else
    unsigned char *stubs=mmap(NULL,192,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (stubs==MAP_FAILED) stubs=NULL;
#endif
    if (!stubs) return 0;
    for (int i=0;i<6;++i) if (!writable(image+sites[i],stolen[i])) return 0;
    for (int i=0;i<6;++i) if (!writable(image+slots[i],8)) return 0;
    void *handlers[]={(void *)start_search,(void *)register_client,(void *)cancel_search,(void *)leave_lobby,(void *)destroyed,(void *)placeable_event};
    void *events[]={(void *)joined,(void *)created,(void *)updated,(void *)merged,(void *)disbanded,(void *)kicked};
    native_destroy=(void (ABI *)(void *))(stubs+128);
    native_disbanded=(void (ABI *)(void *, uint64_t))(image+targets[4]);
    native_kicked=(void (ABI *)(void *, uint64_t))(image+targets[5]);
    native_search=(Search)stubs; native_register=(Register)(stubs+32);
    native_cancel=(Cancel)(stubs+64); native_leave=(Leave)(stubs+96);
    native_join=(Join)(image+targets[0]); native_create=(Create)(image+targets[1]);
    native_update=(Join)(image+targets[2]); native_merge=(Create)(image+targets[3]);
    network_manager=(void **)(image+0x2acb3d8);
    participants=(void **)(image+0x2ad1160);
    party_manager=(void **)(image+0x2acf700);
    native_placeable_event=(void (ABI *)(void *, void *))(stubs+160);
    native_stop_matchmaking=(void (ABI *)(void *))(image+0xa97a00);
    native_return_to_lobby=(void (ABI *)(void))(image+0xa9cbe0);
    native_notice=(Notice)(image+0xa548e0);
    for (int i=0;i<6;++i) {
        memcpy(stubs+i*32,image+sites[i],stolen[i]);
        jump(stubs+i*32+stolen[i],image+sites[i]+stolen[i]);
        jump(image+sites[i],handlers[i]);
    }
    for (int i=0;i<6;++i) memcpy(image+slots[i],&events[i],8);
    size_t shift=!strcmp(hook,"1277df0") ? 0x10 : 0;
    native_make_handle=(void (ABI *)(void **,void *))(image+0x14bea20-shift);
    native_resolve_handle=(void (ABI *)(void **))(image+0x149fca0-shift);
    native_destroy_handle=(void (ABI *)(void **))(image+0x149f280-shift);
    installed=1;
    puts("Public ITEMLESS: isolated matchmaking hooks installed (full-party gate, native OK notice)");
    return 1;
}
