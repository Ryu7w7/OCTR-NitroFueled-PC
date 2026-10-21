/* RyuNET friends: a background thread asks the server (ryunet_friends.go, its text form)
 * for this player's friends, requests and invitations every few seconds, and runs what the
 * menu's Friends tab asks (request, accept, decline, remove, invite, search). The tab reads a
 * copy of the latest answer; changes become notices (runtime_friends_take_event). */
#include "runtime_friends.h"
#include "runtime_http.h"
#include "runtime_net.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

const char *runtime_np_get_online_name(void);
void runtime_invite_join(const char *host); /* runtime_services.c */

#define REFRESH_SECONDS 8
#define MAX_QUEUE 64
#define MAX_EVENTS 32

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_wake = PTHREAD_COND_INITIALIZER;
static int s_started;
static RfView s_view;
static struct { char action[12], who[RF_NAME]; } s_queue[MAX_QUEUE];
static int s_queued, s_refresh_now, s_search_due;
static struct { int due; char who[RF_NAME], reason[12], comment[RF_REPORT_COMMENT]; } s_report;
static int s_hidden_wanted = -1, s_hidden_sent = -1; /* appear offline: as the menu says, as the server knows */
static time_t s_privacy_retry;                       /* after a failed attempt */
static struct { int kind; char name[RF_NAME]; } s_events[MAX_EVENTS];
static int s_event_head, s_event_count;
static char s_names[RF_MAX_FRIENDS * RF_NAME];
static unsigned s_names_serial;

/* Nicknames are 3-16 of A-Z a-z 0-9 _ -: anything else is cut, so a name is safe in a URL. */
static void clean_name(char *dst, const char *src) {
    size_t n = 0;
    for (; src && *src && n < RF_NAME - 1; src++) {
        const char c = *src;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') dst[n++] = c;
    }
    dst[n] = '\0';
}

/* The next line of a text answer (cut in place), NULL at the end. strtok would share its state
 * with other threads on some C libraries. */
static char *next_line(char **cursor) {
    char *line = *cursor;
    if (!line || !*line) return NULL;
    char *end = strchr(line, '\n');
    if (end) {
        *end = '\0';
        *cursor = end + 1;
    } else {
        *cursor = line + strlen(line);
    }
    return line;
}

static void push_event_locked(int kind, const char *name) {
    if (s_event_count == MAX_EVENTS) return;
    const int at = (s_event_head + s_event_count++) % MAX_EVENTS;
    s_events[at].kind = kind;
    snprintf(s_events[at].name, RF_NAME, "%s", name);
}

static int in_list(char (*list)[RF_NAME], int count, const char *name) {
    for (int i = 0; i < count; i++)
        if (!_stricmp(list[i], name)) return 1;
    return 0;
}

/* Invitations already told: who, and when they sent it (to tell a new one from the same friend). */
static struct { char from[RF_NAME]; time_t sent; } s_told[RF_MAX_INVITES];
static int s_told_count;

static int invite_told(const char *from, time_t sent) {
    for (int i = 0; i < s_told_count; i++)
        if (!_stricmp(s_told[i].from, from) && labs((long)(s_told[i].sent - sent)) < 20) return 1;
    if (s_told_count == RF_MAX_INVITES) {
        memmove(s_told, s_told + 1, sizeof s_told[0] * (RF_MAX_INVITES - 1));
        s_told_count--;
    }
    snprintf(s_told[s_told_count].from, RF_NAME, "%s", from);
    s_told[s_told_count++].sent = sent;
    return 0;
}

/* The administrators' messages (the server's messages.go), each shown once: their IDs, and the
 * ones still to show. */
#define MAX_MESSAGES 8
static long long s_msg_seen[64];
static int s_msg_seen_next;
static char s_msgs[MAX_MESSAGES][RF_MESSAGE];
static int s_msg_head, s_msg_count;

