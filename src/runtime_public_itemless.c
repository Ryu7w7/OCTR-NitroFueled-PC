/* Public Itemless: authenticated search extension and lobby confirmation. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_http.h"
#include "runtime_np_online.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <ctype.h>
#include <pthread.h>
#ifdef _WIN32
#include "compat_win.h"
#include <windows.h>
#else
#include <sys/random.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

static pthread_mutex_t mode_lock = PTHREAD_MUTEX_INITIALIZER;
static int installed, selected, searching, allow_party;
static uint64_t request_nonce, registration_nonce, confirmed_room;
static unsigned char *room_owner;

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

static int itemless_client_available(void) {
    const char *assets = getenv("BB_PRIVATE_ARCHIVE_PATCHES");
    return installed && assets && *assets;
}

int runtime_public_itemless_available(void) {
    const char *v = getenv("BB_PUBLIC_ITEMLESS");
    return itemless_client_available() && v && !strcmp(v, "1");
}

static void reset_locked(void) {
    confirmed_room = 0;
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
    const int active = confirmed_room && native_room == confirmed_room;
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
    char target[192], body[128];
    snprintf(target, sizeof target, "/ryunet/itemless-client?name=%s&version=1&nonce=%016" PRIx64 "&cup=1&teams=1&difficulty=1", name, token);
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

static int party_check(const char *doc) {
    const char *members = field(doc, "members");
    char ids[192], target[320], body[512];
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
    snprintf(target, sizeof target, "/ryunet/itemless-party?name=%s&members=%s", name, ids);
    int status = runtime_ryunet_request("GET", target, body, sizeof body);
    if (status == 200 && !strcmp(body, "ok")) return 1;
    reject_search(status == 409 ? body : "Cannot verify party compatibility. Please retry before starting ITEMLESS.");
    return 0;
}

/* Check before Go To Lobby or private Ready/Start can run their VSC side effects.
 * Read the same participant IDs used by the native manager. */
static int early_party_check(const char *feature) {
    char ids[192] = "", target[320], body[512] = "";
    void *manager = participants ? *participants : NULL;
    void *list = manager ? *(void **)((char *)manager + 0x10) : NULL;
    int count = list ? *(int *)((char *)list + 0xc) : 0;
    void **items = list ? *(void ***)((char *)list + 0x20) : NULL;
    uint64_t seen[8];
    int total = 0;
    size_t length = 0;
    if (!itemless_client_available() || !items || count < 1 || count > 8)
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
    fprintf(stderr, "ITEMLESS: pre-transition roster members=%s local=1 remote=%d\n", ids, total-1);
    if (!runtime_public_itemless_announce()) goto unavailable;
    if (feature)
        snprintf(target, sizeof target, "/ryunet/private-party?name=%s&feature=%s&members=%s",
                 runtime_np_get_online_name(), feature, ids);
    else
        snprintf(target, sizeof target, "/ryunet/itemless-party?name=%s&members=%s",
                 runtime_np_get_online_name(), ids);
    int status = runtime_ryunet_request("GET", target, body, sizeof body);
    if (status == 200 && !strcmp(body, feature ? "ok-private-v1" : "ok")) {
        fprintf(stderr, "ITEMLESS: pre-transition gate accepted %d party members\n", total);
        return 1;
    }
    notice(status == 409 ? body : "Cannot verify party compatibility. Please retry before starting this race.");
    return 0;
unavailable:
    notice("Cannot verify party compatibility. Please retry before starting this race.");
    return 0;
}

static const char *private_feature(void) {
    const int difficulty = runtime_private_difficulty_modified();
    if (runtime_online_mode_private_cup()) return difficulty ? "cup,difficulty" : "cup";
    if (runtime_online_mode_private_teams()) return difficulty ? "teams,difficulty" : "teams";
    return difficulty ? "difficulty" : NULL;
}

/* Recheck at native Start as well: the roster can change in team selection. */
int runtime_private_party_check(void) {
    if (!runtime_online_mode_in_private()) return 1;
    const char *feature = private_feature();
    return (!feature && !runtime_speed_mode_enabled()) || early_party_check(feature);
}

static const char *meta_name(void *object) {
    if (!object) return NULL;
    typedef void *(ABI *GetType)(void *);
    GetType type = (*(GetType **)object)[0];
    void *meta = type(object);
    return meta ? *(const char **)((char *)meta + 0x10) : NULL;
}
static void ABI placeable_event(void *self, void *event) {
    int private_room = runtime_online_mode_in_private();
    const char *feature = private_room ? private_feature() : NULL;
    int private_gate = private_room && (runtime_speed_mode_enabled() || feature);
    int public_itemless = !runtime_online_mode_in_private() && selected;
    if ((private_gate || public_itemless) && self && event) {
        const char *type = meta_name(event);
        int signal = 0;
        if (type && !strcmp(type, "igGuiEventFocusInputPressed"))
            memcpy(&signal, (char *)event + 0x18, sizeof signal);
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
                if (name && ((public_itemless && !strcmp(name, "Scripts.Graph.Octane_Lobby_ModeSelect_GoToLobbyButton")) ||
                             (private_gate && !strcmp(name, "Scripts.Graph.Octane_Lobby_PrivateGame_ButtonReady")))) {
                    fprintf(stderr, "ITEMLESS: %s gate reached before native transition\n",
                            private_gate ? "private ready/start" : "public Go To Lobby");
                    if (!early_party_check(feature)) return;
                    break;
                }
            }
        }
    }
    native_placeable_event(self, event);
}

