/* Public Itemless: authenticated search extension and lobby confirmation. */
#define _GNU_SOURCE
#ifndef SACREDFIRE_RELEASE
#define SACREDFIRE_RELEASE "unversioned"
#endif
#include "runtime.h"
#include "runtime_http.h"
#include "runtime_json.h"
#include "runtime_hall_of_fame.h"
#include "runtime_np_online.h"
#include "runtime_ranked.h"
#include "bb_sha256.h"
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

static pthread_once_t device_once = PTHREAD_ONCE_INIT;
static char device_id[65];

static void init_ranked_device(void) {
    char machine[160] = "", input[224];
#ifdef _WIN32
    HMODULE library = LoadLibraryA("advapi32.dll");
    if (!library) return;
    typedef LSTATUS (WINAPI *ReadRegistry)(HKEY,LPCSTR,LPCSTR,DWORD,LPDWORD,PVOID,LPDWORD);
    ReadRegistry read = (ReadRegistry)GetProcAddress(library,"RegGetValueA");
    DWORD size = sizeof machine;
    LSTATUS status = read ? read(HKEY_LOCAL_MACHINE,"SOFTWARE\\Microsoft\\Cryptography","MachineGuid",
        RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,NULL,machine,&size) : ERROR_INVALID_FUNCTION;
    FreeLibrary(library);
    if (status != ERROR_SUCCESS) return;
#else
    FILE *file = fopen("/etc/machine-id","r");
    if (!file) file = fopen("/var/lib/dbus/machine-id","r");
    if (!file) return;
    char *read = fgets(machine,sizeof machine,file);
    fclose(file);
    if (!read) return;
#endif
    size_t length = strcspn(machine,"\r\n");
    if (length < 16 || length >= sizeof machine - 1) return;
    machine[length] = 0;
    for (size_t i=0;i<length;++i) machine[i]=(char)tolower((unsigned char)machine[i]);
    int count = snprintf(input,sizeof input,"SacredFire Ranked device v1:%s",machine);
    if (count < 0 || (size_t)count >= sizeof input) return;
    uint8_t digest[32];
    bb_sha256((const unsigned char *)input,(uint64_t)count,digest);
    for (int i=0;i<32;++i) snprintf(device_id+i*2,3,"%02x",digest[i]);
}

static const char *ranked_device(void) {
    pthread_once(&device_once,init_ranked_device);
    return device_id;
}

static pthread_mutex_t mode_lock = PTHREAD_MUTEX_INITIALIZER;
static int installed, selected, ranked_selected, searching, allow_party;
/* Whether the party is in a Ranked search. Survives reset_locked (which follows a leave) so a
 * guest that takes over as host re-searches Ranked, not Casual (2026-11-19). */
static int joined_ranked;
/* Snapshot of the found room's mode, used to return to the right ModeSelect on a party leave. */
static int joined_itemless;
__attribute__((weak)) void bbgpu_notify(const char *, const char *, const char *, int, const void *, uint64_t);
static uint64_t last_departure_room, last_departure_version, last_departure_pid;
static uint64_t room_alive_after;
static uint64_t left_room_pending; /* Survives lobby teardown until the actual leave. */
static uint64_t race_host_room, race_host_pid, pending_host_room, host_attempt_until, host_retry_after;
static uint64_t prepromoted_room;
static int local_race_quitting;
static int had_public_race;
static void **match_session_manager;
static void (ABI *native_start_match_host)(void *);
static void (ABI *native_bind_race_connection)(void *, void *);
static void (ABI *native_set_race_connection)(void *, void *);
static void (ABI *native_race_switch_original)(void *, void *);
static uint64_t (ABI *native_take_racer_ownership)(void *, void *);
static void **race_racers, **race_object_root;
static _Thread_local uint64_t race_switch_departed;
static int (ABI *native_remove_player_original)(void *,void *);
static uint64_t (ABI *native_peer_leave_original)(void *);
static unsigned char *party_trace_image;
static void (ABI *native_join_match_host)(void *, uint64_t, const char *, const char *);
static unsigned char *race_join_state_call;
static void *race_join_old_connection;
static uint64_t race_join_room, race_join_until, race_lobby_host_pid;
static uint64_t race_handoff_departed_pid;
static int confirmed_itemless, confirmed_ranked;
static uint64_t request_nonce, registration_nonce, confirmed_room;
static unsigned char *room_owner;
static uint64_t matchmaking_room;
static unsigned char *matchmaking_owner;
static int matchmaking_search, matchmaking_created;
static int waiting_for_opponent, awaiting_public_peer, prepare_requeue_host;
static uint64_t lobby_host_retry_at;
static uint64_t public_search_since, departure_sent_room;
static void (ABI *native_send_departure)(void);
static int (ABI *native_remove_player)(void *, void *);
static void (ABI *native_disconnect_player)(void *, uint64_t);
static uint64_t requeued_room;
static struct PendingLobbyCreate {
    uint64_t room, search, version;
    unsigned host_len, backend_len;
    char docs[];
} *pending_lobby_create, *pending_lobby_join;

int runtime_public_waiting_for_opponent(void) {
    int count = __atomic_load_n(&waiting_for_opponent, __ATOMIC_ACQUIRE);
    return count ? count : __atomic_load_n(&awaiting_public_peer, __ATOMIC_ACQUIRE);
}
static int menu_party_returned = 1;
/* The player's own party when they last entered a lobby session (a search, a private room),
 * before any room member joined it: 1 when they went alone. */
static int party_at_entry = 1;
/* That session is a private room: leaving it never brings anyone along, not even a party that
 * came in together (2026-11-16, the user: leaving a private room must not group you with anyone). */
static int entry_private;
static int private_lobby_open;
static int guest_room_open; /* this player is on the room's own screens */
static int queue_return_pending, party_return_received, party_exit_sent;
static int ranked_return_party;
static void (ABI *native_party_quit_trace)(void *);
static int ranked_party_size(void);
/* Entering a lobby session: from now on, going back to the menus may bring the party along. */
static void arm_party_return(void) {
    if (__atomic_exchange_n(&menu_party_returned,0,__ATOMIC_SEQ_CST)) {
        ranked_return_party = 0;
        __atomic_store_n(&party_exit_sent,0,__ATOMIC_SEQ_CST);
        __atomic_store_n(&party_at_entry,ranked_party_size(),__ATOMIC_SEQ_CST);
        __atomic_store_n(&entry_private,0,__ATOMIC_SEQ_CST);
    }
}
static void arm_private_room(void) {
    arm_party_return();
    __atomic_store_n(&entry_private,1,__ATOMIC_SEQ_CST);
    __atomic_store_n(&private_lobby_open,0,__ATOMIC_SEQ_CST);
}
static void (ABI *native_party_return)(void *);
static uint64_t search_mark, search_ui_at, search_poll_after, search_policy_at, search_poll_serial;
static int search_elapsed_base, search_timeout = 180, search_range, search_is_ranked;
static int search_server_active, search_poll_running, search_timeout_pending;
static int party_return_check;
static void return_to_party_menu(void);
/* Stays up after the server reports "matched" so a party member leaving during the found
 * screen still notifies the remaining player. Cleared only on a real reset. */
static int match_found;
static void send_confirmed_departure(uint64_t departing_room);
static void notify_room_left(void); /* defined with the native hooks below */

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
static void (ABI *native_cancel_queue)(void *);
static void (ABI *native_leave_race_session)(void *,int);
static void (ABI *native_reset_race_session)(void *);
static void (ABI *native_close_party_lobby)(int);
static void ABI stop_search_keep_party(void *self) {
    unsigned char *manager=self;
    native_cancel_queue(*(void **)(manager+0x58));
    void *session=*(void **)(manager+0x90);
    native_leave_race_session(session,1);
    native_reset_race_session(session);
    native_close_party_lobby(0);
}
static void (ABI *native_return_to_lobby)(void);
static uint64_t session_generation;
static void (ABI *native_make_handle)(void **, void *);
static void (ABI *native_resolve_handle)(void **);
static void *(ABI *native_meta_field)(void *, const char *);
static void *(ABI *native_leaderboard_ready)(void *);
static void (ABI *native_release_object)(void *);
static void (ABI *native_destroy_handle)(void **);

static int itemless_client_available(void) {
    const char *assets = getenv("BB_PRIVATE_ARCHIVE_PATCHES");
    return installed && assets && *assets;
}

static uint64_t check_clock(void);
static void *ptr(const void *,size_t);
static void *resolve_private_target(void **slot);
static void notice(const char *message);
static uint64_t capability_next;
static int capability_running, capability_supported;
static void *capability_worker(void *unused) {
    (void)unused;
    char body[16384] = "";
    const int status = runtime_ryunet_request("GET", "/ryunet/theme", body, sizeof body);
    const char *value = status == 200 ? runtime_json_field(body, "itemless_matchmaking") : NULL;
    if (value && *value == '1' && (value[1] == ',' || value[1] == '}' || isspace((unsigned char)value[1])))
        __atomic_store_n(&capability_supported, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&capability_running, 0, __ATOMIC_RELEASE);
    return NULL;
}
int runtime_public_itemless_available(void) {
    if (!itemless_client_available()) return 0;
    const char *v = getenv("BB_PUBLIC_ITEMLESS");
    if ((v && !strcmp(v, "1")) || __atomic_load_n(&capability_supported, __ATOMIC_ACQUIRE)) return 1;
    /* A server unavailable at launch must not disable Itemless for the whole session. */
    const uint64_t now = check_clock();
    if (now >= __atomic_load_n(&capability_next, __ATOMIC_RELAXED) && !__atomic_exchange_n(&capability_running, 1, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&capability_next, now + 5000, __ATOMIC_RELAXED);
        pthread_t worker;
        if (pthread_create(&worker, NULL, capability_worker, NULL))
            __atomic_store_n(&capability_running, 0, __ATOMIC_RELEASE);
        else pthread_detach(worker);
    }
    return 0;
}

static void reset_locked(void) {
    __atomic_add_fetch(&session_generation, 1, __ATOMIC_SEQ_CST);
    pending_host_room = 0;
    host_attempt_until = 0;
    race_handoff_departed_pid = 0;
    prepare_requeue_host = 0;
    free(pending_lobby_create);
    pending_lobby_create = NULL;
    free(pending_lobby_join);
    pending_lobby_join = NULL;
    __atomic_store_n(&awaiting_public_peer,0,__ATOMIC_RELEASE);
    requeued_room = public_search_since = departure_sent_room = 0;
    __atomic_store_n(&waiting_for_opponent, 0, __ATOMIC_RELEASE);
    matchmaking_room = 0;
    matchmaking_owner = NULL;
    matchmaking_search = matchmaking_created = 0;
    confirmed_room = 0;
    confirmed_itemless = confirmed_ranked = 0;
    room_owner = NULL;
    request_nonce = 0;
    searching = 0;
    allow_party = 0;
    search_mark = search_ui_at = search_poll_after = search_policy_at = 0;
    search_elapsed_base = search_range = search_is_ranked = search_server_active = search_poll_running = 0;
    match_found = 0;
    search_timeout = 180;
    search_timeout_pending = 0;
    party_return_check = 0;
    ++search_poll_serial;
    if (race_join_room && race_join_state_call) {
        memcpy(race_join_state_call,"\xe8\x85\xe8\xff\xff",5);
        race_join_room=0;
    }
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
/* The host is back in the online menus: the room's private flag must not follow it into the
 * next mode (2026-11-17: matchmaking with a party opened the private lobby's map selector). */
static void clear_room_private(unsigned char *party) { if (party) party[0x6c] = 0; }
/* Takeover stays inside the same room. Reset only party-exit bookkeeping; a full online-session
 * reset destroys lobby-player state while the native room still references it. */
void runtime_public_itemless_takeover_host(void) {
    if (__atomic_load_n(&menu_party_returned, __ATOMIC_SEQ_CST))
        __atomic_store_n(&party_at_entry, ranked_party_size(), __ATOMIC_SEQ_CST);
    __atomic_store_n(&entry_private, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&queue_return_pending, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&party_return_received, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&menu_party_returned, 0, __ATOMIC_SEQ_CST);
    /* A guest never pressed the Ranked/Itemless button locally; it learned the mode from the
     * host's confirmation. Restore it before native re-issues start_search, else the party's
     * Ranked queue restarts as Casual. */
    if (joined_ranked) {
        ranked_selected = 1;
        search_is_ranked = 1;
    }
    if (joined_itemless) selected = 1;
}
/* A guest back in the menus has left the host's room for good, but nothing leaves the party:
 * both sides keep it in their Lobby Members list and the host never hears about it (2026-11-18).
 * Leave it the game's way: 0xa9ffd0 tells the host (message 0x29) and empties the roster. */
static uint64_t guest_quit_at;
static int leave_party_as_guest(void) {
    if (ranked_return_party) return 0;
    if (guest_quit_at && check_clock() - guest_quit_at < 5000) return 0;
    unsigned char *party = party_manager ? *party_manager : NULL;
    unsigned char *session = party ? *(void **)(party + 0x18) : NULL;
    if (!session || ranked_party_size() < 2) return 0;
    unsigned char *connection = *(void **)(session + 0x10);
    int role = 0;
    if (connection) memcpy(&role, connection + 0x118, sizeof role);
    if (role < 2) return 0;
    guest_quit_at = check_clock();
    if (native_party_quit_trace) native_party_quit_trace(party);
    puts("Online party: guest left the host's party");
    return 1;
}
/* The game left a session that is not this player's own: a party guest back out of the host's
 * room. The party leave is the same, whatever screen the guest ended on. */
void runtime_public_itemless_guest_left(const char *sid) {
    const char *me = runtime_np_get_online_name();
    char mine[128];
    if (!me || !*me || !sid || !*sid) return;
    snprintf(mine, sizeof mine, "cusa13795-session-%s", me);
    if (!strncmp(sid, mine, strlen(mine))) return; /* its own session, not a host's room */
    leave_party_as_guest();
}
/* Inside the host's private room (its Lobby Members rows are up while the game's private flag is
 * set) and out of it again: the guest leaves the host's party, the game's own way. This does not
 * depend on which screen is focused, so every way out of the room reaches it. */
void runtime_public_itemless_guest_room(int inside) {
    if (inside) { guest_room_open = 1; return; }
    if (!guest_room_open) return;
    guest_room_open = 0;
    leave_party_as_guest();
}
void runtime_public_itemless_ranked_return(void) {
    /* Match teardown must not turn a premade member into a party departure. */
    ranked_return_party = !entry_private && party_at_entry > 1;
    __atomic_store_n(&queue_return_pending, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&party_return_received, 1, __ATOMIC_SEQ_CST);
    runtime_public_itemless_menu_return();
}
/* Use the party notification sent by native Quit with Party, without reloading the host. */
void runtime_public_itemless_menu_return(void) {
    if (__atomic_exchange_n(&menu_party_returned,1,__ATOMIC_SEQ_CST)) return;
    __atomic_store_n(&local_race_quitting,1,__ATOMIC_RELEASE);
    const int queue_return = __atomic_exchange_n(&queue_return_pending,0,__ATOMIC_SEQ_CST);
    const int received_return = __atomic_exchange_n(&party_return_received,0,__ATOMIC_SEQ_CST);
    notify_room_left(); /* native Quit path: server must know now, not on conn close */
    runtime_public_itemless_reset();
    if (runtime_room_mode_following()) runtime_public_itemless_party_ready();
    const int private_room = runtime_private_match_active() || runtime_online_mode_in_private();
    if (network_manager && *network_manager) native_stop_matchmaking(*network_manager);
    unsigned char *party = party_manager ? *party_manager : NULL;
    clear_room_private(party);
    unsigned char *session = party ? *(void **)(party + 0x18) : NULL;
    unsigned char *connection = session ? *(void **)(session + 0x10) : NULL;
    int role = 0;
    if (connection) memcpy(&role, connection + 0x118, sizeof role);
    /* Native Quit with Party takes everyone in the session along: the room's members too. Only a
     * party that existed before the room (friends who searched or came in together) goes back
     * together; a player who came alone leaves the room's players behind (2026-11-15: after a
     * private, public or Ranked room everyone stayed grouped with its host). */
    const int before = __atomic_load_n(&party_at_entry, __ATOMIC_SEQ_CST);
    printf("Online party: menu return role=%d private=%d before=%d\n", role, private_room, before);
    if (role == 1 && before > 1 && (ranked_return_party || queue_return || (!entry_private && !private_room))) {
        /* In a found room the server addresses only this search's followers.
         * Before placement, the native party session contains the premade. */
        if (!__atomic_load_n(&party_exit_sent,__ATOMIC_SEQ_CST) && native_party_return) native_party_return(party);
    } else if (role == 1) {
        /* Server-backed private party: promote a staying member before the native quit so the
         * session is never deleted (no guest teardown -> no allocator churn). Never touch a
         * native role during a race: defer to the end-race hook. */
        if (private_room && runtime_party_server_enabled()) {
            char members[8][24];
            int n = runtime_room_members(members, 8);
            const char *me = runtime_np_get_online_name();
            const char *successor = NULL;
            for (int i = 0; i < n; ++i) {
                /* case-insensitive: the same pick as the game's takeover and the server */
                if (!members[i][0] || (me && !strcasecmp(members[i], me))) continue;
                if (!successor || strcasecmp(members[i], successor) < 0) successor = members[i];
            }
            if (successor) {
                if (runtime_room_mode_race_frozen())
                    runtime_party_server_defer_promote(successor); /* never touch native during a race */
                else
                    runtime_party_server_promote(successor);
            }
        }
        if (native_party_quit_trace) native_party_quit_trace(party);
        puts("Online party: host left alone; remaining members elect a new host");
    } else if (!received_return && !ranked_return_party && private_room) {
        leave_party_as_guest();
    }
}
void runtime_public_itemless_select(int enabled) {
    runtime_public_itemless_reset();
    selected = !!enabled;
}
int runtime_public_itemless_selected(void) { return selected; }
int runtime_ranked_selected(void) { return ranked_selected; }
/* A search or a found match is in progress (used to tell the host when a party member bails). */
int runtime_public_itemless_match_active(void) {
    pthread_mutex_lock(&mode_lock);
    const int active = searching || matchmaking_search || search_server_active || match_found;
    pthread_mutex_unlock(&mode_lock);
    return active;
}
/* The host's roster shrank during matchmaking: tell the player (debounced) that a party member
 * left so they can drop the queue instead of continuing to match short-handed. */
void runtime_public_itemless_party_left_notice(void) {
    static uint64_t last_notice;
    const uint64_t now = check_clock();
    if (last_notice && now - last_notice < 10000) return;
    last_notice = now;
    notice("A player left your party during matchmaking. Leave and search again.");
}
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

int runtime_public_itemless_lobby_mode(void) {
    pthread_mutex_lock(&mode_lock);
    uint64_t native_room = 0;
    if (room_owner) memcpy(&native_room, room_owner + 0x958, 8);
    int mode = confirmed_room && native_room == confirmed_room ? confirmed_itemless : selected;
    pthread_mutex_unlock(&mode_lock);
    return !!mode;
}

int runtime_ranked_active(void) {
    pthread_mutex_lock(&mode_lock);
    uint64_t native_room = 0;
    if (room_owner) memcpy(&native_room, room_owner + 0x958, 8);
    const int active = confirmed_ranked && confirmed_room && native_room == confirmed_room;
    pthread_mutex_unlock(&mode_lock);
    return active;
}

/* Room id whose room-short POST was already sent (0 none). */
static uint64_t short_sent_room;
/* Room whose room-alive answer was fetched (0 none) and that answer. The native map-vote
 * handler is dispatched more than once for one match, so one request per room is enough. */
static uint64_t checked_room;
static int checked_status;

/* The confirmed ranked room and its queue (0 Normal, 1 Itemless), 0 not ranked. */
int runtime_ranked_session(uint64_t *room, int *mode) {
    pthread_mutex_lock(&mode_lock);
    uint64_t native_room = 0;
    if (room_owner) memcpy(&native_room, room_owner + 0x958, 8);
    const int active = confirmed_ranked && confirmed_room && native_room == confirmed_room;
    if (active) {
        if (room) *room = confirmed_room;
        if (mode) *mode = confirmed_itemless ? 1 : 0;
    }
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
/* The game's found-room screen shows a BACK (O/B) legend. Hide its icon+text while a room is
 * found so nobody quits mid-matchmaking; still in queue the legend stays. */
int runtime_public_found_room(void) {
    pthread_mutex_lock(&mode_lock);
    const int found = matchmaking_room != 0;
    pthread_mutex_unlock(&mode_lock);
    return found;
}
/* The found room this client is in, 0 in the queue. Callers need the id to act once per room. */
uint64_t runtime_public_room(void) {
    pthread_mutex_lock(&mode_lock);
    const uint64_t room = matchmaking_room;
    pthread_mutex_unlock(&mode_lock);
    return room;
}

/* Ranked needs enough live players. At the countdown, ask the server how many are really in the
 * room. If below the server's min_players, tell the server to dissolve it (room-short) so the
 * short-handed race cannot start; the server pushes every player back to the queue.
 *
 * min_players is the server's rule and is used as-is (there is no client-side copy of it).
 *
 * Returns:
 *   1  this client saw fewer than min_players live players, posted room-short -> caller shows
 *      the notice.
 *   0  room still raceable (>= min_players live players) -> countdown proceeds normally.
 *  -1  room already dissolved (another client triggered room-short) -> caller still shows the
 *      notice and cancels, so every player in the room gets the same native notice instead of
 *      only whoever polled first.
 *
 * The returned values are status codes, not booleans: -1 is truthy in C, so callers must compare
 * against 1 and -1 explicitly. */
static void *room_short_worker(void *arg) {
    const uint64_t room = (uint64_t)(uintptr_t)arg;
    const char *me = runtime_np_get_online_name();
    char target[512], body[256];
    if (!me || !*me) return NULL;
    snprintf(target, sizeof target, "/ryunet/room-short?name=%s&room=%llu", me, (unsigned long long)room);
    const int status = runtime_ryunet_request("POST", target, body, sizeof body);
    fprintf(stderr, "Public matchmaking: room %llu reported short (%d)\n", (unsigned long long)room, status);
    return NULL;
}

int runtime_public_ranked_short(void) {
    uint64_t room;
    int checked, cached;
    pthread_mutex_lock(&mode_lock);
    room = matchmaking_room;
    const int ranked = confirmed_ranked || joined_ranked || search_is_ranked;
    checked = checked_room == room;
    cached = checked_status;
    pthread_mutex_unlock(&mode_lock);
    if (!room) {
        /* New room, new short report. */
        __atomic_store_n(&short_sent_room, 0, __ATOMIC_SEQ_CST);
        pthread_mutex_lock(&mode_lock);
        checked_room = 0;
        pthread_mutex_unlock(&mode_lock);
        return 0;
    }
    if (!ranked) return 0;
    if (checked) return cached;
    /* No request here: this runs on the game's thread at the end of the map vote, and a server slow to
     * answer froze the game at 00 (20/11: queries of 3 to 18 s). The room-alive thread's latest
     * answer says how many players the room has (runtime_public_ranked_clock); without a fresh one the
     * race goes ahead. A room-dissolved answer is handled by check_found_room_alive. */
    int64_t since = 0;
    int players = 0;
    if (!runtime_public_ranked_clock(&since, &players)) return 0;
    const int min_required = runtime_public_ranked_min_players();
    /* The server's count is its lobby connections, which drop while a player is still in the game:
     * a room the game's own list holds at the minimum is not dissolved (20/11, room 23). Without a
     * list the server's count is the only word. */
    const int local = runtime_online_mode_room_players();
    fprintf(stderr, "Public matchmaking: ranked room %llu has %d live player(s) (min required: %d, game list: %d)\n",
            (unsigned long long)room, players, min_required, local);
    int status_code = 0;
    if (players > 0 && players < min_required && !(local > 0 && local >= min_required)) {
        /* Post once per room, on a thread of its own: the server dissolves the room and pushes
         * everyone back to the queue. */
        if (__atomic_exchange_n(&short_sent_room, room, __ATOMIC_SEQ_CST) != room) {
            pthread_t thread;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&thread, &attr, room_short_worker, (void *)(uintptr_t)room))
                __atomic_store_n(&short_sent_room, 0, __ATOMIC_SEQ_CST);
            pthread_attr_destroy(&attr);
        }
        status_code = 1;
    }
    pthread_mutex_lock(&mode_lock);
    checked_room = room;
    checked_status = status_code;
    pthread_mutex_unlock(&mode_lock);
    return status_code;
}