static void deliver_message(long long id, const char *text) {
    pthread_mutex_lock(&s_lock);
    int seen = 0;
    for (int i = 0; i < 64 && !seen; i++) seen = s_msg_seen[i] == id;
    if (!seen && *text) {
        s_msg_seen[s_msg_seen_next] = id;
        s_msg_seen_next = (s_msg_seen_next + 1) % 64;
        if (s_msg_count < MAX_MESSAGES) {
            snprintf(s_msgs[(s_msg_head + s_msg_count++) % MAX_MESSAGES], RF_MESSAGE, "%s", text);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

/* The server announcement ("maintenance in 5 minutes"): written where the launcher writes it
 * (ryunet_announce.txt next to bbport.ini), so the game draws its banner even without the
 * launcher; the game shows each ID once, whoever wrote it. */
static void deliver_announcement(long long id, const char *level, long long seconds_left, const char *text) {
    static long long last;
    if (id == last || seconds_left <= 0 || !*text) return;
    last = id;
    char path[600];
    const char *cfg = getenv("BB_CONFIG");
    const char *slash = cfg ? strrchr(cfg, '/') : NULL, *bslash = cfg ? strrchr(cfg, '\\') : NULL;
    if (bslash > slash) slash = bslash;
    if (slash) snprintf(path, sizeof path, "%.*s/ryunet_announce.txt", (int)(slash - cfg), cfg);
    else snprintf(path, sizeof path, "ryunet_announce.txt");
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "id=%lld\nlevel=%s\nexpires=%lld\ntext=%s\n", id, level, (long long)time(NULL) + seconds_left, text);
    fclose(f);
}

/* Reads the server's lines into next; events for what changed since the previous answer. */
static void take_view(char *text, RfView *next) {
    for (char *line, *cursor = text; (line = next_line(&cursor));) {
        char name[64];
        int a = 0, b = 0, c = 0, rest = 0;
        long long id = 0, left = 0;
        if (sscanf(line, "message %lld %n", &id, &rest) == 1 && rest > 0) {
            deliver_message(id, line + rest);
        } else if (sscanf(line, "announce %lld %15s %lld %n", &id, name, &left, &rest) == 3 && rest > 0) {
            deliver_announcement(id, name, left, line + rest);
        } else if (sscanf(line, "friend %d %d %d %63s", &a, &b, &c, name) == 4 && next->friend_count < RF_MAX_FRIENDS) {
            RfFriend *f = &next->friends[next->friend_count++];
            clean_name(f->name, name);
            f->online = a, f->players = b, f->max = c;
        } else if (sscanf(line, "incoming %63s", name) == 1 && next->incoming_count < RF_MAX_PENDING) {
            clean_name(next->incoming[next->incoming_count++], name);
        } else if (sscanf(line, "outgoing %63s", name) == 1 && next->outgoing_count < RF_MAX_PENDING) {
            clean_name(next->outgoing[next->outgoing_count++], name);
        } else if (sscanf(line, "invite %d %63s", &a, name) == 2 && next->invite_count < RF_MAX_INVITES) {
            RfInvite *inv = &next->invites[next->invite_count++];
            clean_name(inv->from, name);
            inv->ago = a;
        }
    }
}

static void refresh(const char *me) {
    char target[160], *body = malloc(32768);
    if (!body) return;
    snprintf(target, sizeof target, "/ryunet/friends?name=%s&format=text", me);
    const int status = runtime_ryunet_request("GET", target, body, 32768);
    RfView *next = calloc(1, sizeof *next);
    if (!next) {
        free(body);
        return;
    }
    if (status == 200) take_view(body, next);
    free(body);

    pthread_mutex_lock(&s_lock);
    if (status != 200) {
        s_view.state = status == 0 ? RF_UNREACHABLE : RF_REFUSED;
        s_view.serial++;
        pthread_mutex_unlock(&s_lock);
        free(next);
        return;
    }
    const int first = s_view.state != RF_READY && s_view.state != RF_UNREACHABLE;
    const time_t now = time(NULL);
    for (int i = 0; i < next->friend_count && !first; i++) {
        const RfFriend *f = &next->friends[i];
        int was_friend = 0, was_online = 0;
        for (int j = 0; j < s_view.friend_count; j++) {
            if (!_stricmp(s_view.friends[j].name, f->name)) was_friend = 1, was_online = s_view.friends[j].online;
        }
        if (!was_friend && in_list(s_view.outgoing, s_view.outgoing_count, f->name))
            push_event_locked(RF_EVENT_ACCEPTED, f->name);
        else if (was_friend && f->online && !was_online)
            push_event_locked(RF_EVENT_ONLINE, f->name);
    }
    for (int i = 0; i < next->incoming_count && !first; i++) {
        if (!in_list(s_view.incoming, s_view.incoming_count, next->incoming[i]))
            push_event_locked(RF_EVENT_REQUEST, next->incoming[i]);
    }
    for (int i = 0; i < next->invite_count; i++) {
        if (!invite_told(next->invites[i].from, now - next->invites[i].ago))
            push_event_locked(RF_EVENT_INVITE, next->invites[i].from);
    }
    s_view.state = RF_READY;
    s_view.friend_count = next->friend_count;
    memcpy(s_view.friends, next->friends, sizeof next->friends);
    s_view.incoming_count = next->incoming_count;
    memcpy(s_view.incoming, next->incoming, sizeof next->incoming);
    s_view.outgoing_count = next->outgoing_count;
    memcpy(s_view.outgoing, next->outgoing, sizeof next->outgoing);
    s_view.invite_count = next->invite_count;
    memcpy(s_view.invites, next->invites, sizeof next->invites);
    s_view.serial++;
    char names[sizeof s_names] = "";
    for (int i = 0; i < next->friend_count; i++) {
        if (i) strncat(names, ",", sizeof names - strlen(names) - 1);
        strncat(names, next->friends[i].name, sizeof names - strlen(names) - 1);
    }
    if (strcmp(names, s_names)) {
        memcpy(s_names, names, sizeof s_names);
        s_names_serial++;
    }
    pthread_mutex_unlock(&s_lock);
    free(next);
}

static void act(const char *me, const char *action, const char *who) {
    const char *arg = !strcmp(action, "accept") || !strcmp(action, "decline") ? "from"
                      : !strcmp(action, "remove")                              ? "who"
                                                                               : "to";
    char target[160], body[256];
    snprintf(target, sizeof target, "/ryunet/friends/%s?name=%s&%s=%s&format=text", action, me, arg, who);
    const int status = runtime_ryunet_request("POST", target, body, sizeof body);
    char word[24] = "error";
    if (status == 200) sscanf(body, "status %23s", word);
    else if (status == 403) snprintf(word, sizeof word, "refused");
    runtime_net_trace("Friends: %s %s -> %d %s", action, who, status, word);
    pthread_mutex_lock(&s_lock);
    snprintf(s_view.result, sizeof s_view.result, "%s", word);
    snprintf(s_view.result_who, RF_NAME, "%s", who);
    s_view.serial++;
    pthread_mutex_unlock(&s_lock);
}

/* Percent-encodes text for a URL query (UTF-8 bytes pass through encoded). */
static void url_encode(char *dst, size_t size, const char *src) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && n + 4 < size; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            dst[n++] = (char)*p;
        } else {
            dst[n++] = '%';
            dst[n++] = hex[*p >> 4];
            dst[n++] = hex[*p & 15];
        }
    }
    dst[n] = '\0';
}