static char *extend(const char *raw, unsigned limit, uint64_t token, int mode) {
    if (!raw || *raw != '{' || !limit || limit > 65536 || !token) return NULL;
    size_t n = strnlen(raw, limit);
    if (n == limit || field(raw, "octr_itemless")) return NULL;
    char *out = malloc(n + 192);
    if (out) snprintf(out, n + 192,
        "{\"octr_itemless\":{\"version\":1,\"mode\":%d,\"private_cup\":1,\"private_teams\":1,\"private_difficulty\":1,\"nonce\":\"%016" PRIx64 "\"},%s",
        mode, token, raw + 1);
    return out;
}

static void *ABI register_client(void **result, void *self, const char *raw, unsigned limit) {
    runtime_public_itemless_reset();
    uint64_t token = itemless_client_available() ? nonce() : 0;
    char *doc = token ? extend(raw, limit, token, 0) : NULL;
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
    runtime_public_itemless_reset();
    fprintf(stderr, "Public ITEMLESS: starting %s search\n", selected ? "ITEMLESS" : "NORMAL");
    if (!selected) return native_search(result, self, raw, limit, response);
    if (!runtime_public_itemless_available() || !raw || strnlen(raw, limit) == limit || !party_check(raw)) {
        *result = NULL;
        if (!rejection[0]) reject_search("Cannot verify party compatibility. Please retry before starting ITEMLESS.");
        return result;
    }
    uint64_t token = nonce();
    char *doc = extend(raw, limit, token, 1);
    if (!doc) { *result = NULL; reject_search("Cannot prepare ITEMLESS search."); return result; }
    pthread_mutex_lock(&mode_lock);
    request_nonce = token;
    searching = 1;
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
static int confirmation(const char *confirmation_doc, uint64_t room, uint64_t token) {
    const char *value = field(confirmation_doc, "octr_itemless_v1");
    char prefix[80], wanted[17];
    snprintf(prefix, sizeof prefix, "\"1/%" PRIu64 "/1/", room);
    snprintf(wanted, sizeof wanted, "%016" PRIx64, token);
    if (!value || !token || strncmp(value, prefix, strlen(prefix))) return 0;
    value += strlen(prefix);
    int match = 0, count = 0;
    while (*value && *value != '"') {
        for (int i=0; i<16; ++i) if (!isxdigit((unsigned char)value[i])) return 0;
        if (!memcmp(value, wanted, 16)) match = 1;
        value += 16;
        if (++count > 16) return 0;
        if (*value == '"') break;
        if (*value++ != ',') return 0;
    }
    return *value == '"' && match;
}
static int accept(void *owner, uint64_t room, const char *confirmation_doc, unsigned length) {
    if (!confirmation_doc || length > 65536 || strnlen(confirmation_doc, (size_t)length + 1) != length) {
        fprintf(stderr, "Public ITEMLESS: invalid confirmation length=%u room=%" PRIu64 "\n", length, room);
        return 0;
    }
    pthread_mutex_lock(&mode_lock);
    uint64_t token = searching ? request_nonce : allow_party ? registration_nonce : 0;
    int wants = searching;
    int valid = runtime_public_itemless_available() && confirmation(confirmation_doc, room, token);
    int advertised = field(confirmation_doc, "octr_itemless_v1") != NULL;
    if (valid && confirmed_room != room)
        fprintf(stderr, "Public ITEMLESS: confirmed room=%" PRIu64 "\n", room);
    if (!valid && confirmed_room == room) { confirmed_room = 0; room_owner = NULL; }
    if (valid) { confirmed_room = room; room_owner = owner; }
    else if (!advertised && !wants) { confirmed_room = 0; room_owner = NULL; }
    pthread_mutex_unlock(&mode_lock);
    if (!valid && (advertised || wants)) {
        reject_search("ITEMLESS lobby confirmation failed. Please search again.");
        return 0;
    }
    return 1;
}
static void *ptr(const void *p, size_t offset) { void *v; memcpy(&v,(const char *)p+offset,8); return v; }
static int replacement(uint64_t room) {
    pthread_mutex_lock(&mode_lock);
    int different = confirmed_room && confirmed_room != room;
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
    if (room_owner == owner && (!room || room == confirmed_room)) reset_locked();
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
    installed=1;
    puts("Public ITEMLESS: isolated matchmaking hooks installed (full-party gate, native OK notice)");
    return 1;
}