static void notice(const char *message) {
    fprintf(stderr, "Public ITEMLESS: %s\n", message);
    /* Use the Online/offline error helper: one OK button and native modal input. */
    if (native_notice) native_notice(runtime_text(message), NULL);
}

/* Ranked needs a nickname linked to Discord or Steam (runtime_ranked_account, the server's
 * ranked_account.go): said before the Ranked lobby opens. The server's admission check refuses a
 * search without one anyway, with this same text (translated by it: scripts/octr_texts.py). */
#define RANKED_ACCOUNT_NOTICE "Ranked needs a Discord or Steam account linked to every player. Link yours in the launcher and try again."
static int ranked_account_allowed(void) {
    if (runtime_ranked_account() != 0) return 1;
    notice(RANKED_ACCOUNT_NOTICE);
    return 0;
}

/* Public matchmaking and Ranked take the game's own files and character mods only; any other mod or
 * executable patch is for private rooms (scripts/online_content.py, runtime_online_content). */
#define PUBLIC_CONTENT_NOTICE "Public matchmaking and Ranked allow only character, sound and HUD mods. Your game has other mods or patches: play them in private rooms."
/* A public search refused for its content: 1 the notice is up, 2 its Continue was pressed (go back) */
static int content_return;
static int public_content_allowed(void) {
    if (strcmp(runtime_online_content(), "modified")) return 1;
    notice(PUBLIC_CONTENT_NOTICE);
    return 0;
}

static int ranked_party_size(void) {
    unsigned char *party=party_manager ? *party_manager : NULL;
    void *roster=party ? *(void **)(party+0x30) : NULL;
    int count=roster ? *(int *)((char *)roster+12) : 0;
    void **items=roster ? *(void ***)((char *)roster+0x20) : NULL;
    if (count<0 || count>8 || (count && !items)) return 0;
    int total=1;
    for (int i=0;i<count;++i)
        if (items[i] && !*((unsigned char *)items[i]+0x2c)) ++total;
    return total;
}

int runtime_online_party_active(void) {
    unsigned char *party=party_manager ? *party_manager : NULL;
    if (!installed || !party) return 0;
    unsigned char *session=*(void **)(party+0x18);
    unsigned char *connection=session ? *(void **)(session+0x10) : NULL;
    int peers=0;
    if (connection) memcpy(&peers,connection+0x30,sizeof peers);
    return peers>0; /* Native hasActiveParty_VSC uses the transport count, not the race roster. */
}
static int ranked_entry_allowed(void) {
    if (!*ranked_device()) { notice("Cannot verify this device for Ranked. Check access to the OS device identifier."); return 0; }
    const int players=ranked_party_size();
    if (players>4) {
        char message[320];
        snprintf(message,sizeof message,runtime_text("Your party has %d players. Ranked supports at most 4, regardless of device. Reduce the party or choose Public Matchmaking (up to 8)."),players);
        notice(message);return 0;
    }
    return 1;
}

static void reject_search(const char *message) {
    pthread_mutex_lock(&mode_lock);
    reset_locked();
    snprintf(rejection, sizeof rejection, "%s", message);
    pthread_mutex_unlock(&mode_lock);
}

struct SearchPoll { uint64_t generation, serial; char target[160]; };

static int search_integer(const char *body, const char *key, int *out) {
    const char *value = runtime_json_field(body,key);
    if (!value) return 0;
    char *end;
    long n = strtol(value,&end,10);
    if (end==value || n < -1 || n > 10000 || (*end!=',' && *end!='}' && !isspace((unsigned char)*end))) return 0;
    *out=(int)n;
    return 1;
}

static void *search_poll(void *arg) {
    struct SearchPoll *job=arg;
    char body[512];
    int elapsed=0, timeout=0, range=0;
    int status=runtime_ryunet_request("GET",job->target,body,sizeof body);
    const char *state=status==200 ? runtime_json_field(body,"state") : NULL;
    int valid=state && search_integer(body,"elapsed",&elapsed) && search_integer(body,"timeout",&timeout) &&
        search_integer(body,"range",&range) && elapsed>=0 && elapsed<=timeout && timeout>=30 && timeout<=1800;
    const char *ranked=valid ? runtime_json_field(body,"ranked") : NULL;
    const uint64_t now=check_clock();
    pthread_mutex_lock(&mode_lock);
    if (job->generation==session_generation && job->serial==search_poll_serial) {
        search_poll_running=0;
        if (valid) {
            if (party_return_check == 1) {
                if (!strncmp(state,"\"searching\"",11) || !strncmp(state,"\"matched\"",9))
                    party_return_check=0;
                else if (!strncmp(state,"\"idle\"",6) || !strncmp(state,"\"timed_out\"",11))
                    party_return_check=2;
            }
            search_timeout=timeout;search_range=range;search_policy_at=now;
            if (!strncmp(state,"\"searching\"",11) || !strncmp(state,"\"timed_out\"",11)) {
                if (!matchmaking_room) match_found=0;
                arm_party_return();
                search_server_active=1;search_mark=now;search_elapsed_base=elapsed;
                search_is_ranked=ranked && !strncmp(ranked,"true",4);
                if (search_is_ranked) joined_ranked=1; /* sticky server signal for takeover */
            } else if (!strncmp(state,"\"matched\"",9)) {
                search_server_active=0;
                if (!matchmaking_search) search_mark=0;
                search_timeout_pending=0;
                match_found=1;
            } else {
                if (!matchmaking_room && !strncmp(state,"\"idle\"",6)) match_found=0;
                if (!matchmaking_search) {
                    search_server_active=0;search_mark=0;
                }
            }
        }
    }
    pthread_mutex_unlock(&mode_lock);
    free(job);
    return NULL;
}

int runtime_matchmaking_view(int *elapsed, int *timeout, int *range, int *ranked) {
    uint64_t now=check_clock();
    pthread_mutex_lock(&mode_lock);
    search_ui_at=now;
    int active=search_mark && !matchmaking_room && (matchmaking_search || search_server_active);
    *elapsed=search_elapsed_base+(int)((now>=search_mark ? now-search_mark : 0)/1000);
    *timeout=search_timeout;
    *elapsed=*elapsed>*timeout ? *timeout : *elapsed;
    *range=search_policy_at && now-search_policy_at<5000 ? search_range : 0;
    *ranked=search_is_ranked;
    if (!*ranked) *timeout=*range=0;
    if (matchmaking_room && public_search_since) {
        active=1;
        *elapsed=(int)((now >= public_search_since ? now-public_search_since : 0)/1000);
        *timeout=*range=*ranked=0;
    }
    pthread_mutex_unlock(&mode_lock);
    return active;
}

static void update_search_clock(void) {
    uint64_t now=check_clock();
    pthread_mutex_lock(&mode_lock);
    int expired=search_mark && !matchmaking_room && (matchmaking_search || search_server_active) &&
        search_elapsed_base+(int)((now>=search_mark ? now-search_mark : 0)/1000)>=search_timeout;
    struct SearchPoll *job=NULL;
    const int poll_needed=party_return_check==1 ||
        (!__atomic_load_n(&menu_party_returned,__ATOMIC_SEQ_CST) && !expired && !search_timeout_pending &&
         search_ui_at && now-search_ui_at<500 && !matchmaking_room);
    if (!expired && poll_needed && !search_poll_running && now>=search_poll_after) {
        const char *name=runtime_np_get_online_name();
        if (name && *name && (job=calloc(1,sizeof *job))) {
            job->generation=session_generation;job->serial=++search_poll_serial;
            snprintf(job->target,sizeof job->target,"/ryunet/matchmaking-status?name=%s",name);
            search_poll_running=1;search_poll_after=now+1000;
        }
    }
    if (expired) search_timeout_pending=1;
    pthread_mutex_unlock(&mode_lock);
    if (expired) return;
    if (job) {
        pthread_t worker;
        if (!pthread_create(&worker,NULL,search_poll,job)) pthread_detach(worker);
        else {
            pthread_mutex_lock(&mode_lock);
            if (job->serial==search_poll_serial) search_poll_running=0;
            pthread_mutex_unlock(&mode_lock);
            free(job);
        }
    }
}

/* The party's players by name (a private room's are its host's party), copied on the game's
 * thread in the lobby for voice chat (gpu/shim/bbport_voice.cpp: the host tells the server who
 * may hear the room). */
static pthread_mutex_t members_lock = PTHREAD_MUTEX_INITIALIZER;
static char members_copy[8][24];
static int members_count;

static void members_tick(void) {
    char now[8][24];
    int n = 0;
    /* only a private room's (the host's party): public rooms' players are the server's own */
    unsigned char *party = party_manager && runtime_online_mode_in_private() ? *party_manager : NULL;
    void *roster = party ? *(void **)(party + 0x30) : NULL;
    const int count = roster ? *(int *)((char *)roster + 12) : 0;
    void **items = roster ? *(void ***)((char *)roster + 0x20) : NULL;
    for (int i = 0; items && count > 0 && count <= 8 && i < count && n < 8; ++i) {
        const char *name = items[i] ? *(const char **)((char *)items[i] + 0x10) : NULL;
        if (name && *name) snprintf(now[n++], sizeof now[0], "%s", name);
    }
    pthread_mutex_lock(&members_lock);
    memcpy(members_copy, now, sizeof now[0] * (size_t)n);
    members_count = n;
    pthread_mutex_unlock(&members_lock);
}

int runtime_room_members(char out[][24], int max) {
    pthread_mutex_lock(&members_lock);
    const int n = members_count < max ? members_count : max;
    memcpy(out, members_copy, sizeof members_copy[0] * (size_t)(n > 0 ? n : 0));
    pthread_mutex_unlock(&members_lock);
    return n;
}

static void *ABI start_search(void **result, void *self, const char *raw, unsigned limit, void *response);
/* Someone left the party while a queue/found room was active: cancel the search and send every
 * screen back to its ModeSelect. The native party stays intact (host reassignment is handled by
 * the existing takeover path), so the remaining players can queue again together. */
static volatile int queue_abort_pending;
static uint64_t suppress_search_until; /* briefly swallow the native auto-search after host loss */
void runtime_public_itemless_abort_queue(void) {
    pthread_mutex_lock(&mode_lock);
    const int resumed = requeued_room && matchmaking_search && !matchmaking_room;
    pthread_mutex_unlock(&mode_lock);
    if (resumed) return;
    __atomic_store_n(&suppress_search_until, check_clock() + 3000, __ATOMIC_SEQ_CST);
    __atomic_store_n(&queue_abort_pending, 1, __ATOMIC_SEQ_CST);
}
static void do_queue_abort(void) {
    if (!__atomic_exchange_n(&queue_abort_pending, 0, __ATOMIC_SEQ_CST)) return;
    pthread_mutex_lock(&mode_lock);
    const int leave_room = confirmed_ranked != 0 || joined_ranked;
    const int ret_mode = confirmed_ranked || joined_ranked ? 2 :
                         (confirmed_itemless || joined_itemless ? 1 : 0);
    pthread_mutex_unlock(&mode_lock);
    runtime_online_mode_set_return_mode(ret_mode);
    /* Only the promoted host (takeover cleared followed_host) returns to ModeSelect. A party
     * member still following stays in the party and sits at "Waiting on host...". */
    const int following = runtime_room_mode_following();
    fprintf(stderr, "Public matchmaking: party member left queue, following=%d mode=%d\n", following, ret_mode);
    if (!following && network_manager && *network_manager)
        runtime_online_mode_matchmaking_cancel(leave_room); /* route 1 ranked / 0 matchmaking */
    runtime_public_itemless_reset();
    runtime_public_itemless_party_ready();
}
/* The found room's liveness is asked on a thread of its own: this check runs on every lobby frame
 * of the game's thread, and the request (a round trip, up to the socket's 8 s timeout) made the
 * game stutter once a second in found rooms ("the polling is still deferred, not async", 20/11).
 * The frame only reads the answer: a room the server says is gone. */
static int room_alive_busy;
static int room_alive_racing; /* Written before starting the single polling worker. */
static uint64_t room_alive_gone;   /* a found room the server reported dissolved, 0 none */
/* The server's RYUNET_RANKED_MIN_PLAYERS, from room-alive; 0 unknown (no answer yet): a guess of 6 said
 * "WAITING 3/6" in a room of 3 that was already counting (20/11). */
static int ranked_min_seen = 0;

/* Players a Ranked room needs to start, as the server last said (6 until it answered). */
int runtime_public_ranked_min_players(void) { return __atomic_load_n(&ranked_min_seen, __ATOMIC_SEQ_CST); }

/* The server's countdown clock for a Ranked room (room-alive's ready_ms, ctr_foundroom.go): -1 short of
 * its minimum, else the milliseconds since the room reached it; with the server's count of its players.
 * Every client sets its map countdown from it, so all of them start together (runtime_online_mode.c). */
static pthread_mutex_t ranked_clock_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t ranked_clock_room, ranked_clock_at;
static int64_t ranked_clock_ready;
static int ranked_clock_players;
/* The server's clock placed on ours, NTP style: its now_ms against the middle of our request, from the
 * quickest round trip of the last half minute (the least room for a one-way delay). ready_at_ms is the
 * moment the room reached its minimum on that clock, 0 while it is short. */
static int64_t ranked_clock_offset, ranked_clock_ready_at;
static uint64_t ranked_clock_rtt, ranked_clock_rtt_at;
static int ranked_clock_synced;