static void report(const char *me, const char *who, const char *reason, const char *comment) {
    char encoded[RF_REPORT_COMMENT * 3 + 1], target[RF_REPORT_COMMENT * 3 + 160], body[64];
    url_encode(encoded, sizeof encoded, comment);
    snprintf(target, sizeof target, "/ryunet/report?name=%s&who=%s&reason=%s&comment=%s", me, who, reason, encoded);
    const int status = runtime_ryunet_request("POST", target, body, sizeof body);
    char word[16] = "error";
    if (status == 200) sscanf(body, "%15s", word);
    else if (status == 403) snprintf(word, sizeof word, "refused");
    runtime_net_trace("Friends: report %s (%s) -> %d %s", who, reason, status, word);
    pthread_mutex_lock(&s_lock);
    s_view.reporting = 0;
    snprintf(s_view.report_result, sizeof s_view.report_result, "%s", word);
    s_view.serial++;
    pthread_mutex_unlock(&s_lock);
}

static void search(const char *me, const char *query) {
    char target[160], body[2048];
    snprintf(target, sizeof target, "/ryunet/friends/search?name=%s&q=%s&format=text", me, query);
    const int status = runtime_ryunet_request("GET", target, body, sizeof body);
    RfPlayer found[RF_MAX_SEARCH];
    int count = 0;
    if (status == 200) {
        for (char *line, *cursor = body; count < RF_MAX_SEARCH && (line = next_line(&cursor));) {
            char relation[16], name[64];
            int online = 0;
            if (sscanf(line, "player %d %15s %63s", &online, relation, name) == 3) {
                clean_name(found[count].name, name);
                found[count].online = online;
                snprintf(found[count].relation, sizeof found[count].relation, "%s", relation);
                count++;
            }
        }
    }
    pthread_mutex_lock(&s_lock);
    if (!strcmp(s_view.search_query, query)) { /* not replaced by a newer search meanwhile */
        memcpy(s_view.search, found, sizeof found[0] * count);
        s_view.search_count = count;
        s_view.searching = 0;
        s_view.serial++;
    }
    pthread_mutex_unlock(&s_lock);
}

