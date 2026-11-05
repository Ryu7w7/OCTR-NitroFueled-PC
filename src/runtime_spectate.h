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
};

void runtime_spectate_start(void);
/* Share this player's races with their friends (bbport.ini spectate_share). */
void runtime_spectate_set_share(int share);
/* Download the newest race the friend shared. */
void runtime_spectate_watch(const char *friend_name);
/* The last watch request: its state (RS_*) and friend; serial changes with every update. */
unsigned runtime_spectate_status(int *state, char *friend_name, int size);

#ifdef __cplusplus
}
#endif

#endif
