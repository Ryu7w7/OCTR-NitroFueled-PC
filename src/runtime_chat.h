#ifndef RUNTIME_CHAT_H
#define RUNTIME_CHAT_H

/* The text chat of a room (runtime_chat.c), never during a race: a private room's in the lobby and on the results, a
 * public room's in the lobby only. */

#ifdef __cplusplus
extern "C" {
#endif

#define RC_LINES 40
#define RC_NICK 17
#define RC_TEXT 644  /* 160 letters of up to 4 bytes */
#define RC_SEND 324  /* what the menu lets a player type (the server keeps 160 letters) */

enum { RC_STATUS_OK, RC_STATUS_WAIT, RC_STATUS_REFUSED, RC_STATUS_FAILED };

typedef struct {
    unsigned id;
    char nick[RC_NICK];
    char text[RC_TEXT];
} RcLine;

void runtime_chat_start(void);
/* Off: nothing shown or sent (a host still tells the server who is in its room, for the others). */
void runtime_chat_set_enabled(int on);
/* The public lobbies' chat, a choice of its own. */
void runtime_chat_set_public(int on);
/* 1 where the chat works now (in a public lobby, once the server says this player is in the room). */
int runtime_chat_available(void);
unsigned runtime_chat_serial(void);
/* The newest lines, oldest first, without the muted players'. The serial changes with them. */
unsigned runtime_chat_view(RcLine *out, int max, int *count, int *status);
int runtime_chat_send(const char *text);
/* Kept between games (chat_muted.txt, next to the saves). */
void runtime_chat_mute(const char *nick, int muted);
int runtime_chat_muted(const char *nick);

#ifdef __cplusplus
}
#endif

#endif