int runtime_public_ranked_clock(int64_t *since_ms, int *players) {
    pthread_mutex_lock(&mode_lock);
    const uint64_t room = matchmaking_room;
    pthread_mutex_unlock(&mode_lock);
    const uint64_t now = check_clock();
    pthread_mutex_lock(&ranked_clock_lock);
    /* 20 s: once placed, the clock counts on ours, so a slow answer or two (3 to 18 s on 20/11) changes nothing */
    /* A countdown already on the server's clock goes on with it for 2 minutes without an answer (it
     * counts on our own clock): a stale one put it back to 60 mid-countdown (20/11, answers of 21 s). */
    const int fresh = room && ranked_clock_room == room && ranked_clock_at &&
                      (now - ranked_clock_at < 20000 ||
                       (ranked_clock_ready >= 0 && ranked_clock_synced && ranked_clock_ready_at > 0 && now - ranked_clock_at < 120000));
    if (fresh) {
        /* -1 short of players, -2 the lobby closed (the map vote ended, a race), else the time since */
        int64_t since = ranked_clock_ready < 0 ? ranked_clock_ready : 0;
        if (ranked_clock_ready >= 0) {
            since = ranked_clock_synced && ranked_clock_ready_at > 0
                        ? (int64_t)now + ranked_clock_offset - ranked_clock_ready_at   /* every frame, on our clock */
                        : ranked_clock_ready + (int64_t)(now - ranked_clock_at);       /* an older server */
            if (since < 0) since = 0;
        }
        if (since_ms) *since_ms = since;
        if (players) *players = ranked_clock_players;
    }
    pthread_mutex_unlock(&ranked_clock_lock);
    return fresh;
}
static void resume_room_search(uint64_t room);
static void *room_alive_worker(void *arg) {
    const uint64_t room = (uint64_t)(uintptr_t)arg;
    const char *me = runtime_np_get_online_name();
    char target[512], body[256];
    if (me && *me) {
        snprintf(target, sizeof target, "/ryunet/room-alive?name=%s&room=%llu&race=%d", me, (unsigned long long)room,room_alive_racing);
        const uint64_t sent = check_clock();
        const int status = runtime_ryunet_request("GET", target, body, sizeof body);
        const uint64_t answered = check_clock();
        {   /* the countdown clock's answers, when they change (20/11: a host went 20 s without one) */
            static int last_status = -2;
            static uint64_t last_room;
            static int last_racing = -1;
            if (status != last_status || room != last_room || room_alive_racing != last_racing) {
                last_racing = room_alive_racing;
                last_status = status;
                last_room = room;
                printf("Ranked clock: room %llu answered %d in %llu ms (race=%d): %.160s\n", (unsigned long long)room, status,
                       (unsigned long long)(answered - sent), room_alive_racing, status == 200 ? body : "");
            }
        }
        if (status == 200) {
            const char *host = runtime_json_field(body, "host_pid");
            if (host && *host) {
                const uint64_t pid = strtoull(host, NULL, 10);
                pthread_mutex_lock(&mode_lock);
                if (matchmaking_room == room) race_host_room = room, race_host_pid = pid;
                pthread_mutex_unlock(&mode_lock);
            }
            const char *alive = runtime_json_field(body, "alive");
            if (alive && !strncmp(alive, "false", 5)) {
                const char *queued = runtime_json_field(body, "requeued");
                pthread_mutex_lock(&mode_lock);
                if (matchmaking_room == room && queued && !strncmp(queued,"true",4))
                    resume_room_search(room);
                else __atomic_store_n(&room_alive_gone, room, __ATOMIC_SEQ_CST);
                pthread_mutex_unlock(&mode_lock);
            }
            const char *min_str = runtime_json_field(body, "min_players");
            const int min_players = min_str && *min_str ? (int)strtol(min_str, NULL, 10) : 0;
            if (min_players >= 1 && min_players <= 8) __atomic_store_n(&ranked_min_seen, min_players, __ATOMIC_SEQ_CST);
            const char *ready = runtime_json_field(body, "ready_ms"), *players = runtime_json_field(body, "players");
            const char *server_now = runtime_json_field(body, "now_ms"), *ready_at = runtime_json_field(body, "ready_at_ms");
            if (ready && *ready && players && *players) {
                pthread_mutex_lock(&ranked_clock_lock);
                if (ranked_clock_room != room) ranked_clock_synced = 0;
                ranked_clock_room = room;
                ranked_clock_ready = strtoll(ready, NULL, 10);
                ranked_clock_players = (int)strtol(players, NULL, 10);
                ranked_clock_at = answered;
                if (server_now && *server_now && ready_at && *ready_at) {
                    /* timed from when the request went out, once connected: a connect that lost its first
                     * packets (3 to 6 s on 20/11) is not a round trip, and halving it put a screen 0.5 to 2 s
                     * off. The quickest answer of the room is kept: the clocks do not drift in a minute. */
                    const uint64_t went = runtime_ryunet_last_sent_ms();
                    const uint64_t from = went && went >= sent && went <= answered ? went : sent;
                    const uint64_t rtt = answered - from;
                    const int64_t offset = strtoll(server_now, NULL, 10) - (int64_t)(from + rtt / 2);
                    if (!ranked_clock_synced || rtt <= ranked_clock_rtt) {
                        ranked_clock_offset = offset;
                        ranked_clock_rtt = rtt;
                        ranked_clock_rtt_at = answered;
                        ranked_clock_synced = 1;
                    }
                    ranked_clock_ready_at = strtoll(ready_at, NULL, 10);
                }
                pthread_mutex_unlock(&ranked_clock_lock);
            }
        }
    }
    __atomic_store_n(&room_alive_busy, 0, __ATOMIC_SEQ_CST);
    return NULL;
}

static void poll_room_alive(uint64_t room, uint64_t now) {
    if (!room || now < room_alive_after ||
        __atomic_exchange_n(&room_alive_busy, 1, __ATOMIC_SEQ_CST)) return;
    room_alive_racing=runtime_replay_race_active();
    if (room_alive_racing) __atomic_store_n(&had_public_race,1,__ATOMIC_RELEASE);
    room_alive_after = now + 1000;
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, room_alive_worker, (void *)(uintptr_t)room))
        __atomic_store_n(&room_alive_busy, 0, __ATOMIC_SEQ_CST);
    pthread_attr_destroy(&attr);
}

uint64_t runtime_public_race_host_pid(void) {
    pthread_mutex_lock(&mode_lock);
    const uint64_t pid = race_host_room == matchmaking_room ? race_host_pid : 0;
    pthread_mutex_unlock(&mode_lock);
    return pid;
}

static int preserve_race_ids(void) {
    return __atomic_load_n(&left_room_pending,__ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&local_race_quitting,__ATOMIC_ACQUIRE) &&
        !runtime_online_mode_lobby_open() &&
        (runtime_replay_race_active() || runtime_online_mode_race_running());
}

static void (ABI *native_rejoin_players)(void *);
static void ABI rejoin_players(void *manager) {
    unsigned char *players[8];
    unsigned ids[8];
    int saved=0;
    if (manager && preserve_race_ids()) {
        unsigned char *list=ptr(manager,0x10);
        const int count=list ? *(int *)(list+12) : 0;
        void **rows=list ? ptr(list,0x20) : NULL;
        for (int i=0;rows && count<=8 && i<count;++i) {
            unsigned char *player=rows[i];
            if (!player || *(unsigned *)(player+0x20)>7 || *(unsigned *)(player+0xc8)>=8) continue;
            players[saved]=player;ids[saved++]=*(unsigned *)(player+0xc8);
        }
    }
    /* Native rejoin registers the local players, then replaces race IDs with join slots. */
    native_rejoin_players(manager);
    for (int i=0;i<saved;++i) {
        unsigned *id=(unsigned *)(players[i]+0xc8);
        if (*id!=ids[i]) {
            fprintf(stderr,"Matchmaking: restored race ID pid=%" PRIu64 " id=%u join_slot=%u after native rejoin\n",
                *(uint64_t *)(players[i]+0xa8),ids[i],*id);
            *id=ids[i];
        }
    }
}

static void (ABI *native_reset_local_racer_ids)(void *);
static void ABI reset_local_racer_ids(void *list) {
    /* Session teardown must not replace an active race ID with a local controller slot. */
    if (preserve_race_ids()) {
        const int count=*(int *)((char *)list+12);
        void **rows=ptr(list,0x20);
        for (int i=0; rows && count<=8 && i<count; ++i) {
            unsigned char *player=rows[i];
            if (player && *(unsigned *)(player+0x20)<=7)
                fprintf(stderr,"Matchmaking: retained race player pid=%" PRIu64 " id=%u local=%u during session reset\n",
                    *(uint64_t *)(player+0xa8),*(unsigned *)(player+0xc8),*(unsigned *)(player+0xb0));
        }
        return;
    }
    native_reset_local_racer_ids(list);
}

static int ABI remove_race_player(void *manager,void *player) {
    uint64_t pid=0;
    if (player) memcpy(&pid,(char *)player+0xa8,sizeof pid);
    /* Replacing a transport is not a departure of every player on that transport. */
    if (race_switch_departed && pid && pid!=race_switch_departed) return 0;
    if (pid && preserve_race_ids()) runtime_online_mode_racer_left(pid);
    return native_remove_player_original(manager,player);
}

static void ABI race_connection_switch(void *race, void *connection) {
    void *network=network_manager ? *network_manager : NULL;
    void *replication=race ? ptr(race,0x20) : NULL;
    if (!replication) return;
    const uint64_t room=__atomic_load_n(&left_room_pending,__ATOMIC_ACQUIRE);
    const int seen=__atomic_load_n(&had_public_race,__ATOMIC_ACQUIRE);
    /* Lobby teardown must release the old roster even after this room raced. */
    if (!connection && (seen || runtime_replay_race_active())) {
        const int keep=!__atomic_load_n(&local_race_quitting,__ATOMIC_ACQUIRE) &&
            !runtime_online_mode_lobby_open() && room && network && ptr(network,0x78)==race;
        if (keep) {
            fprintf(stderr,"Matchmaking: holding race connection during host loss\n");
            return;
        }
        if (!ptr(race,0x18)) {
            if (native_set_race_connection) native_set_race_connection(replication,NULL);
            else {
                void *empty=NULL;
                memcpy((unsigned char *)replication+0x10,&empty,sizeof empty);
            }
            return;
        }
    }
    const uint64_t previous=race_switch_departed;
    pthread_mutex_lock(&mode_lock);
    if (connection && room && room==matchmaking_room &&
        !local_race_quitting && (seen || runtime_replay_race_active()))
        race_switch_departed=race_handoff_departed_pid;
    pthread_mutex_unlock(&mode_lock);
    native_race_switch_original(race,connection);
    race_switch_departed=previous;
}

/* A delayed native 0x33 packet names only a PID. After that PID rejoins the same
 * room, it would remove the new player. Lobby departures come from room updates. */
static uint64_t ABI peer_leave_message(void *message) {
    pthread_mutex_lock(&mode_lock);
    const int lobby = matchmaking_room != 0;
    pthread_mutex_unlock(&mode_lock);
    if (lobby && !runtime_replay_race_active() && !runtime_online_mode_race_running()) {
        fprintf(stderr,"Matchmaking: ignored native peer leave; server room update owns lobby departure\n");
        return 1;
    }
    return native_peer_leave_original(message);
}

static void restore_host_racers(uint64_t room, uint64_t now) {
    pthread_mutex_lock(&mode_lock);
    const int handoff=room && room==matchmaking_room && now<host_attempt_until && !local_race_quitting;
    const uint64_t departed=race_handoff_departed_pid;
    pthread_mutex_unlock(&mode_lock);
    if (!handoff || !native_take_racer_ownership ||
        !(runtime_replay_race_active() || runtime_online_mode_race_running())) return;
    void *network=network_manager ? *network_manager : NULL;
    void *race=network ? ptr(network,0x78) : NULL;
    void *replication=race ? ptr(race,0x20) : NULL;
    void *connection=replication ? ptr(replication,0x10) : NULL;
    if (!connection || *(int *)((char *)connection+0x118)!=1) return;
    void *root=race_object_root ? *race_object_root : NULL;
    void *objects=root ? ptr(root,0x28) : NULL;
    void *list=race_racers && *race_racers ? ptr(*race_racers,0x10) : NULL;
    int count=list ? *(int *)((char *)list+12) : 0;
    void **rows=list ? ptr(list,0x20) : NULL;
    if (!objects || !rows || count<1 || count>8) return;
    for (int i=0;i<count;++i) {
        unsigned char *racer=rows[i];
        if (!racer) continue;
        unsigned char *player=resolve_private_target((void **)(racer+0x10));
        if (ptr(racer,0x10) && !player) continue;
        uint64_t pid=0;
        if (player) memcpy(&pid,player+0xa8,sizeof pid);
        if (player && *(unsigned *)(player+0x20)>7 && (!departed || pid!=departed)) continue;
        unsigned char *entity=resolve_private_target((void **)(racer+0x20));
        /* Rejoining guests reclaim old-host objects; native transfer also notifies the peer. */
        if (entity && !(entity[0x3b]&1)) native_take_racer_ownership(objects,entity);
    }
}

void runtime_public_itemless_poll_race_host(void) {
    pthread_mutex_lock(&mode_lock);
    uint64_t room=matchmaking_room;
    const uint64_t now=check_clock();
    void *manager=NULL, *race_network=NULL, *host_connection=NULL;
    if (race_join_room && race_join_room==room && match_session_manager && *match_session_manager) {
        void *candidate=*match_session_manager;
        void *session=ptr(candidate,0x28);
        void *connection=session ? ptr(session,0x10) : NULL;
        int role=0;
        if (connection) memcpy(&role,(unsigned char *)connection+0x118,sizeof role);
        void *network=network_manager ? *network_manager : NULL;
        void *race=network && ptr(network,0x90)==candidate ? ptr(network,0x78) : NULL;
        if (connection && connection!=race_join_old_connection && role==2 && ptr(connection,0x110) &&
            race && ptr(race,0x18) && ptr(race,0x20) && native_bind_race_connection) {
            race_network=race;
            host_connection=connection;
            fprintf(stderr,"Matchmaking: room %" PRIu64 " guest reconnected to promoted host\n",room);
            memcpy(race_join_state_call,"\xe8\x85\xe8\xff\xff",5);
            race_join_room=0;
        } else if (now>=race_join_until) {
            fprintf(stderr,"Matchmaking: room %" PRIu64 " guest host reconnect timed out\n",room);
            memcpy(race_join_state_call,"\xe8\x85\xe8\xff\xff",5);
            race_join_room=0;
        }
    }
    if (pending_host_room && pending_host_room==room) {
        if (now >= host_attempt_until) {
            fprintf(stderr, "Matchmaking: room %" PRIu64 " native host session timed out\n", room);
            pending_host_room = 0;
        } else if (match_session_manager && *match_session_manager) {
            void *candidate = *match_session_manager;
            void *session = ptr(candidate, 0x28);
            void *connection = session ? ptr(session, 0x10) : NULL;
            int role = 0;
            if (connection) memcpy(&role, (unsigned char *)connection + 0x118, sizeof role);
            if (!ptr(candidate, 0x18) && !ptr(candidate, 0x20)) {
                if (role == 1) {
                    void *network = network_manager ? *network_manager : NULL;
                    void *race = network && ptr(network, 0x90) == candidate ? ptr(network, 0x78) : NULL;
                    void *replication = race ? ptr(race, 0x20) : NULL;
                    if (replication && ptr(replication, 0x10) == connection) {
                        fprintf(stderr, "Matchmaking: room %" PRIu64 " race uses promoted host connection\n", room);
                        pending_host_room = 0;
                    } else if (replication && ptr(race, 0x18) && native_bind_race_connection && now >= host_retry_after) {
                        race_network = race;
                        host_connection = connection;
                        host_retry_after = now + 1000;
                    }
                } else if ((!session || connection) && now >= host_retry_after) {
                    manager = candidate;
                    host_retry_after = now + 1000;
                }
            }
        }
    }
    pthread_mutex_unlock(&mode_lock);
    if (manager) native_start_match_host(manager);
    /* Both roles need native peer cleanup and ownership rebinding; a transport swap skips them. */
    if (race_network) native_bind_race_connection(race_network,host_connection);
    restore_host_racers(room,now);
    poll_room_alive(room ? room : __atomic_load_n(&left_room_pending, __ATOMIC_SEQ_CST), now);
}

/* mode_lock held. A replacement search owns its own timer and keeps the original mode token. */
static void resume_room_search(uint64_t room) {
    free(pending_lobby_create);
    pending_lobby_create = NULL;
    free(pending_lobby_join);
    pending_lobby_join = NULL;
    requeued_room = room;
    prepare_requeue_host = 1;
    lobby_host_retry_at = 0;
    __atomic_store_n(&queue_abort_pending, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&suppress_search_until, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&waiting_for_opponent, 1, __ATOMIC_RELEASE);
    selected = joined_itemless = confirmed_itemless;
    ranked_selected = search_is_ranked = joined_ranked = confirmed_ranked;
    matchmaking_room = confirmed_room = 0;
    matchmaking_owner = room_owner = NULL;
    confirmed_ranked = confirmed_itemless = matchmaking_created = match_found = 0;
    matchmaking_search = 1;
    search_mark = check_clock();
    search_elapsed_base = search_timeout_pending = search_poll_running = 0;
    ++search_poll_serial; /* Discard matched status from before the departure. */
    search_poll_after = search_policy_at = 0;
    __atomic_store_n(&left_room_pending, 0, __ATOMIC_SEQ_CST);
    fprintf(stderr, "Matchmaking: searching again after room=%" PRIu64 " ranked=%d\n", room, ranked_selected);
}

/* Fresh searches already own a native host session. A solo survivor was a guest
 * and can be selected as the next host without another native startSearch. */