/* The launchers' old Friends field (bbport.ini online_friends, BB_ONLINE_FRIENDS from run.py):
 * once per installation, at the first answer of the server, each of those nicknames gets a friend
 * request (or its own request is accepted), so nobody loses the friends they had. Done when
 * friends_migrated.txt exists next to bbport.ini; the old list still feeds the game's friend list. */
static void migrate_old_friends(const char *me) {
    char path[600];
    const char *cfg = getenv("BB_CONFIG");
    const char *slash = cfg ? strrchr(cfg, '/') : NULL, *bslash = cfg ? strrchr(cfg, '\\') : NULL;
    if (bslash > slash) slash = bslash;
    if (slash) snprintf(path, sizeof path, "%.*s/friends_migrated.txt", (int)(slash - cfg), cfg);
    else snprintf(path, sizeof path, "friends_migrated.txt");
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return;
    }
    char list[2048];
    snprintf(list, sizeof list, "%s", getenv("BB_ONLINE_FRIENDS") ? getenv("BB_ONLINE_FRIENDS") : "");
    int sent = 0;
    pthread_mutex_lock(&s_lock);
    for (char *p = list; *p;) {
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        char name[RF_NAME];
        clean_name(name, p);
        p = comma ? comma + 1 : p + strlen(p);
        int known = strlen(name) < 3 || !_stricmp(name, me) || in_list(s_view.outgoing, s_view.outgoing_count, name);
        for (int i = 0; i < s_view.friend_count && !known; i++) known = !_stricmp(s_view.friends[i].name, name);
        if (known || s_queued == MAX_QUEUE) continue;
        snprintf(s_queue[s_queued].action, sizeof s_queue[0].action, "%s",
                 in_list(s_view.incoming, s_view.incoming_count, name) ? "accept" : "request");
        snprintf(s_queue[s_queued].who, RF_NAME, "%s", name);
        s_queued++;
        sent++;
    }
    pthread_mutex_unlock(&s_lock);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", getenv("BB_ONLINE_FRIENDS") ? getenv("BB_ONLINE_FRIENDS") : "");
        fclose(f);
    }
    runtime_net_trace("Friends: %d friend request(s) from the old Friends list", sent);
}

static void *friends_thread(void *arg) {
    (void)arg;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa); /* its own reference: the game may end its network */
