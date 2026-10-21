#ifndef RUNTIME_FRIENDS_H
#define RUNTIME_FRIENDS_H

/* RyuNET friends (the server's ryunet_friends.go): the in-game menu's Friends tab, the
 * notices "a friend is online / sent a request / invites you", the friend list the game
 * shows, and the game's Invite button. */

#ifdef __cplusplus
extern "C" {
#endif

#define RF_NAME 17
#define RF_MAX_FRIENDS 200
#define RF_MAX_PENDING 50
#define RF_MAX_INVITES 16
#define RF_MAX_SEARCH 10

enum {
    RF_OFF = 0,         /* offline, or no key for the server */
    RF_WAITING = 1,     /* no answer yet */
    RF_READY = 2,
    RF_REFUSED = 3,     /* the server refused: nickname not registered, or an older server */
    RF_UNREACHABLE = 4, /* the last request got no answer */
};

typedef struct { char name[RF_NAME]; int online, players, max; } RfFriend;
typedef struct { char name[RF_NAME]; int online; char relation[12]; } RfPlayer; /* none friend sent received */
typedef struct { char from[RF_NAME]; int ago; } RfInvite;

typedef struct {
    unsigned serial; /* changes with every update */
    int state;
    int friend_count, incoming_count, outgoing_count, invite_count, search_count;
    RfFriend friends[RF_MAX_FRIENDS];
    char incoming[RF_MAX_PENDING][RF_NAME];
    char outgoing[RF_MAX_PENDING][RF_NAME];
    RfInvite invites[RF_MAX_INVITES];
    RfPlayer search[RF_MAX_SEARCH];
    char search_query[RF_NAME];
    int searching;    /* a search is on its way */
    char result[24];  /* the server's word for the last action: sent, friends, removed, busy... */
    char result_who[RF_NAME];
    int reporting;            /* a report is on its way */
    char report_result[16];   /* sent, already, limit, unknown, self, refused, error ("" before any) */
} RfView;

/* Started by runtime_net_init when online with a key; safe to call again. */
void runtime_friends_start(void);
unsigned runtime_friends_serial(void);
void runtime_friends_view(RfView *out);

/* Asynchronous; the view changes when the server answers. action: request, accept, decline,
 * remove, invite. */
void runtime_friends_action(const char *action, const char *who);
void runtime_friends_search(const char *query);

/* Report a player to the server's administrators (the server's moderation.go): reason is
 * cheating, name, behavior or other; comment is free text (UTF-8, at most RF_REPORT_COMMENT - 1
 * bytes). The answer comes in the view's report_result. */
#define RF_REPORT_COMMENT 400
void runtime_friends_report(const char *who, const char *reason, const char *comment);

/* Appear offline to friends (the menu's "Don't show my activity"); sent to the server when it
 * changes, and once at start. */
void runtime_friends_set_hidden(int hidden);

/* Join the room of a friend who invited this player (as a Discord invitation does). */
void runtime_friends_join(const char *host);

/* RF_EVENT_INVITED: name is the friend invited, or "+N" for N friends; RF_EVENT_NO_ONE_ONLINE:
 * the game's Invite button found no friend online. */
enum {
    RF_EVENT_ONLINE = 1, RF_EVENT_REQUEST = 2, RF_EVENT_INVITE = 3, RF_EVENT_ACCEPTED = 4,
    RF_EVENT_INVITED = 5, RF_EVENT_NO_ONE_ONLINE = 6,
};
/* The next notice to show, 0 when none. */
int runtime_friends_take_event(char name[RF_NAME]);

/* The next message from the server's administrators, 0 when none. The server's
 * announcement goes to the game's announcement banner instead (ryunet_announce.txt). */
#define RF_MESSAGE 256
int runtime_friends_take_message(char text[RF_MESSAGE]);

/* The friends' names, comma-separated (the game's friend list), and when they changed. */
const char *runtime_friends_names(unsigned *serial);

/* The game's Invite button (sceInvitationDialogOpenA): every friend online is invited, with a
 * notice. 0 when the friends list is not available (offline, not registered). */
int runtime_friends_invite_online(void);

#ifdef __cplusplus
}
#endif

#endif