static void update_lobby_connection(void) {
    void *start = NULL, *owner = NULL, *join_owner = NULL;
    struct PendingLobbyCreate *ready = NULL, *join = NULL;
    pthread_mutex_lock(&mode_lock);
    void *join_manager=pending_lobby_join && match_session_manager ? *match_session_manager : NULL;
    if (pending_lobby_join && join_manager && !ptr(join_manager,0x18) && !ptr(join_manager,0x20)) {
        join=pending_lobby_join;
        pending_lobby_join=NULL;
        if (join->room==matchmaking_room) join_owner=matchmaking_owner;
    }
    if (__atomic_load_n(&awaiting_public_peer,__ATOMIC_RELAXED) && runtime_online_mode_room_players() > 1) {
        __atomic_store_n(&awaiting_public_peer,0,__ATOMIC_RELEASE);
        if (!waiting_for_opponent) public_search_since = 0;
    }
    if (prepare_requeue_host && native_start_match_host && match_session_manager && *match_session_manager) {
        void *manager = *match_session_manager;
        void *session = ptr(manager,0x28);
        void *connection = session ? ptr(session,0x10) : NULL;
        uint64_t now = check_clock();
        if (!ptr(manager,0x18) && !ptr(manager,0x20)) {
            if (connection && *(int *)((char *)connection+0x118)==1) {
                prepare_requeue_host = 0;
                ready = pending_lobby_create;
                pending_lobby_create = NULL;
                if (ready && ready->room == matchmaking_room) owner = matchmaking_owner;
            }
            else if ((!session || connection) && now >= lobby_host_retry_at) {
                start = manager;
                lobby_host_retry_at = now + 1000;
            }
        }
    }
    pthread_mutex_unlock(&mode_lock);
    if (join_owner) {
        fprintf(stderr,"Matchmaking: native session ready for deferred join room=%" PRIu64 "\n",join->room);
        native_join(join_owner,join->search,join->room,join->docs,join->host_len,
                    join->docs+join->host_len+1,join->backend_len);
    }
    free(join);
    if (start) native_start_match_host(start);
    if (owner) {
        fprintf(stderr,"Matchmaking: native host ready for Public rematch room=%" PRIu64 "\n",ready->room);
        native_create(owner,ready->search,ready->room,ready->version,ready->docs,ready->host_len,
                      ready->docs + ready->host_len + 1,ready->backend_len);
    }
    free(ready);
}

static void check_found_room_alive(void) {
    pthread_mutex_lock(&mode_lock);
    const uint64_t room = matchmaking_room;
    pthread_mutex_unlock(&mode_lock);
    const uint64_t gone = __atomic_exchange_n(&room_alive_gone, 0, __ATOMIC_SEQ_CST);
    const uint64_t now = check_clock();
    {   /* the countdown clock's queries (20/11: a host went 20 s without an answer): the room this game
         * asks about when it changes, and a query still running after 3 s */
        static uint64_t logged_room = UINT64_MAX, busy_logged_at;
        if (room != logged_room) {
            logged_room = room;
            printf("Ranked clock: asking about room %llu (host %d)\n", (unsigned long long)room, runtime_room_mode_following() ? 0 : 1);
        }
        if (__atomic_load_n(&room_alive_busy, __ATOMIC_SEQ_CST) && now > room_alive_after + 2000 &&
            now - busy_logged_at > 3000) {
            busy_logged_at = now;
            printf("Ranked clock: room %llu query still running %llu ms after it started\n", (unsigned long long)room,
                   (unsigned long long)(now - (room_alive_after - 1000)));
        }
    }
    if (gone != room) poll_room_alive(room, now);
    if (!room || gone != room) return;   /* an answer about an older room is dropped */
    /* The server dissolved this found room (short-handed / room-short from another player).
     * Tell every client still in the found room before dumping them back to ModeSelect, so no
     * player is left confused on the unranked screen without a notice. Ranked only: classic
     * rooms dissolve for other reasons and keep their own flow. */
    pthread_mutex_lock(&mode_lock);
    const int ranked = confirmed_ranked || joined_ranked || search_is_ranked;
    pthread_mutex_unlock(&mode_lock);
    if (ranked) notice("Not enough players to start Ranked. Back to queue.");
    fprintf(stderr, "Public matchmaking: found room %llu gone, returning to ModeSelect\n",
            (unsigned long long)room);
    runtime_public_itemless_abort_queue();
}

static void ranked_preflight(void);
static int party_member_ids(char *ids, size_t size);

void runtime_public_itemless_update(void) {
    members_tick();
    ranked_preflight();
    runtime_party_server_tick(); /* poll owner changes on lobby frames; race frames use note_frame */
    update_lobby_connection();
    update_search_clock();
    pthread_mutex_lock(&mode_lock);
    const int returned=party_return_check==2;
    if (returned) party_return_check=0;
    pthread_mutex_unlock(&mode_lock);
    if (returned) { return_to_party_menu(); return; }
    check_found_room_alive();
    do_queue_abort();
    /* the content notice was dismissed: out of the refused search, back to the lobby (start_search) */
    if (__atomic_load_n(&content_return, __ATOMIC_SEQ_CST) == 2) {
        __atomic_store_n(&content_return, 0, __ATOMIC_SEQ_CST);
        fprintf(stderr, "Public ITEMLESS: content notice dismissed: back to the lobby\n");
        if (network_manager && *network_manager) {
            native_stop_matchmaking(*network_manager);
            native_return_to_lobby();
        }
        return;
    }
    pthread_mutex_lock(&mode_lock);
    const int ranked=confirmed_ranked || (matchmaking_search && ranked_selected);
    const int leave_room=confirmed_ranked!=0;
    pthread_mutex_unlock(&mode_lock);
    if (ranked && ranked_party_size()>4) {
        pthread_mutex_lock(&mode_lock);search_timeout_pending=2;pthread_mutex_unlock(&mode_lock);
    }
    pthread_mutex_lock(&mode_lock);
    const int cancelled=search_timeout_pending;
    pthread_mutex_unlock(&mode_lock);
    if (cancelled) {
        /* Followers wait for the host's native party notification instead of leaving independently. */
        if (runtime_room_mode_following()) return;
        const int timeout_return = cancelled == 1;
        if (timeout_return) __atomic_store_n(&queue_return_pending,1,__ATOMIC_SEQ_CST);
        if (runtime_online_mode_matchmaking_cancel(leave_room)) {
            runtime_public_itemless_reset();
            runtime_public_itemless_party_ready();
            if (cancelled==2 && !runtime_room_mode_following())
                notice("Ranked supports at most 4 party members. Ranked was cancelled because another player joined. Your party stays together; use Public Matchmaking for up to 8.");
        } else if (timeout_return) {
            __atomic_store_n(&queue_return_pending,0,__ATOMIC_SEQ_CST);
        }
        return;
    }
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
    snprintf(target, sizeof target, "/ryunet/itemless-client?name=%s&version=1&nonce=%016" PRIx64 "&cup=1&teams=1&difficulty=1&collision_rules=1&ranked=2&release=%s&device=%s", name, token, SACREDFIRE_RELEASE, ranked_device());
    int status = runtime_ryunet_request("POST", target, body, sizeof body);
    int accepted = status == 200 && !strcmp(body, "ok");
    pthread_mutex_lock(&mode_lock);
    if (accepted && registration_nonce == token) allow_party = 1;
    pthread_mutex_unlock(&mode_lock);
    return accepted;
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

static char ranked_target[512]; /* the last Ranked party check target */

static int ranked_party_check(const char *doc) {
    const char *members = runtime_json_field(doc, "members");
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
    snprintf(ranked_target, sizeof ranked_target, "%s", target);
    /* No wait here: this runs on the game's thread, inside startSearch. The 50 ms poll loop that
     * used to wait for the answer froze the frame where Play started the search for 1.5 s
     * (log 20/11 13:03: 1522/1623/1521 ms, guest CPU 3-4 ms). ranked_preflight keeps the answer
     * cached while the lobby is open, so the press normally finds it here. */
    const int status = compatibility_check(target, body, sizeof body);
    if (status == -1) return -1;
    if (status != 200 || strcmp(body, "ok")) {
        reject_search(status == 409 ? body : "Cannot verify Ranked server and party compatibility. Please retry.");
        return 0;
    }
    return 1;
}

/* Keep the Ranked party answer warm while Ranked is the chosen mode, from the ModeSelect screen
 * on. The check only runs on a thread of its own; startSearch reads whatever answer is there and
 * otherwise lets the server's own startSearch validate the party. */
static void ranked_preflight(void) {
    if (!ranked_selected || matchmaking_search || confirmed_ranked) return;
    if (runtime_public_match_active() || !itemless_client_available()) return;
    char body[512], target[512], ids[192];
    const char *name = runtime_np_get_online_name();
    if (!name || !*name || !party_member_ids(ids, sizeof ids)) return;
    snprintf(target, sizeof target, "/ryunet/ranked-party?name=%s&members=%s&mode=%d",
             name, ids, selected);
    (void)compatibility_check(target, body, sizeof body);
    if (ranked_target[0] && strcmp(ranked_target, target))
        (void)compatibility_check(ranked_target, body, sizeof body);
}

/* Check before private Ready/Start can run its VSC side effects.
 * Read the same participant IDs used by the native manager. */
static _Thread_local const char *dispatch_approved_party;
static _Thread_local char checked_party[512];
/* The account ids the native search document carries: the primary local account, then the party's
 * remote members (a5d286 / a5d329 / a5d51e). The login gate and the Ranked party check key on
 * this same list, so both build it here. */
static int party_member_ids(char *ids, size_t size) {
    ids[0] = 0;
    void *manager = participants ? *participants : NULL;
    void *list = manager ? *(void **)((char *)manager + 0x10) : NULL;
    int count = list ? *(int *)((char *)list + 0xc) : 0;
    void **items = list ? *(void ***)((char *)list + 0x20) : NULL;
    uint64_t seen[8];
    int total = 0;
    size_t length = 0;
    if (!items || count < 1 || count > 8) return 0;
    /* Native search constructor (a5d286) takes the primary local account. */
    for (int i = 0; i < count; ++i) {
        if (!items[i]) return 0;
        unsigned kind;
        memcpy(&kind, (char *)items[i] + 0x20, sizeof kind);
        if (kind > 7) continue;
        memcpy(&seen[total++], (char *)items[i] + 0xa8, sizeof seen[0]);
        break;
    }
    if (!total || !seen[0]) return 0;
    /* a5d329 / a5d51e append remote party accounts, not local participants. */
    void *party = party_manager ? *party_manager : NULL;
    void *roster = party ? *(void **)((char *)party + 0x30) : NULL;
    int remote_count = roster ? *(int *)((char *)roster + 0xc) : -1;
    void **remote = roster ? *(void ***)((char *)roster + 0x20) : NULL;
    if (remote_count < 0 || remote_count > 8 || (remote_count && !remote)) return 0;
    for (int i = 0; i < remote_count; ++i) {
        if (!remote[i]) return 0;
        if (*((unsigned char *)remote[i] + 0x2c)) continue;
        uint64_t pid;
        memcpy(&pid, (char *)remote[i] + 0x18, sizeof pid);
        if (!pid || total == 8) return 0;
        for (int j = 0; j < total; ++j) if (seen[j] == pid) return 0;
        seen[total++] = pid;
    }
    for (int i = 0; i < total; ++i)
        length += (size_t)snprintf(ids + length, size - length,
                                  "%s%" PRIu64, i ? "," : "", seen[i]);
    return length < size;
}

static int early_party_check(const char *feature, int notify) {
    char ids[192] = "", target[512], body[512] = "";
    checked_party[0]=0;
    const char *online_name=runtime_np_get_online_name();
    int total = 1;
    if (!itemless_client_available() || !online_name || !*online_name ||
        !party_member_ids(ids, sizeof ids))
        goto unavailable;
    for (const char *at = ids; *at; ++at) if (*at == ',') ++total;
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
struct GuiFocusInputEvent {
    void *vtable;
    uint64_t flags;
    void *sender;
    unsigned signal;
    unsigned char tail[12];
};
static void *(ABI *native_deferred_event_create)(void *, void *, int);
static void (ABI *native_deferred_event_release)(void *);
static struct PendingPrivate {
    void *target;
    struct GuiFocusInputEvent *event;
    void (ABI *resume)(void *);
    uint64_t generation, deadline;
    int ranked;
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
    if (cancelled.event) native_deferred_event_release(cancelled.event);
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
        typedef void *(ABI *EventType)(void *);
        EventType type = (*(EventType **)event)[0];
        pending_private.event = native_deferred_event_create(type(event), NULL, 0);
        if (!pending_private.event) {
            clear_private_pending();
            notice("Cannot prepare the compatibility check. Please try again.");
            return;
        }
        /* Keep the factory-owned identity; only copy the input payload. */
        memcpy(&pending_private.event->sender, (char *)event + 0x10,
               sizeof *pending_private.event - 0x10);
        void *sender=pending_private.event->sender;
        if (sender) __atomic_fetch_add((unsigned *)sender,1,__ATOMIC_SEQ_CST);
    }
    pending_private.resume=resume;
    pending_private.generation=__atomic_load_n(&session_generation,__ATOMIC_SEQ_CST);
    pending_private.deadline=check_clock()+15000;
    snprintf(pending_private.party,sizeof pending_private.party,"%s",checked_party);
    fputs("Lobby entry: waiting for server check\n",stderr);
}
void runtime_private_party_defer_start(void *self, void (ABI *resume)(void *)) {
    defer_private(self,NULL,resume);
}

/* Returns 1 after dispatch: callers must not touch the old scene afterward. */
int runtime_private_party_update(void) {
    const char *feature=runtime_online_mode_in_private() ? private_feature() : NULL;
    if (!pending_private.ranked && !feature) { clear_private_pending(); return 0; }
    if (!pending_private.target) { (void)early_party_check(feature,0); return 0; }
    void *target=resolve_private_target(&pending_private.target);
    if (!target || pending_private.generation != __atomic_load_n(&session_generation,__ATOMIC_SEQ_CST)) {
        clear_private_pending(); return 0;
    }
    if (pending_private.ranked && (!ranked_selected || runtime_online_mode_in_private())) {
        clear_private_pending(); return 0;
    }
    int status=pending_private.ranked ? runtime_ranked_status(0) : early_party_check(feature,0);
    if (!pending_private.ranked && strcmp(pending_private.party,checked_party)) { clear_private_pending(); return 0; }
    if (status == -1 && check_clock()<pending_private.deadline) return 0;
    if (status != 1) {
        const int ranked = pending_private.ranked;
        clear_private_pending();
        if (status == -1) notice(ranked ? "Ranked status check timed out. Please try again." :
            "Player compatibility check timed out. Please try again.");
        else if (feature) (void)early_party_check(feature,1);
        return 0;
    }
    if (pending_private.ranked && (runtime_ranked_account() == 0 || !strcmp(runtime_online_content(), "modified"))) {
        clear_private_pending();
        if (ranked_account_allowed()) (void)public_content_allowed();     /* the notice */
        return 0;
    }
    struct PendingPrivate ready=pending_private;
    memset(&pending_private,0,sizeof pending_private);
    const char *previous=dispatch_approved_party;
    dispatch_approved_party=ready.ranked ? previous : ready.party;
    if (ready.resume) ready.resume(target);
    else native_placeable_event(target,ready.event);
    dispatch_approved_party=previous;
    release_handle(&ready.target);
    if (ready.event) native_deferred_event_release(ready.event);
    return 1;
}

static const char *meta_name(void *object) {
    if (!object) return NULL;
    typedef void *(ABI *GetType)(void *);
    GetType type = (*(GetType **)object)[0];
    void *meta = type(object);
    return meta ? *(const char **)((char *)meta + 0x10) : NULL;
}
static const char *vsc_graph_name(void *item) {
    const char *name = meta_name(item);
    if (name && !strcmp(name, "igGuiVscBehavior")) {
        void *graph = *(void **)((char *)item + 0x20);
        name = graph ? *(const char **)((char *)graph + 0x10) : NULL;
    }
    return name;
}
/* The navigation item resolves the shared prefab's label placeable at +0xf0 ('Text_LevelListName',
 * the RNEX reference); the button's own tree has no children (the visuals live in the shared
 * comp instance). Set the label on it and on the focused variant inside the same comp. */
static void menu_text_walk(void *node, int depth, const char *label) {
    if (!node || depth > 8) return;
    const char *name = *(const char **)((char *)node + 0x28);
    if (name && (!strcmp(name, "[LevelListName]") ||
                 !strcmp(name, "LevelListName_focused")))
        runtime_online_mode_set_text(node, label);
    void *list = *(void **)((char *)node + 0x48);
    int count = list ? *(int *)((char *)list + 0xc) : 0;
    void **children = list ? *(void ***)((char *)list + 0x20) : NULL;
    for (int i = 0; children && i < count && i < 40; ++i)
        menu_text_walk(children[i], depth + 1, label);
}
static void menu_label(void *button, const char *label) {
    void *list = *(void **)((char *)button + 0xe8);
    int count = list ? *(int *)((char *)list + 0xc) : 0;
    void **items = list ? *(void ***)((char *)list + 0x20) : NULL;
    for (int i = 0; items && i < count && i < 16; ++i) {
        const char *name = meta_name(items[i]);
        if (!name || strcmp(name, "CGuiBehaviorOctaneNavigationItem")) continue;
        void *text = *(void **)((char *)items[i] + 0xf0);
        if (!text) continue;
        menu_text_walk(text, 0, label);
        menu_text_walk(*(void **)((char *)text + 0x40), 0, label);
    }
}

static void ranked_gate_walk(void *node, int ranked, int state, int depth) {
    if (!node || depth > 12) return;
    void *list = *(void **)((char *)node + 0xe8);
    int count = list ? *(int *)((char *)list + 0xc) : 0;
    void **items = list ? *(void ***)((char *)list + 0x20) : NULL;
    for (int i = 0; items && i < count && i < 32; ++i) {
        const char *name = vsc_graph_name(items[i]);
        if (!name || strcmp(name, "Scripts.Graph.Octane_Lobby_ModeSelect_GoToLobbyButton")) continue;
        /* Keep focus/navigation available; only Confirm is gated. */
        const float alpha = ranked && state != 1 ? 0.45f : 1.0f;
        memcpy((char *)node + 0xe0, &alpha, sizeof alpha);
        if (ranked)
            menu_label(node, "GO TO LOBBY");
    }
    list = *(void **)((char *)node + 0x48);
    count = list ? *(int *)((char *)list + 0xc) : 0;
    items = list ? *(void ***)((char *)list + 0x20) : NULL;
    for (int i = 0; items && i < count && i < 64; ++i)
        ranked_gate_walk(items[i], ranked, state, depth + 1);
}

void runtime_ranked_gate_update(void *root, int ranked) {
    int state = ranked ? runtime_ranked_status(0) : 1;
    if (ranked && state == 1 && runtime_ranked_account() == 0) state = 0;   /* no linked account: dimmed */
    ranked_gate_walk(root, ranked, state, 0);
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

static void *return_button_in(void *node, const char *name, int depth);

static void (ABI *native_hof_focus)(void *, void *, int);
void runtime_hof_focus_list(void *placeable) {
    void *project = placeable ? *(void **)((char *)placeable + 0x20) : NULL;
    if (!project) return;
    void *root = runtime_online_mode_content(*(void **)((char *)project + 0x10));
    void *focused = *(void **)((char *)project + 0x50);
    const char *tag = focused ? *(const char **)((char *)focused + 0x28) : NULL;
    int in_list = tag && !strncmp(tag, "OCTR_HOF_ROW_", 13);
    void *target = return_button_in(root, in_list ? "OCTR_HOF_MODE" : "OCTR_HOF_ROW_0", 0);
    if (target) {
        native_hof_focus(project, target, 1);
        runtime_online_mode_hof_legend(project);
    }
}

void runtime_hof_native_ready(void *project) {
    if (!project) return;
    void *root = runtime_online_mode_content(*(void **)((char *)project + 0x10));
    const char *tags[] = {"OCTR_HOF_MODE", "OCTR_HOF_POPULATION", "OCTR_HOF_SEASON", "OCTR_HOF_LIST"};
    void *controls[4];
    for (unsigned i = 0; i < 4; ++i) controls[i] = return_button_in(root, tags[i], 0);
    for (unsigned i = 0; i < 4; ++i) {
        void *list = controls[i] ? *(void **)((char *)controls[i] + 0xe8) : NULL;
        void **items = list && *(int *)((char *)list + 12) > 0 ?
            *(void ***)((char *)list + 0x20) : NULL;
        if (!items || !items[0]) continue;
        /* Native selectors normally populate these links from Time Trial data. */
        *(void **)((char *)items[0] + 0x110) = i ? controls[i - 1] : NULL;
        *(void **)((char *)items[0] + 0x118) = i + 1 < 3 ? controls[i + 1] : NULL;
    }
    void *rows[8];
    for (unsigned i = 0; i < 8; ++i) {
        char tag[32]; snprintf(tag, sizeof tag, "OCTR_HOF_ROW_%u", i);
        rows[i] = return_button_in(root, tag, 0);
    }
    for (unsigned i = 0; i < 8; ++i) {
        void *list = rows[i] ? *(void **)((char *)rows[i] + 0xe8) : NULL;
        void **items = list && *(int *)((char *)list + 12) > 0 ?
            *(void ***)((char *)list + 0x20) : NULL;
        if (!items || !items[0]) continue;
        *(void **)((char *)items[0] + 0x110) = i ? rows[i - 1] : NULL;
        *(void **)((char *)items[0] + 0x118) = i + 1 < 8 ? rows[i + 1] : NULL;
    }
    unsigned char *event = native_leaderboard_ready(NULL);
    if (!event) return;
    /* HOF owns loading/empty labels; native HasData reveals the selectors and list. */
    event[0x18] = 0;
    typedef void (ABI *Dispatch)(void *, void *);
    Dispatch dispatch = *(Dispatch *)((char *)*(void **)project + 0xb0);
    dispatch(project, event);
    native_release_object(event);
    runtime_online_mode_hof_legend(project);
    puts("HOF: dispatched native LeaderboardReady");
}

/* MainMenu_Base_ButtonIndex remains 0 for clones; preserve their native focus target. */
static int online_return_button;

/* ---- Join from Discord: a Ranked room still filling (the server's ranked_lfg.go) -----------------
 * Its "Join" button opens sacredfire://ranked?mode=N; the launcher writes ryunet_ranked.txt
 * ("mode=N", "time=<unix>") next to bbport.ini, as ryunet_join.txt for invites. Picked up here at
 * most once a second, on the game's thread, and removed. Then the online menu's focus lands on
 * Ranked with a notice once, and its Mode Select opens on that mode (0 Normal, 1 Itemless): the
 * player confirms, and the server puts them in the room that waits. Kept for 10 minutes. */
static int ranked_request_mode = -1, ranked_request_notice, ranked_request_pending;
static uint64_t ranked_request_until;
/* The announced room ("room=<id>", 0 for a mode's queue): sent with the next Ranked search of that
 * mode as octr_ranked.room, which the server places first (ranked_lfg.go, ctr_async.go place). */
static uint64_t ranked_request_room;

/* The same request straight from the Discord bot's "Unirse" button (the server's ranked_join.go): the
 * bot tells the server which player pressed it, and the game asks every 3 seconds on a thread of its
 * own, with no browser. "ok\nmode=N\nroom=R" once, else "none". */
static int bot_join_busy, bot_join_ready, bot_join_mode;
static uint64_t bot_join_room, bot_join_next;

static void *bot_join_worker(void *unused) {
    (void)unused;
    const char *me = runtime_np_get_online_name();
    char target[256], body[128] = "";
    if (me && *me) {
        snprintf(target, sizeof target, "/ryunet/ranked-join?name=%s", me);
        if (runtime_ryunet_request("GET", target, body, sizeof body) == 200 && !strncmp(body, "ok\n", 3)) {
            const char *mode = strstr(body, "mode="), *room = strstr(body, "room=");
            if (mode && room) {
                __atomic_store_n(&bot_join_mode, atoi(mode + 5), __ATOMIC_SEQ_CST);
                __atomic_store_n(&bot_join_room, strtoull(room + 5, NULL, 10), __ATOMIC_SEQ_CST);
                __atomic_store_n(&bot_join_ready, 1, __ATOMIC_SEQ_CST);
            }
        }
    }
    __atomic_store_n(&bot_join_busy, 0, __ATOMIC_SEQ_CST);
    return NULL;
}

static void ranked_request_apply(int mode, uint64_t room, uint64_t now, const char *from) {
    __atomic_store_n(&ranked_request_mode, mode, __ATOMIC_SEQ_CST);
    __atomic_store_n(&ranked_request_pending, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&ranked_request_notice, 1, __ATOMIC_SEQ_CST);
    ranked_request_until = now + 600000;
    __atomic_store_n(&ranked_request_room, room, __ATOMIC_SEQ_CST);
    online_return_button = 1;   /* the online menu opens with Ranked focused */
    printf("Ranked join: Discord asks for Ranked %s (room %llu, %s)\n", mode ? "Itemless" : "Normal",
           (unsigned long long)room, from);
}

void runtime_public_itemless_poll_ranked_request(void) {
    static uint64_t next_check;
    const uint64_t now = check_clock();
    if (__atomic_exchange_n(&bot_join_ready, 0, __ATOMIC_SEQ_CST)) {
        const int mode = __atomic_load_n(&bot_join_mode, __ATOMIC_SEQ_CST);
        if (mode == 0 || mode == 1)
            ranked_request_apply(mode, __atomic_load_n(&bot_join_room, __ATOMIC_SEQ_CST), now, "the bot's button");
    }
    /* Not while searching or in a room (public, Ranked or private): a join from the bot could not be taken
     * there anyway, and these asks were ~90 a second on the server at 140 players (21/11). */
    const int busy_online = runtime_public_match_active() || runtime_online_mode_room_players() > 0;
    if (busy_online) bot_join_next = now + 3000;
    if (!busy_online && now >= bot_join_next && !__atomic_exchange_n(&bot_join_busy, 1, __ATOMIC_SEQ_CST)) {
        bot_join_next = now + 3000;
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thread, &attr, bot_join_worker, NULL)) __atomic_store_n(&bot_join_busy, 0, __ATOMIC_SEQ_CST);
        pthread_attr_destroy(&attr);
    }
    if (now < next_check) return;
    next_check = now + 1000;
    char path[600];
    const char *cfg = getenv("BB_LAUNCHER_CONFIG");   /* mod sessions redirect BB_CONFIG */
    if (!cfg || !*cfg) cfg = getenv("BB_CONFIG");
    const char *slash = cfg ? strrchr(cfg, '/') : NULL;
    const char *bslash = cfg ? strrchr(cfg, '\\') : NULL;
    if (bslash && (!slash || bslash > slash)) slash = bslash;
    if (slash) snprintf(path, sizeof path, "%.*s/ryunet_ranked.txt", (int)(slash - cfg), cfg);
    else snprintf(path, sizeof path, "ryunet_ranked.txt");
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    int mode = -1;
    long long written = 0;
    unsigned long long room = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "mode=", 5)) mode = atoi(line + 5);
        else if (!strncmp(line, "time=", 5)) written = atoll(line + 5);
        else if (!strncmp(line, "room=", 5)) room = strtoull(line + 5, NULL, 10);
    }
    fclose(f);
    remove(path);
    const long long wall = (long long)time(NULL);
    /* A stale file (a game that never started) must not steer the menus much later. */
    if ((mode != 0 && mode != 1) || written < wall - 300 || written > wall + 60) {
        printf("Ranked join: ignored a request file (mode=%d age=%llds)\n", mode, wall - written);
        return;
    }
    ranked_request_apply(mode, (uint64_t)room, now, "the join page");
}