#endif
    char me[RF_NAME];
    clean_name(me, runtime_np_get_online_name());
    time_t next_refresh = 0;
    for (;;) {
        char action[12] = "", who[RF_NAME] = "", query[RF_NAME] = "";
        char report_reason[12] = "", report_comment[RF_REPORT_COMMENT] = "";
        int refresh_now = 0, privacy = -1;
        pthread_mutex_lock(&s_lock);
#define PRIVACY_DUE (s_hidden_wanted != s_hidden_sent && time(NULL) >= s_privacy_retry)
        while (!s_queued && !s_search_due && !s_refresh_now && !s_report.due && !PRIVACY_DUE && time(NULL) < next_refresh) {
            struct timespec until = {next_refresh, 0};
            pthread_cond_timedwait(&s_wake, &s_lock, &until);
        }
        if (PRIVACY_DUE) {
            privacy = s_hidden_wanted;
        } else if (s_report.due) {
            s_report.due = 0;
            snprintf(who, sizeof who, "%s", s_report.who);
            snprintf(report_reason, sizeof report_reason, "%s", s_report.reason);
            snprintf(report_comment, sizeof report_comment, "%s", s_report.comment);
        } else if (s_queued) {
            snprintf(action, sizeof action, "%s", s_queue[0].action);
            snprintf(who, sizeof who, "%s", s_queue[0].who);
            memmove(s_queue, s_queue + 1, sizeof s_queue[0] * (size_t)--s_queued);
        } else if (s_search_due) {
            s_search_due = 0;
            snprintf(query, sizeof query, "%s", s_view.search_query);
        } else {
            refresh_now = 1;
            s_refresh_now = 0;
        }
        pthread_mutex_unlock(&s_lock);
        if (privacy >= 0) {
            char target[160], body[64];
            snprintf(target, sizeof target, "/ryunet/friends/privacy?name=%s&hidden=%d&format=text", me, privacy);
            const int status = runtime_ryunet_request("POST", target, body, sizeof body);
            runtime_net_trace("Friends: appear offline = %d -> %d", privacy, status);
            pthread_mutex_lock(&s_lock);
            if (status == 200) s_hidden_sent = privacy;
            else s_privacy_retry = time(NULL) + REFRESH_SECONDS;
            pthread_mutex_unlock(&s_lock);
        } else if (report_reason[0]) {
            report(me, who, report_reason, report_comment);
        } else if (action[0]) {
            act(me, action, who);
            pthread_mutex_lock(&s_lock);
            s_refresh_now = 1; /* show what the action changed */
            pthread_mutex_unlock(&s_lock);
        } else if (query[0]) {
            search(me, query);
        } else if (refresh_now) {
            refresh(me);
            next_refresh = time(NULL) + REFRESH_SECONDS;
            static int migrated;
            if (!migrated && s_view.state == RF_READY) {
                migrated = 1;
                migrate_old_friends(me);
            }
        }
    }
    return NULL;
}

void runtime_friends_start(void) {
    pthread_mutex_lock(&s_lock);
    const int start = !s_started && runtime_net_is_online() && getenv("BB_ONLINE_KEYHASH") &&
                      runtime_np_get_online_name() && *runtime_np_get_online_name();
    if (start) {
        s_started = 1;
        s_view.state = RF_WAITING;
        s_refresh_now = 1;
    }
    pthread_mutex_unlock(&s_lock);
    if (!start) return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, friends_thread, NULL) == 0) {
        pthread_detach(thread);
        runtime_net_trace("Friends: started");
    }
}

unsigned runtime_friends_serial(void) {
    pthread_mutex_lock(&s_lock);
    const unsigned serial = s_view.serial;
    pthread_mutex_unlock(&s_lock);
    return serial;
}

void runtime_friends_view(RfView *out) {
    pthread_mutex_lock(&s_lock);
    *out = s_view;
    pthread_mutex_unlock(&s_lock);
}

void runtime_friends_action(const char *action, const char *who) {
    char name[RF_NAME];
    clean_name(name, who);
    if (!name[0] || strlen(action) >= sizeof s_queue[0].action) return;
    pthread_mutex_lock(&s_lock);
    if (s_started && s_queued < MAX_QUEUE) {
        snprintf(s_queue[s_queued].action, sizeof s_queue[0].action, "%s", action);
        snprintf(s_queue[s_queued].who, RF_NAME, "%s", name);
        s_queued++;
        s_view.result[0] = '\0';
        s_view.serial++;
        pthread_cond_signal(&s_wake);
    }
    pthread_mutex_unlock(&s_lock);
}

