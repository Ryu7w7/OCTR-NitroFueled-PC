#ifndef RUNTIME_SPECTATE_H
#define RUNTIME_SPECTATE_H

/* Spectator build (experimental): friends watch each other's races through the Replay Mode mod.
 * See runtime_spectate.c. */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RS_IDLE = 0,
    RS_FETCHING = 1,    /* downloading a friend's race */
    RS_READY = 2,       /* saved: it is at the top of the main menu's REPLAYS list */
    RS_NONE = 3,        /* the friend has no recent race shared */
    RS_REFUSED = 4,     /* not friends, or the friend does not share */
    RS_FAILED = 5,      /* the server did not answer, or the file could not be written */
    RS_LIVE = 6,        /* watching the friend's race live */
    RS_WAITING = 7,     /* the friend is live but their race has not reached GO yet */
    RS_BUSY = 8,        /* a replay or a race is on: Spectate works from the menus */
    RS_PLAYING = 9,     /* the friend's last race is playing */
    RS_FOLLOW = 10,     /* following: the friend's next race loads by itself */
};

void runtime_spectate_start(void);
/* Share this player's races with their friends (bbport.ini spectate_share). */
void runtime_spectate_set_share(int share);
/* Spectate: the friend's live race (else the newest race they shared), then each next race of theirs by
 * itself until runtime_spectate_stop. */
void runtime_spectate_watch(const char *friend_name);
void runtime_spectate_stop(void);
/* 1 and the friend while Spectate is on */
int runtime_spectate_following(char *friend_name, int size);
/* The last watch request: its state (RS_*) and friend; serial changes with every update. */
unsigned runtime_spectate_status(int *state, char *friend_name, int size);
/* Friends watching this player's race live now, 0 when none (shown as SPECTATOR (n)). */
int runtime_spectate_viewers(void);

#ifdef __cplusplus
}
#endif

#endif