/* Mode Select (runtime_online_mode.c), Ranked chosen: the mode the Discord join asked for, once; -1 none. */
int runtime_public_itemless_ranked_request_take(void) {
    if (!__atomic_exchange_n(&ranked_request_pending, 0, __ATOMIC_SEQ_CST)) return -1;
    if (check_clock() > ranked_request_until) return -1;
    __atomic_store_n(&ranked_request_notice, 0, __ATOMIC_SEQ_CST);
    return __atomic_load_n(&ranked_request_mode, __ATOMIC_SEQ_CST);
}

static void *return_button_in(void *node, const char *name, int depth) {
    if (!node || depth > 8) return NULL;
    const char *tag = *(const char **)((char *)node + 0x28);
    if (tag && !strcmp(tag, name)) return node;
    void *list = *(void **)((char *)node + 0x48);
    int count = list ? *(int *)((char *)list + 12) : 0;
    void **children = list ? *(void ***)((char *)list + 0x20) : NULL;
    for (int i = 0; children && i < count; ++i) {
        void *found = return_button_in(children[i], name, depth + 1);
        if (found) return found;
    }
    return return_button_in(runtime_online_mode_content(node), name, depth + 1);
}
/* Each online lobby screen this player focuses, once per name, so the next run shows the screens
 * the guest really visited next to the room decision below (2026-11-18). */
static void note_room_screen(const char *name) {
    static char seen[12][48];
    static int count;
    if (!name || strncmp(name, "Scripts.Graph.", 14)) return;
    for (int i = 0; i < count; ++i)
        if (!strcmp(seen[i], name)) return;
    if (count >= 12) return;
    snprintf(seen[count++], sizeof seen[0], "%s", name);
    printf("Online party: focus graph %.48s following=%d\n", name, runtime_room_mode_following());
}
/* The room's screen: the dump's Octane_Lobby_PrivateGame_en.lng labels this graph's two buttons
 * "Start Game" (host) and "Ready" (guest), so this graph is the room both roles sit in
 * (2026-11-18). Octane_Lobby_WaitingRoom is "Waiting on host..." and ModeSelect is the setup
 * screen before the room, which is where Back from the room lands. */
static int room_screen(const char *name) {
    return name && !strcmp(name, "Scripts.Graph.Octane_Lobby_PrivateGame_Root");
}
/* A screen change to ModeSelect/Main is only a room exit when the view BEFORE it was a room or
 * the mode-select screen. OnlineMode -> ModeSelect is forward navigation into a mode, not Back. */
static char last_lobby_graph[64];
static int lobby_graph_back(const char *now, const char *prev) {
    if (!now || !prev || !*prev) return 0;
    const int exit_screen = !strcmp(now, "Scripts.Graph.Octane_Lobby_ModeSelect_Root") ||
                            !strcmp(now, "Scripts.Graph.Octane_Lobby_Main_Root");
    if (!exit_screen) return 0;
    return !strcmp(prev, "Scripts.Graph.Octane_Lobby_PrivateGame_Root") ||
           !strcmp(prev, "Scripts.Graph.Octane_Lobby_Matchmaking_Root") ||
           !strcmp(prev, "Scripts.Graph.Octane_Lobby_WaitingRoom_Root") ||
           (!strcmp(now, "Scripts.Graph.Octane_Lobby_Main_Root") &&
            !strcmp(prev, "Scripts.Graph.Octane_Lobby_ModeSelect_Root"));
}
static void native_return_focus(void *node, int ancestors) {
    for (int depth = 0; node && depth < 16; ++depth) {
        void *list = *(void **)((char *)node + 0xe8);
        int count = list ? *(int *)((char *)list + 12) : 0;
        void **items = list ? *(void ***)((char *)list + 0x20) : NULL;
        for (int i = 0; items && i < count; ++i) {
            const char *name = vsc_graph_name(items[i]);
            const int was_in_room = guest_room_open;
            if (!ancestors) {
                note_room_screen(name);
                if (name && !strncmp(name, "Scripts.Graph.Octane_Lobby_", 27)) {
                    if (lobby_graph_back(name, last_lobby_graph)) notify_room_left();
                    snprintf(last_lobby_graph, sizeof last_lobby_graph, "%s", name);
                }
            }
            /* The room itself (host: "Start Game", guest: "Ready"). Focus inside it also comes
             * back to it when its quit dialog closes: that visit changes nothing. */
            if (!ancestors && !guest_room_open && room_screen(name)) {
                printf("Online party: in the room screen %.40s\n", name);
                runtime_public_itemless_guest_room(1);
            }
            if (!ancestors && name && !strcmp(name, "Scripts.Graph.Octane_Lobby_Matchmaking_Root")) {
                arm_party_return();
                return;
            }
            if (!ancestors && name && was_in_room &&
                !strcmp(name, "Scripts.Graph.Octane_Lobby_ModeSelect_Root")) {
                /* Back out of the room lands on its setup screen. Complete the same party return
                 * as every other room exit before this early return. */
                runtime_public_itemless_guest_room(0);
                runtime_public_itemless_menu_return();
                return;
            }
            if (!ancestors && name && (!strcmp(name, "Scripts.Graph.Octane_Lobby_ModeSelect_Root") ||
                                      !strcmp(name, "Scripts.Graph.Octane_Lobby_WaitingRoom_Root"))) {
                if (__atomic_load_n(&menu_party_returned,__ATOMIC_SEQ_CST)) return;
                /* ModeSelect appears before PrivateGame while entering, and after it on Back. */
                if (__atomic_load_n(&entry_private,__ATOMIC_SEQ_CST) &&
                    !__atomic_exchange_n(&private_lobby_open,0,__ATOMIC_SEQ_CST)) return;
                runtime_public_itemless_menu_return();
                return;
            }
            if (!name || strcmp(name, "Scripts.Graph.Octane_Lobby_Main_Root")) continue;
            __atomic_store_n(&private_lobby_open,0,__ATOMIC_SEQ_CST);
            if (!ancestors && !__atomic_load_n(&menu_party_returned,__ATOMIC_SEQ_CST)) {
                runtime_public_itemless_menu_return();
            }
            /* Back to the menus from the room without going through its screens: the guest still
             * has to leave the host's party (its Lobby Members row and the host's notice). */
            if (!ancestors) runtime_public_itemless_guest_room(0);
            typedef void *(ABI *GetType)(void *);
            void *meta = (*(GetType **)items[i])[0](items[i]);
            const char *names[] = {"Gui_Placeable_id_gto91nsu",
                "Gui_Placeable_id_rcxu6uoi", "Gui_Placeable_id_bdnprt14"};
            void **slots[3];
            for (unsigned j = 0; j < 3; ++j) {
                unsigned char *field = native_meta_field(meta, names[j]);
                if (!field) {
                    printf("Online focus: missing reflected field %s\n", names[j]);
                    return;
                }
                unsigned short offset;
                memcpy(&offset, field + 0x14, sizeof offset);
                slots[j] = (void **)((char *)items[i] + offset);
            }
            void *target = online_return_button ? return_button_in(node,
                online_return_button == 1 ? "OCTR_RANKED_BUTTON" : "OCTR_HALL_OF_FAME_BUTTON", 0) :
                resolve_private_target(slots[0]);
            if (!target) {
                printf("Online focus: selection=%d target absent under root=%p\n", online_return_button, node);
                return;
            }
            /* Use reflected native fields: loaded VSC instances do not use the IGZ layout. */
            for (unsigned j = 1; j < 3; ++j) {
                if (resolve_private_target(slots[j]) == target) continue;
                release_handle(slots[j]);
                native_make_handle(slots[j], target);
            }
            printf("Online focus: selection=%d root=%p native target=%p\n",
                   online_return_button, node, target);
            return;
        }
        if (!ancestors) return;
        node = *(void **)((char *)node + 0x40);
    }
    if (ancestors) printf("Online focus: selection=%d no Main_Root ancestor\n", online_return_button);
}

/* Ranked uses a cloned Matchmaking button and VSC state. */
void runtime_public_itemless_new_session(void) {
    runtime_public_itemless_reset();
    selected = ranked_selected = 0;
    clear_private_pending();
    runtime_public_itemless_party_ready();
}