void runtime_friends_report(const char *who, const char *reason, const char *comment) {
    char name[RF_NAME];
    clean_name(name, who);
    if (!name[0] || !reason || strlen(reason) >= sizeof s_report.reason) return;
    pthread_mutex_lock(&s_lock);
    if (s_started && !s_view.reporting) {
        snprintf(s_report.who, RF_NAME, "%s", name);
        snprintf(s_report.reason, sizeof s_report.reason, "%s", reason);
        snprintf(s_report.comment, sizeof s_report.comment, "%s", comment ? comment : "");
        s_report.due = 1;
        s_view.reporting = 1;
        s_view.report_result[0] = '\0';
        s_view.serial++;
        pthread_cond_signal(&s_wake);
    }
    pthread_mutex_unlock(&s_lock);
}

void runtime_friends_search(const char *query) {
    char q[RF_NAME];
    clean_name(q, query);
    pthread_mutex_lock(&s_lock);
    snprintf(s_view.search_query, RF_NAME, "%s", q);
    s_view.search_count = 0;
    s_view.searching = s_started && strlen(q) >= 2;
    s_search_due = s_view.searching;
    s_view.serial++;
    pthread_cond_signal(&s_wake);
    pthread_mutex_unlock(&s_lock);
}

void runtime_friends_set_hidden(int hidden) {
    pthread_mutex_lock(&s_lock);
    if (s_hidden_wanted != !!hidden) {
        s_hidden_wanted = !!hidden;
        pthread_cond_signal(&s_wake);
    }
    pthread_mutex_unlock(&s_lock);
}

void runtime_friends_join(const char *host) {
    char name[RF_NAME];
    clean_name(name, host);
    if (name[0]) runtime_invite_join(name);
}

int runtime_friends_take_message(char text[RF_MESSAGE]) {
    pthread_mutex_lock(&s_lock);
    const int got = s_msg_count > 0;
    if (got) {
        snprintf(text, RF_MESSAGE, "%s", s_msgs[s_msg_head]);
        s_msg_head = (s_msg_head + 1) % MAX_MESSAGES;
        s_msg_count--;
    }
    pthread_mutex_unlock(&s_lock);
    return got;
}

int runtime_friends_take_event(char name[RF_NAME]) {
    pthread_mutex_lock(&s_lock);
    int kind = 0;
    if (s_event_count) {
        kind = s_events[s_event_head].kind;
        snprintf(name, RF_NAME, "%s", s_events[s_event_head].name);
        s_event_head = (s_event_head + 1) % MAX_EVENTS;
        s_event_count--;
    }
    pthread_mutex_unlock(&s_lock);
    return kind;
}

const char *runtime_friends_names(unsigned *serial) {
    static char copy[2][sizeof s_names];
    static int cur;
    pthread_mutex_lock(&s_lock);
    if (serial) *serial = s_names_serial;
    cur ^= 1;
    memcpy(copy[cur], s_names, sizeof s_names);
    pthread_mutex_unlock(&s_lock);
    return copy[cur];
}

int runtime_friends_invite_online(void) {
    pthread_mutex_lock(&s_lock);
    if (!s_started || s_view.state != RF_READY) {
        pthread_mutex_unlock(&s_lock);
        return 0;
    }
    int invited = 0;
    const char *first = "";
    for (int i = 0; i < s_view.friend_count && s_queued < MAX_QUEUE; i++) {
        if (!s_view.friends[i].online) continue;
        snprintf(s_queue[s_queued].action, sizeof s_queue[0].action, "invite");
        snprintf(s_queue[s_queued].who, RF_NAME, "%s", s_view.friends[i].name);
        s_queued++;
        if (!invited++) first = s_view.friends[i].name;
    }
    if (invited == 1) {
        push_event_locked(RF_EVENT_INVITED, first);
    } else if (invited) {
        char count[RF_NAME];
        snprintf(count, sizeof count, "+%d", invited);
        push_event_locked(RF_EVENT_INVITED, count);
    } else {
        push_event_locked(RF_EVENT_NO_ONE_ONLINE, "");
    }
    pthread_cond_signal(&s_wake);
    pthread_mutex_unlock(&s_lock);
    runtime_net_trace("Friends: the game's Invite button invited %d friend(s) online", invited);
    return 1;
}