static int ranked_pause_quit(void *self) {
    if (!self || !runtime_ranked_active()) return 0;
    void *list = *(void **)((char *)self + 0xe8);
    int count = list ? *(int *)((char *)list + 12) : 0;
    void **items = list ? *(void ***)((char *)list + 0x20) : NULL;
    for (int i = 0; items && i < count; ++i) {
        const char *name = vsc_graph_name(items[i]);
        if (name && !strcmp(name, "Scripts.Graph.PauseMenu_Octane_QuitButton")) return 1;
    }
    return 0;
}
static void ABI placeable_event(void *self, void *event) {
    if (ranked_pause_quit(self)) {
        const char *type = event ? meta_name(event) : NULL;
        if (type && (!strcmp(type, "igGuiEventFocusInputPressed") ||
                     !strcmp(type, "igGuiEventFocusInputRepeated"))) return;
        const int teardown = type && (strstr(type, "Unload") || strstr(type, "Uninitialize") || strstr(type, "LoseFocus"));
        native_placeable_event(self, event);
        if (!teardown) runtime_online_mode_show_placeable(self, 0);
        return;
    }
    char approval[sizeof checked_party] = "";
    int private_room = runtime_online_mode_in_private();
    const char *feature = private_room ? private_feature() : NULL;
    int private_gate = private_room && (runtime_speed_mode_enabled() || feature);
    const char *tag = self ? *(const char **)((char *)self + 0x28) : NULL;
    int ranked_button = tag && !strcmp(tag, "OCTR_RANKED_BUTTON");
    int hall_of_fame_button = tag && !strcmp(tag, "OCTR_HALL_OF_FAME_BUTTON");
    int signal = 0;
    if (self && event) {
        const char *type = meta_name(event);
        const int gain = type && (!strcmp(type, "igGuiEventProjectGainFocus") ||
                                  !strcmp(type, "igGuiEventPlaceableGainFocus"));
        if (gain && (ranked_button || hall_of_fame_button)) runtime_online_mode_selector_left();
        if (gain) native_return_focus(self, 0);
        /* The Discord announcement's Join (ryunet_ranked.txt): once, where the focus was put on Ranked. */
        if (gain && ranked_button && __atomic_exchange_n(&ranked_request_notice, 0, __ATOMIC_SEQ_CST)) {
            const int mode = __atomic_load_n(&ranked_request_mode, __ATOMIC_SEQ_CST);
            notice(mode == 1 ? "A Ranked Itemless room is waiting for players. Choose Ranked and then Itemless to join it."
                             : "A Ranked Normal room is waiting for players. Choose Ranked and then Normal to join it.");
        }
        /* the content notice's Continue (Confirm 8) or Back (9): leave the refused search */
        if (__atomic_load_n(&content_return, __ATOMIC_SEQ_CST) == 1 && type &&
            (!strcmp(type, "igGuiEventFocusInputPressed") || !strcmp(type, "igGuiEventProjectInputPressed"))) {
            int pressed = 0;
            memcpy(&pressed, (char *)event + 0x18, sizeof pressed);
            if (pressed == 8 || pressed == 9) __atomic_store_n(&content_return, 2, __ATOMIC_SEQ_CST);
        }
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
        const int confirm = type && !strcmp(type, "igGuiEventFocusInputPressed") && signal == 8;
        if (confirm && ranked_button && !ranked_entry_allowed()) return;
        if (type && (((!strcmp(type, "igGuiEventFocusInputPressed") ||
                      !strcmp(type, "igGuiEventFocusInputRepeated")) && signal != 10) ||
                     (!strcmp(type, "igGuiEventProjectInputPressed") && signal == 10 &&
                      tag && !strcmp(tag, "OCTR_HOF_ROOT"))) &&
            runtime_hof_input(self, (unsigned)signal)) return;
        if (tag && !strcmp(tag, "OCTR_HOF_ROOT") && type && !strcmp(type, "igGuiEventProjectUnload")) runtime_hof_close();
        const int direction = type && (!strcmp(type, "igGuiEventFocusInputPressed") ||
                                       !strcmp(type, "igGuiEventFocusInputRepeated")) && signal < 8;
        if (confirm || gain || direction) {
            if (hall_of_fame_button) {
                if (confirm) { ranked_selected = 0; runtime_hof_open(); }
            }
            void *list = *(void **)((char *)self + 0xe8);
            int count = list ? *(int *)((char *)list + 0xc) : 0;
            void **items = list ? *(void ***)((char *)list + 0x20) : NULL;
            for (int i = 0; items && i < count; ++i) {
                const char *name = vsc_graph_name(items[i]);
                if (gain && !ranked_button && !hall_of_fame_button && name &&
                    !strcmp(name, "Scripts.Graph.Octane_Lobby_Main_MatchmakingButton"))
                    runtime_online_mode_selector_left();
                if (confirm && name && (!strcmp(name, "Scripts.Graph.Octane_Lobby_Main_MatchmakingButton") ||
                    !strcmp(name, "Scripts.Graph.Octane_Lobby_Main_PrivateMatchButton"))) {
                    runtime_online_mode_new_session();
                    runtime_room_mode_hosting();
                }
                if (confirm && name && !strcmp(name, "Scripts.Graph.Octane_Lobby_Main_MatchmakingButton")) {
                    ranked_selected = ranked_button;
                    joined_ranked = ranked_button; /* a fresh user choice wins over a stale snapshot */
                    online_return_button = hall_of_fame_button ? 2 : ranked_button;
                    native_return_focus(self, 1);
                }
                if (name && !strcmp(name, "Scripts.Graph.Octane_Lobby_Main_PrivateMatchButton")) {
                    if (confirm) { ranked_selected = 0; joined_ranked = 0; }
                    if (gain) runtime_online_mode_selector_left();
                }
                if (confirm && private_room && name &&
                    !strcmp(name, "Scripts.Graph.Octane_Lobby_ModeSelect_GoToLobbyButton")) {
                    arm_private_room();
                    __atomic_store_n(&private_lobby_open,1,__ATOMIC_SEQ_CST);
                }
                if (confirm && ranked_selected && !private_room && name &&
                    !strcmp(name, "Scripts.Graph.Octane_Lobby_ModeSelect_GoToLobbyButton")) {
                    if (!ranked_entry_allowed()) return;
                    const int state = runtime_ranked_status(1);
                    if (state == -1) {
                        snprintf(checked_party, sizeof checked_party, "ranked-status");
                        defer_private(self, event, NULL);
                        pending_private.ranked = pending_private.target != NULL;
                    }
                    if (state != 1) return;
                    if (!ranked_account_allowed() || !public_content_allowed()) return;
                }
                if (confirm && private_gate && name && !strcmp(name, "Scripts.Graph.Octane_Lobby_PrivateGame_ButtonReady")) {
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
    /* Found rooms used to block Back (no leaving mid-matchmaking); players may cancel or go back
     * again (20/11): the server handles a departure (room-left, found-room rule). */
    const char *previous = dispatch_approved_party;
    if (approval[0]) dispatch_approved_party = approval;
    native_placeable_event(self, event);
    /* The label override touches the shared comp instance through the nav's resolved text
     * reference: safe while the scene is live (focus and input events), but the LoseFocus
     * events also fire while the project is tearing down and the comp may already be gone. */
    if ((ranked_button || hall_of_fame_button) && event) {
        const char *event_type = meta_name(event);
        if (!event_type || (strcmp(event_type, "igGuiEventPlaceableLoseFocus") &&
                            strcmp(event_type, "igGuiEventProjectLoseFocus"))) {
            menu_label(self, ranked_button ? "RANKED" : "HALL OF FAME");
            if (getenv("BB_GUI_DUMP")) gui_dump(self);
        }
    }
    dispatch_approved_party = previous;
}

static char *extend(const char *raw, unsigned limit, uint64_t token, int mode, int ranked) {
    if (!raw || *raw != '{' || !limit || limit > 65536 || !token) return NULL;
    size_t n = strnlen(raw, limit);
    if (n == limit || runtime_json_field(raw, "octr_itemless") || runtime_json_field(raw, "octr_ranked")) return NULL;
    char *out = malloc(n + 480);
    if (out) {
        size_t used = 0;
        /* Joined from a Discord announcement: that room, once, for a search of its mode in time. */
        uint64_t room = 0;
        if (ranked && __atomic_load_n(&ranked_request_mode, __ATOMIC_SEQ_CST) == mode &&
            check_clock() <= ranked_request_until)
            room = __atomic_exchange_n(&ranked_request_room, 0, __ATOMIC_SEQ_CST);
        if (room) fprintf(stderr, "Ranked join: searching for the announced room %llu\n", (unsigned long long)room);
        if (ranked && room) used = (size_t)snprintf(out, n + 480,
            "{\"octr_ranked\":{\"version\":2,\"mode\":%d,\"nonce\":\"%016" PRIx64 "\",\"room\":%llu},",
            mode, token, (unsigned long long)room);
        else if (ranked) used = (size_t)snprintf(out, n + 480,
            "{\"octr_ranked\":{\"version\":2,\"mode\":%d,\"nonce\":\"%016" PRIx64 "\"},",
            mode, token);
        else { out[0] = '{'; out[1] = 0; used = 1; }
        if (mode || !ranked) used += (size_t)snprintf(out + used, n + 480 - used,
            "\"octr_itemless\":{\"version\":1,\"mode\":%d,\"ranked\":2,\"private_cup\":1,\"private_teams\":1,\"private_difficulty\":1,\"collision_rules\":1,\"release\":\"%s\",\"nonce\":\"%016" PRIx64 "\",\"device\":\"%s\"},",
            mode, SACREDFIRE_RELEASE, token, ranked_device());
        snprintf(out + used, n + 480 - used, "%s", raw + 1);
    }
    return out;
}

static void *ABI register_client(void **result, void *self, const char *raw, unsigned limit) {
    pthread_mutex_lock(&mode_lock);
    if (!requeued_room && !matchmaking_room) reset_locked();
    /* Leader searches snapshot the follower's capability before its native lobby reloads. */
    if (!registration_nonce && itemless_client_available()) registration_nonce = nonce();
    uint64_t token = itemless_client_available() ? registration_nonce : 0;
    pthread_mutex_unlock(&mode_lock);
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
/* The native party role and the other member's name (the host, when we are a guest). Used to
 * mirror a queueing party into a server-backed session like private rooms have. */
static int matchmaking_party_host(char host_name[24]) {
    host_name[0] = 0;
    unsigned char *party = party_manager ? *party_manager : NULL;
    unsigned char *session = party ? *(void **)(party + 0x18) : NULL;
    unsigned char *connection = session ? *(void **)(session + 0x10) : NULL;
    int role = 0;
    if (connection) memcpy(&role, connection + 0x118, sizeof role);
    void *roster = party ? *(void **)(party + 0x30) : NULL;
    int count = roster ? *(int *)((char *)roster + 12) : 0;
    void **items = roster ? *(void ***)((char *)roster + 0x20) : NULL;
    if (count < 0 || count > 8 || !items) count = 0;
    for (int i = 0; i < count; ++i) {
        if (!items[i] || *((unsigned char *)items[i] + 0x2c)) continue; /* skip the local player */
        const char *name = *(const char **)((char *)items[i] + 0x10);
        if (name && *name) { snprintf(host_name, 24, "%s", name); break; }
    }
    return role;
}
static void *ABI start_search(void **result, void *self, const char *raw, unsigned limit, void *response) {
    /* The host just left the queue. Native recreates the party and auto-issues start_search with
     * the guest's own (default Casual) mode. Swallow that one automatic search; do_queue_abort
     * handles the return to ModeSelect / Waiting on host. A real Play press later is not suppressed. */
    if (check_clock() < __atomic_load_n(&suppress_search_until, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&suppress_search_until, 0, __ATOMIC_SEQ_CST); /* swallow just that one */
        fprintf(stderr, "Public matchmaking: suppressed native auto-search after host left queue\n");
        *result = NULL;
        return result;
    }
    if (rejection[0]) { *result = NULL; return result; }
    /* any public search (Normal, Itemless, Ranked): character mods only (private rooms never search).
     * The notice first; its Continue (or Back) stops the search and goes back to the lobby
     * (placeable_event, then runtime_public_itemless_update). */
    if (!strcmp(runtime_online_content(), "modified")) {
        fprintf(stderr, "Public ITEMLESS: search refused: online content is modified (private rooms only)\n");
        notice(PUBLIC_CONTENT_NOTICE);
        __atomic_store_n(&content_return, 1, __ATOMIC_SEQ_CST);
        *result = NULL;
        return result;
    }
    /* A matchmaking search is never a private room: the party's room flag must not open the
     * private lobby (map selector) while the host waits for opponents (2026-11-17). */
    clear_room_private(party_manager ? *party_manager : NULL);
    /* Mirror the queueing party into a server session so a host quit is visible immediately,
     * exactly like a private room. No ctrRoom exists yet to carry that information. */
    char mm_host[24];
    const int mm_role = matchmaking_party_host(mm_host);
    runtime_party_server_matchmaking(mm_role == 1, mm_host);
    if (ranked_selected) {
        if (!ranked_entry_allowed()) { *result = NULL; return result; }
        if (runtime_ranked_status(0) != 1) { *result = NULL; return result; }
        /* A pending check never blocks or pops a notice: the search document carries the party,
         * and the server validates it in its own asynchronous startSearch, like the game does. */
        if (ranked_party_check(raw) == 0) { *result = NULL; return result; }
    }
    pthread_mutex_lock(&mode_lock);
    uint64_t previous_mark=public_search_since ? public_search_since : matchmaking_search ? search_mark : 0;
    int previous_elapsed=public_search_since ? 0 : search_elapsed_base;
    reset_locked();
    arm_party_return();
    search_mark=previous_mark ? previous_mark : check_clock();
    search_elapsed_base=previous_mark ? previous_elapsed : 0;
    search_is_ranked=!!ranked_selected;
    if (!selected && !ranked_selected) matchmaking_search=1;
    pthread_mutex_unlock(&mode_lock);
    fprintf(stderr, "Public matchmaking: starting %s %s search\n", ranked_selected ? "RANKED" : "CASUAL", selected ? "ITEMLESS" : "NORMAL");
    if (!selected && !ranked_selected) {
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
    /* Cancelling the old search can follow a successful join. Room departure has its own task. */
    pthread_mutex_lock(&mode_lock);
    if (!matchmaking_room && !requeued_room) reset_locked();
    pthread_mutex_unlock(&mode_lock);
    return native_cancel(result, self, id, response);
}
/* Tell RyuNET the instant a player exits a found matchmaking room. Without this the server
 * only learns on RyuNET connection close ~10s later and keeps a ghost member in the room.
 * The room id is STICKY: native Quit calls cancel_search/reset before menu_return, so
 * matchmaking_room is often already 0 by the time the leave hook runs. Both menu_return and
 * leave_lobby can fire for one exit; report once. */
static volatile int room_left_reported; /* reset on join, dedups the two leave hooks */
void runtime_public_itemless_prepare_host_quit(void) {
    const uint64_t room = __atomic_load_n(&left_room_pending, __ATOMIC_SEQ_CST);
    if (!room || room == prepromoted_room || !runtime_replay_race_active()) return;
    void *session = match_session_manager && *match_session_manager ? ptr(*match_session_manager,0x28) : NULL;
    void *connection = session ? ptr(session,0x10) : NULL;
    int role = 0;
    if (connection) memcpy(&role,(unsigned char *)connection+0x118,sizeof role);
    if (role != 1) return;
    const char *me = runtime_np_get_online_name();
    char target[256], body[64];
    snprintf(target,sizeof target,"/ryunet/room-promote?name=%s&room=%" PRIu64 "&auto=1",me ? me : "",room);
    const int status = runtime_ryunet_request("POST",target,body,sizeof body);
    if (status == 200) prepromoted_room = room;
    fprintf(stderr,"Matchmaking: room %" PRIu64 " pre-leave promotion status=%d\n",room,status);
}
void runtime_public_itemless_begin_race_quit(void) {
    __atomic_store_n(&local_race_quitting,1,__ATOMIC_RELEASE);
    runtime_public_itemless_prepare_host_quit();
}
static void notify_room_left(void) {
    const uint64_t room = __atomic_load_n(&left_room_pending, __ATOMIC_SEQ_CST);
    const int keep_party = !entry_private && __atomic_load_n(&party_at_entry,__ATOMIC_SEQ_CST) > 1;
    /* A queue exit can follow an earlier room exit. Keep its premade session
     * alive even when the native leave handler fires twice. */
    if (!room) {
        pthread_mutex_lock(&mode_lock);
        uint64_t previous = matchmaking_search ? requeued_room : 0;
        pthread_mutex_unlock(&mode_lock);
        if (previous) {
            char target[256], body[128];
            const char *me = runtime_np_get_online_name();
            snprintf(target,sizeof target,"/ryunet/room-left?name=%s&room=%" PRIu64 "&cancel_requeue=1",
                     me ? me : "",previous);
            runtime_ryunet_request("POST",target,body,sizeof body);
        }
        runtime_party_server_matchmaking_leave(keep_party);
        return;
    }
    if (__atomic_exchange_n(&room_left_reported, 1, __ATOMIC_SEQ_CST)) return;
    runtime_public_itemless_prepare_host_quit();
    __atomic_store_n(&left_room_pending, 0, __ATOMIC_SEQ_CST);
    const char *me = runtime_np_get_online_name();
    char target[512], body[64];
    snprintf(target, sizeof target, "/ryunet/room-left?name=%s&room=%llu",
             me ? me : "", (unsigned long long)room);
    const int status = runtime_ryunet_request("POST", target, body, sizeof body);
    if (status == 200) {
        /* The found room ended, not the menu party. Clear queue tracking even for solo entrants. */
        runtime_party_server_matchmaking_leave(1);
        __atomic_store_n(&party_exit_sent,1,__ATOMIC_SEQ_CST);
        send_confirmed_departure(room);
    }
    fprintf(stderr, "Online party: room-left room=%llu status=%d\n",
            (unsigned long long)room, status);
}
static void *ABI leave_lobby(void **result, void *self, uint64_t id) {
    pthread_mutex_lock(&mode_lock);
    const int stale = id && (id == requeued_room || (matchmaking_room && matchmaking_room != id));
    pthread_mutex_unlock(&mode_lock);
    if (stale) return native_leave(result, self, id);
    notify_room_left();
    /* This callback decodes the leave reply; scene teardown belongs to menu return. */
    void *value = native_leave(result, self, id);
    runtime_public_itemless_reset();
    return value;
}

/* Validate tokens against the authoritative confirmation document, never the host document. */
static int confirmation_mode(const char *confirmation_doc, const char *key, uint64_t room, uint64_t token, int mode) {
    const char *value = runtime_json_field(confirmation_doc, key);
    char prefix[80], wanted[17];
    snprintf(prefix, sizeof prefix, "\"2/%" PRIu64 "/%d/", room, mode);
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
static int accept_entry(void *owner, uint64_t room, const char *confirmation_doc, unsigned length, int entry) {
    if (!confirmation_doc || length > 65536 || strnlen(confirmation_doc, (size_t)length + 1) != length) {
        fprintf(stderr, "Public ITEMLESS: invalid confirmation length=%u room=%" PRIu64 "\n", length, room);
        return 0;
    }
    const int party_guest=runtime_room_mode_following();
    pthread_mutex_lock(&mode_lock);
    if (!searching && !matchmaking_search && !(allow_party && party_guest) &&
        !(matchmaking_room==room && matchmaking_owner==owner)) {
        pthread_mutex_unlock(&mode_lock);
        return 0; /* A cancelled host search must not rejoin through its registration nonce. */
    }
    if (entry && matchmaking_room && (matchmaking_room!=room ||
        (matchmaking_owner==owner && (entry==1 || matchmaking_created)))) {
        pthread_mutex_unlock(&mode_lock);
        return 0;
    }
    uint64_t token = searching ? request_nonce : allow_party && party_guest ? registration_nonce : 0;
    int wants_itemless = searching && selected;
    int wants_ranked = searching && ranked_selected;
    int itemless_advertised = runtime_json_field(confirmation_doc, "octr_itemless_v1") != NULL;
    int ranked_advertised = runtime_json_field(confirmation_doc, "octr_ranked_v1") != NULL;
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
        joined_ranked = ranked_advertised; /* sticky: survives reset, for takeover re-search */
        joined_itemless = confirmed_itemless; /* re-queue the same Classic/Ranked variant */
    } else if (confirmed_room == room || (!itemless_advertised && !ranked_advertised)) {
        confirmed_room = 0; room_owner = NULL;
        confirmed_itemless = confirmed_ranked = 0;
    }
    if (valid) {
        clear_room_private(party_manager ? *party_manager : NULL);
        const char *waiting = runtime_json_field(confirmation_doc, "octr_waiting_for_opponent");
        int count = 0;
        if (waiting && !strncmp(waiting,"true",4)) {
            const char *players = runtime_json_field(confirmation_doc, "octr_waiting_players");
            count = players ? atoi(players) : 1;
            if (count < 1 || count > 8) count = 1;
        }
        if (entry == 1) prepare_requeue_host = 0;
        int joining = !ranked_advertised && !runtime_replay_race_active() && !runtime_online_mode_race_running() &&
                      runtime_online_mode_room_players() < 2;
        __atomic_store_n(&awaiting_public_peer,joining,__ATOMIC_RELEASE);
        if ((!count && !joining) || ranked_advertised) public_search_since = 0;
        else if (!public_search_since || matchmaking_room != room) {
            uint64_t elapsed_ms = (uint64_t)search_elapsed_base * 1000;
            public_search_since = !matchmaking_room && search_mark > elapsed_ms ? search_mark - elapsed_ms : check_clock();
        }
        __atomic_store_n(&waiting_for_opponent, count, __ATOMIC_RELEASE);
        arm_party_return();
        if (matchmaking_room != room) {
            __atomic_store_n(&local_race_quitting,0,__ATOMIC_RELEASE);
            __atomic_store_n(&had_public_race,0,__ATOMIC_RELEASE);
        }
        matchmaking_room = room;
        __atomic_store_n(&left_room_pending, room, __ATOMIC_SEQ_CST); /* sticky across reset */
        __atomic_store_n(&room_left_reported, 0, __ATOMIC_SEQ_CST); /* new room: allow a leave notice */
        matchmaking_owner = owner;
        matchmaking_search = 0;
        if (entry==2) matchmaking_created=1;
    }
    pthread_mutex_unlock(&mode_lock);
    if (!valid) {
        reject_search("Matchmaking lobby confirmation failed. Please search again.");
        return 0;
    }
    return 1;
}
static void *ptr(const void *p, size_t offset) { void *v; memcpy(&v,(const char *)p+offset,8); return v; }
static uint64_t lobby_host_pid(const char *host) {
    const char *listen=runtime_json_field(host,"listen_server");
    if (!listen || *listen!='{') return 0;
    const char *end=strchr(listen,'}');
    char object[512];
    if (!end || (size_t)(end-listen+1)>=sizeof object) return 0;
    memcpy(object,listen,(size_t)(end-listen+1));
    object[end-listen+1]=0;
    const char *id=runtime_json_field(object,"host_player_id");
    return id ? strtoull(id,NULL,10) : 0;
}
static int accept(void *owner, uint64_t room, const char *doc, unsigned length) {
    return accept_entry(owner,room,doc,length,0);
}
static void ABI joined(void *self, uint64_t search, uint64_t room, const char *host, unsigned hn, const char *confirmation_doc, unsigned bn) {
    const uint64_t host_pid=lobby_host_pid(host);
    if (host_pid) race_lobby_host_pid=host_pid;
    if (!accept_entry(self, room, confirmation_doc, bn,1)) return;
    void *manager=match_session_manager ? *match_session_manager : NULL;
    if (manager && (ptr(manager,0x18) || ptr(manager,0x20))) {
        /* Join must reuse the session published by the pending native HostSession task. */
        struct PendingLobbyCreate *pending=host && hn<=65536 ? malloc(sizeof *pending+hn+bn+2) : NULL;
        if (!pending) { reject_search("Cannot prepare matchmaking lobby. Please search again."); return; }
        *pending=(struct PendingLobbyCreate){room,search,0,hn,bn};
        memcpy(pending->docs,host,hn);pending->docs[hn]=0;
        memcpy(pending->docs+hn+1,confirmation_doc,bn);pending->docs[hn+1+bn]=0;
        pthread_mutex_lock(&mode_lock);
        free(pending_lobby_join);
        pending_lobby_join=pending;
        pthread_mutex_unlock(&mode_lock);
        fprintf(stderr,"Matchmaking: waiting for native session before join room=%" PRIu64 "\n",room);
        return;
    }
    native_join(self, search, room, host, hn, confirmation_doc, bn);
}
/* Native Back resets the race session. Send its peer message while the session still owns the connection. */
static void send_confirmed_departure(uint64_t departing_room) {
    if (!native_send_departure || !match_session_manager || !*match_session_manager) return;
    pthread_mutex_lock(&mode_lock);
    uint64_t room = 0;
    if (matchmaking_owner) memcpy(&room, matchmaking_owner + 0x958, sizeof room);
    int leaving = room == departing_room && room && room == matchmaking_room && room != departure_sent_room;
    pthread_mutex_unlock(&mode_lock);
    if (!leaving) return;
    void *session = ptr(*match_session_manager, 0x28);
    void *connection = session ? ptr(session, 0x10) : NULL;
    int role = connection ? *(int *)((char *)connection + 0x118) : 0;
    /* Host message 0x34 disbands the survivor's native session during requeue. */
    if (role != 2) return;
    void *list = participants && *participants ? ptr(*participants, 0x10) : NULL;
    int count = list ? *(int *)((char *)list + 12) : 0;
    void **rows = list ? ptr(list, 0x20) : NULL;
    int i = 0;
    for (; rows && count <= 8 && i < count && rows[i]; ++i)
        if (*(unsigned *)((char *)rows[i] + 0x20) <= 7) break;
    if (!rows || count > 8 || i >= count || !rows[i]) return;
    pthread_mutex_lock(&mode_lock);
    int send = room == matchmaking_room && room != departure_sent_room;
    if (send) departure_sent_room = room;
    pthread_mutex_unlock(&mode_lock);
    if (send) {
        native_send_departure();
        fprintf(stderr, "Matchmaking: confirmed departure sent room=%" PRIu64 " role=%d\n", room, role);
    }
}
/* The native peer-leave receiver at a540e2 removes this participant, then disconnects
 * its gameplay peer by account ID. The server confirms the same ID in the room push. */
static void apply_lobby_departure(uint64_t pid) {
    if (!native_remove_player || !native_disconnect_player || runtime_replay_race_active() ||
        runtime_online_mode_race_running()) return;
    void *manager = participants ? *participants : NULL;
    void *list = manager ? ptr(manager, 0x10) : NULL;
    void **rows = list ? ptr(list, 0x20) : NULL;
    int count = list ? *(int *)((char *)list + 12) : 0;
    for (int i = count; rows && i > 0 && count <= 8; --i) {
        unsigned char *player = rows[i - 1];
        uint64_t account = 0;
        if (!player || *(unsigned *)(player + 0x20) <= 7) continue;
        memcpy(&account, player + 0xa8, sizeof account);
        if (account != pid) continue;
        native_remove_player(manager, player);
        void *session = match_session_manager && *match_session_manager ? ptr(*match_session_manager, 0x28) : NULL;
        if (session) {
            __atomic_fetch_add((int *)((char *)session + 8), 1, __ATOMIC_SEQ_CST);
            native_disconnect_player(session, pid);
            native_release_object(session);
        }
        fprintf(stderr, "Matchmaking: applied native departure pid=%" PRIu64 "\n", pid);
        return;
    }
}
static void apply_room_update(uint64_t room, uint64_t version, const char *doc, int requeue) {
    const char *field = runtime_json_field(doc, "octr_departed_pid");
    uint64_t pid = field ? strtoull(field, NULL, 10) : 0;
    const char *name = runtime_json_field(doc, "octr_departed_name");
    size_t length = name && *name == '"' ? strspn(name + 1, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") : 0;
    pthread_mutex_lock(&mode_lock);
    int current = room == matchmaking_room && (room != last_departure_room || version >= last_departure_version);
    if (current && (room != last_departure_room || version > last_departure_version)) {
        last_departure_room = room;
        last_departure_version = version;
        last_departure_pid = 0;
    }
    int fresh = current && pid && pid != last_departure_pid;
    if (fresh) last_departure_pid = pid;
    /* Native disconnect can synchronously cancel the old lobby. Protect the replacement search first. */
    if (current && requeue) resume_room_search(room);
    pthread_mutex_unlock(&mode_lock);
    if (!fresh) return;
    apply_lobby_departure(pid);
    if (!bbgpu_notify) return;
    /* Finishing a race leaves the room, but retained premade members did not leave the party. */
    struct runtime_ranked_result result;
    const int ranked_finished=runtime_ranked_result_copy(&result) && result.room==room;
    if (__atomic_load_n(&had_public_race,__ATOMIC_ACQUIRE) &&
        (ranked_finished || (!runtime_replay_race_active() && !runtime_online_mode_race_running()))) {
        void *party = party_manager ? *party_manager : NULL;
        void *roster = party ? ptr(party,0x30) : NULL;
        int count = roster ? *(int *)((char *)roster+12) : 0;
        void **members = roster ? ptr(roster,0x20) : NULL;
        for (int i=0; members && count<=8 && i<count; ++i) {
            uint64_t member=0;
            if (members[i]) memcpy(&member,(char *)members[i]+0x18,sizeof member);
            if (member==pid) return;
        }
    }
    char message[64];
    if (length >= 3 && length <= 16 && name[length + 1] == '"')
        snprintf(message, sizeof message, "%.*s left", (int)length, name + 1);
    else
        snprintf(message, sizeof message, "Player %" PRIu64 " left", pid);
    bbgpu_notify("Matchmaking", message, "", 7, NULL, 0);
}
static void ABI created(void *self, uint64_t search, uint64_t room, uint64_t version, const char *host, unsigned hn, const char *confirmation_doc, unsigned bn) {
    const uint64_t host_pid=lobby_host_pid(host);
    const int racing=runtime_replay_race_active() || runtime_online_mode_race_running();
    if (racing) {
        uint64_t current;
        memcpy(&current,(unsigned char *)self+0x958,sizeof current);
        if (!room || current!=room) return;
    }
    if (!accept_entry(self, room, confirmation_doc, bn,2)) return;
    pthread_mutex_lock(&mode_lock);
    free(pending_lobby_join);
    pending_lobby_join=NULL;
    pthread_mutex_unlock(&mode_lock);
    if (racing && host_pid && race_lobby_host_pid && host_pid!=race_lobby_host_pid)
        race_handoff_departed_pid=race_lobby_host_pid;
    if (host_pid) race_lobby_host_pid=host_pid;
    apply_room_update(room, version, confirmation_doc, 0);
    /* A promotion updates the current room. Native create also enters lobby state 8. */
    if (racing) {
        native_update(self,room,version,host,hn,confirmation_doc,bn);
        pthread_mutex_lock(&mode_lock);
        if (native_bind_race_connection && native_start_match_host && matchmaking_room==room && matchmaking_owner==self) {
            pending_host_room=room;
            host_retry_after=0;
            host_attempt_until=check_clock()+20000;
        }
        pthread_mutex_unlock(&mode_lock);
    }
    else {
        /* The first native sync releases the joiner on the server. A requeued solo
         * guest must have finished opening its listener before it sends that sync. */
        pthread_mutex_lock(&mode_lock);
        int defer = requeued_room && !confirmed_ranked && native_start_match_host && match_session_manager &&
                    matchmaking_room == room && matchmaking_owner == self;
        if (defer && host && hn <= 65536) {
            struct PendingLobbyCreate *pending = malloc(sizeof *pending + hn + bn + 2);
            if (pending) {
                *pending = (struct PendingLobbyCreate){room,search,version,hn,bn};
                memcpy(pending->docs,host,hn);
                pending->docs[hn] = 0;
                memcpy(pending->docs + hn + 1,confirmation_doc,bn + 1);
                free(pending_lobby_create);
                pending_lobby_create = pending;
                prepare_requeue_host = 1;
                pthread_mutex_unlock(&mode_lock);
                return;
            }
        }
        pthread_mutex_unlock(&mode_lock);
        native_create(self,search,room,version,host,hn,confirmation_doc,bn);
    }
}
static void ABI updated(void *self, uint64_t room, uint64_t version, const char *host, unsigned hn, const char *confirmation_doc, unsigned bn) {
    uint64_t current = 0;
    memcpy(&current, (unsigned char *)self + 0x958, 8);
    pthread_mutex_lock(&mode_lock);
    int requeued = requeued_room && requeued_room == room;
    int tracked = matchmaking_room == room && matchmaking_owner == self;
    int stale = (matchmaking_search && !matchmaking_room) ||
        (room == last_departure_room && version < last_departure_version);
    pthread_mutex_unlock(&mode_lock);
    if (stale) return;
    const char *party_return = confirmation_doc && bn <= 65536 && strnlen(confirmation_doc,(size_t)bn+1) == bn
        ? runtime_json_field(confirmation_doc,"octr_party_return") : NULL;
    if ((tracked || (room && current == room && !requeued)) && party_return &&
        !strncmp(party_return,"true",4) && !runtime_room_mode_race_frozen() &&
        !runtime_replay_race_active() && !runtime_online_mode_race_running()) {
        return_to_party_menu();
        return;
    }
    if (requeued || (current != room && (current || !tracked))) return; /* A delayed update must not replace a different lobby. */
    if (accept(self, room, confirmation_doc, bn)) {
        const char *field = runtime_json_field(confirmation_doc, "octr_requeue");
        int requeue = field && !strncmp(field, "true", 4);
        apply_room_update(room, version, confirmation_doc, requeue);
        if (requeue) return;
        if (current == room) {
            const uint64_t new_host=lobby_host_pid(host);
            const int racing=runtime_replay_race_active() || runtime_online_mode_race_running();
            const int changed=racing && new_host && race_lobby_host_pid && new_host!=race_lobby_host_pid;
            if (changed) race_handoff_departed_pid=race_lobby_host_pid;
            native_update(self, room, version, host, hn, confirmation_doc, bn);
            if (new_host) race_lobby_host_pid=new_host;
            if (changed && native_join_match_host && race_join_state_call && !race_join_room) {
                void *candidate=match_session_manager && *match_session_manager ? *match_session_manager : NULL;
                void *session=candidate ? ptr(candidate,0x28) : NULL;
                race_join_old_connection=session ? ptr(session,0x10) : NULL;
                memcpy(race_join_state_call,"\x90\x90\x90\x90\x90",5);
                race_join_room=room;
                race_join_until=check_clock()+20000;
                fprintf(stderr,"Matchmaking: room %" PRIu64 " guest joining promoted host pid=%" PRIu64 " during race\n",room,new_host);
                native_join_match_host(self,room,host,confirmation_doc);
            }
        }
    }
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
static int object_valid(void *obj);
static void ABI disbanded(void *self, uint64_t room) {
    retire(self, room);
    /* Always tell the server the room was disbanded - this is the native lobbyDisbanded
     * task. Skipping it when the object looks stale leaves the server with a ghost member
     * (13:40 log: player left the found room but server only learned on connection close).
     * The probe.c fault handler already contains stale-vtable recovery for this path. */
    native_disbanded(self, room);
}
static void ABI kicked(void *self, uint64_t room) {
    retire(self, room);
    native_kicked(self, room);
}
static size_t image_bytes;
static void (ABI *native_party_reset_trace)(void *);
static unsigned char (ABI *native_party_return_trace)(void *,void *,void *);
typedef uint32_t (ABI *SessionInit)(void *,void *,void *,int);
static SessionInit native_session_init;
typedef unsigned char (ABI *SessionAdmission)(void *,void *,int *);
static SessionAdmission native_session_admission;
static int trace_session_init;

static unsigned char ABI admit_race_rejoin(void *session,void *peer,int *reason) {
    void *manager=match_session_manager ? *match_session_manager : NULL;
    unsigned char *owner=manager ? ptr(manager,0x10) : NULL;
    void *connection=session ? ptr(session,0x10) : NULL;
    pthread_mutex_lock(&mode_lock);
    const int handoff=matchmaking_room && owner==matchmaking_owner &&
        check_clock()<host_attempt_until && !local_race_quitting;
    pthread_mutex_unlock(&mode_lock);
    const int reopen=handoff && manager && ptr(manager,0x28)==session && connection &&
        *(int *)((char *)connection+0x118)==1 &&
        (runtime_replay_race_active() || runtime_online_mode_race_running());
    /* Native admission rejects closed race lobbies with 0x28; retain every other check. */
    unsigned char open=reopen ? owner[0x960] : 0;
    if (reopen) owner[0x960]=1;
    unsigned char accepted=native_session_admission(session,peer,reason);
    if (reopen) {
        owner[0x960]=open;
        fprintf(stderr,"Matchmaking: race handoff peer admission accepted=%u reason=%d\n",accepted,*reason);
    }
    return accepted;
}

static uint32_t ABI traced_session_init(void *session,void *second,void *third,int host) {
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)party_trace_image;
    void *connection=ptr(session,0x10);
    unsigned char security[24];
    void *manager=match_session_manager ? *match_session_manager : NULL;
    void *job=manager ? ptr(manager,0x18) : NULL;
    pthread_mutex_lock(&mode_lock);
    const int handoff=host && pending_host_room && pending_host_room==matchmaking_room &&
        matchmaking_owner && manager && ptr(manager,0x10)==matchmaking_owner &&
        job && ptr(job,0xa0)==session;
    if (handoff) memcpy(security,matchmaking_owner+0x900,sizeof security);
    pthread_mutex_unlock(&mode_lock);
    /* RESERVE generated fresh keys; reconnecting guests already have the room's DTLS keys. */
    if (handoff) { second=security; third=security+8; }
    uint32_t result=native_session_init(session,second,third,host);
    if (handoff) fprintf(stderr,"Matchmaking: promoted host initialized with room security success=%u\n",(unsigned char)result);
    if ((unsigned char)result && connection && ptr(connection,0xb0)==(void *)native_session_admission) {
        SessionAdmission admission=admit_race_rejoin;
        memcpy((char *)connection+0xb0,&admission,sizeof admission);
    }
    if (trace_session_init) fprintf(stderr,"Native session init: caller=0x%" PRIxPTR " session=%p connection=%p host_arg=%d result=%u\n",
            caller,session,connection,host,result);
    return result;
}

static void party_trace(const char *action, void *self, void *caller) {
    unsigned char *party=self;
    int state=0,role=0,peers=0;
    if (party) memcpy(&state,party+0x54,4);
    unsigned char *session=party ? *(void **)(party+0x18) : NULL;
    unsigned char *connection=session ? *(void **)(session+0x10) : NULL;
    if (connection) { memcpy(&role,connection+0x118,4);memcpy(&peers,connection+0x30,4); }
    uintptr_t address=(uintptr_t)caller,base=(uintptr_t)party_trace_image;
    fprintf(stderr,"Party lifecycle: %s caller=0x%" PRIxPTR " state=%d role=%d transport_peers=%d roster=%d following=%d menu_returned=%d\n",
            action,address>=base ? address-base : address,state,role,peers,ranked_party_size(),
            runtime_room_mode_following(),__atomic_load_n(&menu_party_returned,__ATOMIC_SEQ_CST));
}
/* Guard against stale party/session objects left over from previous takeovers.
 * Their vtable pointer gets reset to image+0x1490000; native release/quit then
 * calls vtable[0] which jumps outside the image and faults. Skip the native call
 * when the object's vtable or its first method are not inside the image. */
static int object_valid(void *obj) {
    if (!obj) return 1; /* NULL: nothing to validate */
    void *vtable = *(void **)obj;
    if (!vtable) return 1; /* no vtable: let native handle it (tests/mocks) */
    if ((uintptr_t)vtable < (uintptr_t)party_trace_image) return 1; /* outside image: not ours */
    if ((uintptr_t)vtable >= (uintptr_t)party_trace_image + image_bytes) return 1; /* outside image */
    /* vtable is inside the image: its first entry must be a code pointer too.
     * Stale takeover objects point at image+0x1490000 whose vtable[0] is outside. */
    void *vtable0 = *(void **)vtable;
    if (!vtable0) return 0;
    if ((uintptr_t)vtable0 < (uintptr_t)party_trace_image) return 0;
    if ((uintptr_t)vtable0 >= (uintptr_t)party_trace_image + image_bytes) return 0;
    return 1;
}

static void ABI traced_party_reset(void *self) {
    if (!object_valid(self)) return;
    unsigned char *session = *(void **)(self + 0x18);
    if (!object_valid(session)) return;
    unsigned char *connection = session ? *(void **)(session + 0x10) : NULL;
    if (!object_valid(connection)) return;
    party_trace("reset",self,__builtin_return_address(0));
    /* A guest loses the host here. Snapshot whether the active search was Ranked before the
     * native teardown / register_client reset wipes it, so takeover_host restores it. */
    pthread_mutex_lock(&mode_lock);
    if (confirmed_ranked || search_is_ranked) joined_ranked=1;
    pthread_mutex_unlock(&mode_lock);
    const int private_match=runtime_private_match_active();
    const int announced_return=__atomic_load_n(&party_return_received,__ATOMIC_SEQ_CST);
    int role=0;
    if (connection) memcpy(&role,connection+0x118,sizeof role);
    runtime_party_host_lost(role,private_match,announced_return);
    native_party_reset_trace(self);
}
static void ABI traced_party_quit(void *self) {
    if (!object_valid(self)) return;
    unsigned char *session = *(void **)(self + 0x18);
    if (!object_valid(session)) return;
    unsigned char *connection = session ? *(void **)(session + 0x10) : NULL;
    if (!object_valid(connection)) return;
    party_trace("quit",self,__builtin_return_address(0));
    native_party_quit_trace(self);
}

static void return_to_party_menu(void) {
    __atomic_store_n(&party_return_received,1,__ATOMIC_SEQ_CST);
    unsigned char *party=party_manager ? *party_manager : NULL;
    const int already_returned=__atomic_load_n(&menu_party_returned,__ATOMIC_SEQ_CST) &&
        (!party || !party[0x6c]);
    if (party) party[0x6c]=0; /* Same private-lobby cleanup as the native receiver. */
    /* Pin the ModeSelect we come back to. route 2 does not save it, so without this the
     * party returns to Casual Normal even though it was queued Ranked/Itemless. */
    pthread_mutex_lock(&mode_lock);
    const int ret_mode = confirmed_ranked || joined_ranked ? 2 :
                         (confirmed_itemless || joined_itemless ? 1 : 0);
    pthread_mutex_unlock(&mode_lock);
    runtime_online_mode_set_return_mode(ret_mode);
    /* In menus, close to native Waiting for Host; the race handler can load Start Screen. */
    if (runtime_online_mode_matchmaking_cancel(2)) return;
    runtime_public_itemless_menu_return();
    /* A later host exit can reach a guest already in Waiting Room. */
    if (!already_returned) native_return_to_lobby();
}
static unsigned char ABI received_party_return(void *sender,void *message,void *context) {
    if (runtime_ranked_active() && !entry_private && party_at_entry > 1)
        ranked_return_party = 1;
    party_trace("receive-return",party_manager ? *party_manager : NULL,__builtin_return_address(0));
    if (!runtime_room_mode_race_frozen() && !runtime_replay_race_active() &&
        !runtime_online_mode_race_running()) {
        unsigned char *party=party_manager ? *party_manager : NULL;
        int state=0;
        if (party) memcpy(&state,party+0x54,sizeof state);
        pthread_mutex_lock(&mode_lock);
        const int public_session=matchmaking_search || search_server_active || matchmaking_room || (!entry_private && state==7);
        if (public_session) {
            /* Native return packets carry no room/search ID; corroborate against the current queue. */
            party_return_check=1;
            search_poll_after=check_clock()+250;
            ++search_poll_serial;
            search_poll_running=0;
        }
        pthread_mutex_unlock(&mode_lock);
        if (public_session) return 1;
        return_to_party_menu();
        return 1;
    }
    return native_party_return_trace(sender,message,context);
}

static void jump(unsigned char *at, const void *target) { memcpy(at,"\xff\x25\0\0\0\0",6); memcpy(at+6,&target,8); }
static int ABI in_network_game(void) {
    if (runtime_public_found_room() && runtime_online_mode_race_running()) return 1;
    if (!party_trace_image || !ptr(party_trace_image,0x2acc7b0)) return 0;
    void *state=((void *(ABI *)(void))(party_trace_image+0x3e2430))();
    void *players=state ? ptr(state,0x28) : NULL;
    return players && ((int (ABI *)(void *))(party_trace_image+0x43c650))(players)>0;
}
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
    const size_t shift=!strcmp(hook,"1277df0") ? 0x10 : 0;
    const size_t sites[]={0xda300,0xd8fb0,0xda4f0,0xda7f0,0xa14550,0x24de10};
    const size_t stolen[]={17,17,17,17,14,17};
    const unsigned char destroy_prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x89,0xfb};
    const unsigned char prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec};
    const size_t slots[]={0x2549ea8,0x2549ee0,0x2549ee8,0x2549ec0,0x2549eb0,0x2549eb8};
    const size_t targets[]={0xa14890,0xa14910,0xa14e20,0xa148c0,0xa148a0,0xa148b0};
    const unsigned char ui_prefix[]={0x55,0x48,0x89,0xe5,0x53,0x50};
    if (memcmp(image+0xa97a00,ui_prefix,6) || memcmp(image+0xa9cbe0,ui_prefix,6)) return 0;
    const unsigned char return_trace_prefix[]={0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x83,0xec,0x30,0x4c,0x8b,0x35,0x06,0xba,0xda,0x01};
    if (memcmp(image+0xa5b680,return_trace_prefix,sizeof return_trace_prefix)) return 0;
    const unsigned char reset_trace_prefix[]={0x55,0x48,0x89,0xe5,0x53,0x50,0x48,0x89,0xfb,0x83,0x7b,0x54,0x0a,0x0f,0x84,0x93,0x00,0x00,0x00};
    if (memcmp(image+0xa18210,reset_trace_prefix,sizeof reset_trace_prefix) ||
        memcmp(image+0xa9ffd0,prefix,16) || image[0xa9ffe0]!=0x28) return 0;
    const unsigned char stop_party_call[] = {0xe8,0x52,0xba,0xf8,0xff};
    const unsigned char stop_tail[] = {0x31,0xf6,0x48,0x83,0xc4,0x08,0x5b,0x5d,0xe9,0x9e,0xfd,0xf7,0xff};
    if (memcmp(image+0xa97a19,stop_party_call,sizeof stop_party_call) ||
        memcmp(image+0xa97a25,stop_tail,sizeof stop_tail)) return 0;
    const unsigned char party_return_prefix[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xec,0x18};
    if (memcmp(image+0xaa06a0,party_return_prefix,sizeof party_return_prefix)) return 0;
    if (memcmp(image+sites[4],destroy_prefix,sizeof destroy_prefix)) return 0;
    if (memcmp(image+sites[5],prefix,16) || image[sites[5]+16]!=0x48) return 0;
    if (memcmp(image+0xa548e0,prefix,16) || image[0xa548f0]!=0x38) return 0;
    for (int i=0;i<4;++i) {
        if (memcmp(image+sites[i],prefix,16) || image[sites[i]+16]!=(i==0?0x68:0x58)) return 0;
    }
    for (int i=0;i<6;++i) if (ptr(image,slots[i])!=image+targets[i]) return 0;
#ifdef _WIN32
    unsigned char *stubs=VirtualAlloc(NULL,624,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
#else
    unsigned char *stubs=mmap(NULL,624,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (stubs==MAP_FAILED) stubs=NULL;
#endif
    if (!stubs) return 0;
    const unsigned char rejoin_prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18};
    if (memcmp(image+0xbd0ad0,rejoin_prefix,sizeof rejoin_prefix) ||
        !writable(image+0xbd0ad0,sizeof rejoin_prefix)) return 0;
    memcpy(stubs+592,rejoin_prefix,sizeof rejoin_prefix);
    jump(stubs+609,image+0xbd0ae1);
    native_rejoin_players=(void (ABI *)(void *))(stubs+592);
    jump(image+0xbd0ad0,rejoin_players);
    const unsigned char reset_ids_prefix[]={0x48,0x63,0x47,0x0c,0x48,0x85,0xc0,0x7e,0x43,0x48,0x8b,0x4f,0x20,0x48,0x8b,0x11};
    if (memcmp(image+0xc23120,reset_ids_prefix,sizeof reset_ids_prefix) ||
        !writable(image+0xc23120,sizeof reset_ids_prefix)) return 0;
    memcpy(stubs+544,reset_ids_prefix,7);
    memcpy(stubs+551,"\x7f\x0e",2);
    jump(stubs+553,image+0xc2316c);
    memcpy(stubs+567,reset_ids_prefix+9,7);
    jump(stubs+574,image+0xc23130);
    native_reset_local_racer_ids=(void (ABI *)(void *))(stubs+544);
    jump(image+0xc23120,reset_local_racer_ids);
    const unsigned char remove_prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x54,0x53,0x48,0x83,0xec,0x10};
    if (memcmp(image+0xc22160,remove_prefix,sizeof remove_prefix) ||
        !writable(image+0xc22160,sizeof remove_prefix)) return 0;
    memcpy(stubs+512,remove_prefix,sizeof remove_prefix);
    jump(stubs+512+sizeof remove_prefix,image+0xc22160+sizeof remove_prefix);
    native_remove_player_original=(int (ABI *)(void *,void *))(stubs+512);
    jump(image+0xc22160,remove_race_player);
    if (!writable(image+0xa18210,19) || !writable(image+0xa9ffd0,17) || !writable(image+0xa5b680,18)) return 0;
    if (!memcmp(image+0xa17526,"\xe8\x85\xe8\xff\xff",5) &&
        !memcmp(image+0xa170a8,"\x0f\x84\x86\x01\x00\x00",6) &&
        writable(image+0xa17526,5) && writable(image+0xa170a8,6)) {
        race_join_state_call=image+0xa17526;
        native_join_match_host=(void (ABI *)(void *,uint64_t,const char *,const char *))(image+0xa17070);
        /* The server's host update arrives on the race map, not OnlineLobby. */
        memset(image+0xa170a8,0x90,6);
    }
    for (int i=0;i<6;++i) if (!writable(image+sites[i],stolen[i])) return 0;
    for (int i=0;i<6;++i) if (!writable(image+slots[i],8)) return 0;
    const char *experiment=getenv("BB_HOST_EXPERIMENT");
    const size_t init_slots[]={0x254edc0,0x255e6c8};
    trace_session_init=experiment && !strcmp(experiment,"1");
    for (size_t i=0;i<2;++i)
        if (ptr(image,init_slots[i])!=image+0xa52710 || !writable(image+init_slots[i],8)) return 0;
    /* The reset prologue contains a conditional branch; preserve both native destinations. */
    memcpy(stubs+360,image+0xa18210,13);
    memcpy(stubs+373,"\x75\x0e",2);
    jump(stubs+375,image+0xa182b6);
    jump(stubs+389,stubs+416);
    memcpy(stubs+256,image+0xa9ffd0,17);
    jump(stubs+273,image+0xa9ffe1);
    native_party_reset_trace=(void (ABI *)(void *))(stubs+360);
    native_party_quit_trace=(void (ABI *)(void *))(stubs+256);
    memcpy(stubs+320,image+0xa5b680,11);
    memcpy(stubs+331,"\x49\xbe",2);
    void *guard_slot=image+0x2807098;
    memcpy(stubs+333,&guard_slot,8);
    memcpy(stubs+341,"\x4d\x8b\x36",3);
    jump(stubs+344,image+0xa5b692);
    native_party_return_trace=(unsigned char (ABI *)(void *,void *,void *))(stubs+320);
    /* A guest whose host went away can hold a session with a dead vtable. Skip its virtual stop,
     * but still clear and release the session exactly as the native reset does. */
    {
        unsigned char *guard=stubs+416;
        const uintptr_t code=(uintptr_t)image, end=code+size;
        memcpy(guard,   "\x48\x8b\x7b\x18",4);            /* mov 0x18(%rbx),%rdi */
        memcpy(guard+4, "\x48\x85\xff",3);                /* test %rdi,%rdi */
        memcpy(guard+7, "\x74\x40",2);                    /* je skip */
        memcpy(guard+9, "\x48\x8b\x07",3);                /* mov (%rdi),%rax */
        memcpy(guard+12,"\x48\x85\xc0",3);                /* test %rax,%rax */
        memcpy(guard+15,"\x74\x38",2);                    /* je skip */
        memcpy(guard+17,"\x48\x8b\x80\xb8\x00\x00\x00",7); /* mov 0xb8(%rax),%rax */
        memcpy(guard+24,"\x48\xb9",2); memcpy(guard+26,&code,8);
        memcpy(guard+34,"\x48\x39\xc8",3);                /* cmp %rcx,%rax */
        memcpy(guard+37,"\x72\x22",2);                    /* jb skip */
        memcpy(guard+39,"\x48\xb9",2); memcpy(guard+41,&end,8);
        memcpy(guard+49,"\x48\x39\xc8",3);                /* cmp %rcx,%rax */
        memcpy(guard+52,"\x73\x13",2);                    /* jae skip */
        memcpy(guard+54,"\x90\x90\x90\x90\x90",5);      /* any function inside the image is valid */
        jump(guard+59,image+0xa18223);
        jump(guard+73,image+0xa18235);                    /* skip stop; clear +0x18 and release */
    }
    party_trace_image=image;
    image_bytes=size;
    if (!memcmp(image+0xa90b10,"\x48\x83\x3d\x98\xbc\x03\x02\x00\x74\x19\x55\x48\x89\xe5",14) &&
        writable(image+0xa90b10,14)) jump(image+0xa90b10,(void *)in_network_game);
    /* The native wrapper checks network state before queuing HostSession on this manager. */
    const unsigned char host_wrapper[]={0x55,0x48,0x89,0xe5,0x53,0x50,0x48,0x8b,0x05,0x5b,0x76,0x07,0x02};
    const unsigned char host_tail[]={0xe9,0x3e,0xb5,0xff,0xff};
    const unsigned char bind_prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50,0x49,0x89,0xfd,0x49,0x89,0xf6,0x49,0x8b,0x45,0x20};
    if (!memcmp(image+0xa177dd,"\xe8\x8e\x00\x00\x00",5) &&
        !memcmp(image+0xa17890,"\x48\x8b\x05\x19\x52\x0b\x02\x48\x8b\x58\x28",11)) {
        match_session_manager=(void **)(image+0x2accab0);
        native_send_departure=(void (ABI *)(void))(image+0xa17870);
    }
    if (!memcmp(image+0xa540e9,"\xe8\x72\xe0\x1c\x00\x48\x8b\x05\xbb\x89\x07\x02",12) &&
        !memcmp(image+0xa5410a,"\xe8\x71\x00\x00\x00",5)) {
        native_remove_player=(int (ABI *)(void *,void *))(image+0xc22160);
        native_disconnect_player=(void (ABI *)(void *,uint64_t))(image+0xa54180);
        match_session_manager=(void **)(image+0x2accab0);
    }
    if (!memcmp(image+0xa53d70,host_wrapper,sizeof host_wrapper) &&
        !memcmp(image+0xa53dad,host_tail,sizeof host_tail)) {
        match_session_manager=(void **)(image+0x2accab0);
        native_start_match_host=(void (ABI *)(void *))(image+0xa53d70);
        if (!memcmp(image+0xa44190,bind_prefix,sizeof bind_prefix))
            native_bind_race_connection=(void (ABI *)(void *,void *))(image+0xa44190);
        if (native_bind_race_connection)
            native_set_race_connection=(void (ABI *)(void *,void *))(image+0xa444e0);
    }
    native_session_init=(SessionInit)(image+0xa52710);
    native_session_admission=(SessionAdmission)(image+0xa53100);
    SessionInit trace=traced_session_init;
    for (size_t i=0;i<2;++i) memcpy(image+init_slots[i],&trace,sizeof trace);
    jump(image+0xa18210,(void *)traced_party_reset);
    jump(image+0xa9ffd0,(void *)traced_party_quit);
    jump(image+0xa5b680,(void *)received_party_return);
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
    race_racers=(void **)(image+0x2ad4b70);
    race_object_root=(void **)(image+0x2a30500);
    native_take_racer_ownership=(uint64_t (ABI *)(void *,void *))(image+0x4332f0);
    participants=(void **)(image+0x2ad1160);
    party_manager=(void **)(image+0x2acf700);
    native_placeable_event=(void (ABI *)(void *, void *))(stubs+160);
    native_cancel_queue=(void (ABI *)(void *))(image+0xab66f0);
    native_leave_race_session=(void (ABI *)(void *,int))(image+0xa177d0);
    native_reset_race_session=(void (ABI *)(void *))(image+0xa18380);
    native_close_party_lobby=(void (ABI *)(int))(image+0xa5d550);
    native_stop_matchmaking=stop_search_keep_party;
    native_party_return=(void (ABI *)(void *))(image+0xaa06a0);
    native_return_to_lobby=(void (ABI *)(void))(image+0xa9cbe0);
    native_notice=(Notice)(image+0xa548e0);
    if (native_bind_race_connection &&
        !memcmp(image+0xa44d34,"\xe8\x57\xf4\xff\xff",5) &&
        writable(image+0xa44190,14)) {
        memcpy(stubs+288,image+0xa44190,14);
        jump(stubs+302,image+0xa4419e);
        native_race_switch_original=(void (ABI *)(void *,void *))(stubs+288);
        jump(image+0xa44190,race_connection_switch);
        puts("Matchmaking: race host-loss guard installed");
    }
    const unsigned char leave_prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18};
    if (!memcmp(image+0xa54060,leave_prefix,sizeof leave_prefix) && writable(image+0xa54060,sizeof leave_prefix)) {
        memcpy(stubs+224,image+0xa54060,sizeof leave_prefix);
        jump(stubs+224+sizeof leave_prefix,image+0xa54060+sizeof leave_prefix);
        native_peer_leave_original=(uint64_t (ABI *)(void *))(stubs+224);
        jump(image+0xa54060,peer_leave_message);
        puts("Matchmaking: lobby peer-leave guard installed");
    }
    for (int i=0;i<6;++i) {
        memcpy(stubs+i*32,image+sites[i],stolen[i]);
        jump(stubs+i*32+stolen[i],image+sites[i]+stolen[i]);
        jump(image+sites[i],handlers[i]);
    }
    for (int i=0;i<6;++i) memcpy(image+slots[i],&events[i],8);
    native_make_handle=(void (ABI *)(void **,void *))(image+0x14bea20-shift);
    native_resolve_handle=(void (ABI *)(void **))(image+0x149fca0-shift);
    native_hof_focus=(void (ABI *)(void *,void *,int))(image+0x24db60);
    native_meta_field=(void *(ABI *)(void *,const char *))(image+0x14a7250-shift);
    native_leaderboard_ready=(void *(ABI *)(void *))(image+0x1104d80-shift);
    native_release_object=(void (ABI *)(void *))(image+0x149d260-shift);
    native_deferred_event_create=(void *(ABI *)(void *,void *,int))(image+0x14a0e30-shift);
    native_deferred_event_release=native_release_object;
    native_destroy_handle=(void (ABI *)(void **))(image+0x149f280-shift);
    installed=1;
    puts("Public ITEMLESS: isolated matchmaking hooks installed (full-party gate, native OK notice)");
    return 1;
}
