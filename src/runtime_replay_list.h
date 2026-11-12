#ifndef RUNTIME_REPLAY_LIST_H
#define RUNTIME_REPLAY_LIST_H

#include <stddef.h>

/* The in-game menu's Replays tab (spectator build): the races in RaceReplay, newest first. */

#ifdef __cplusplus
extern "C" {
#endif

#define RR_LIST_MAX 64

typedef struct {
    char file[64];      /* name in RaceReplay */
    char track[40];
    char player[32];    /* the recording player */
    char when[24];      /* "2026-11-04 23:16" (UTC) */
    int place;          /* the recording player's finish place, 0 none */
    int racers, laps, mode;
    float duration;     /* s */
    long long sort;     /* seconds, for the order */
} RrListEntry;

/* Fills out with up to max entries, newest first; the number. */
int runtime_replay_list(RrListEntry *out, int max);
/* Plays one (it starts from the next frame of the game). 0 when a replay is already starting. */
int runtime_replay_play_file(const char *name);
/* 1 when a replay can start now: none is playing and no race is on (use it from the menus). */
int runtime_replay_can_start(void);
/* Why the last runtime_replay_can_start said no (its log words), 0 when it said yes: the menu tells
 * the player what to do instead of only greying the button out. */
const char *runtime_replay_blocked_reason(void);
/* 1 while a replay is loading or playing. */
int runtime_replay_playing(void);
/* The chat stays out of a race: 1 with none on, or once this player finished it. */
int runtime_replay_chat_allowed(void);
int runtime_replay_local_finished(void);
/* Managing the list. The shared folder: "replays" next to bbport.ini (made when missing), as a full path. */
void runtime_replay_folder(char *out, size_t size);
/* Copies a race of the list to the shared folder with a readable name (saved_as: that name). 1 = done. */
int runtime_replay_export(const char *file, char *saved_as, size_t size);
/* Copies the shared folder's races RaceReplay does not have yet: the number copied, -1 when there is none there. */
int runtime_replay_import(void);
/* Deletes a race of the list. 1 = done. */
int runtime_replay_delete(const char *file);

/* What the replay panel shows (taken every frame from the game's thread). */
typedef struct {
    int active;         /* the replay's race is on */
    char track[40];
    int laps;
    float time, end;    /* play time and the recording's end, s since GO */
    float speed;        /* x0.1 .. x4 */
    int paused, rewinding, forwarding, ended;
    int watch;          /* the racer the camera follows */
    int kmh;
    struct {
        char name[48];  /* the online name, else the driver's */
        int kind;       /* 1 local, 2 another PC, 3 AI, 0 none */
        int rank, lap, finished, present;
        float time;     /* its finish time (s), 0 while it races */
    } racer[8];
    int results;        /* the recorded player finished (or the replay ended): the results show */
} RrView;
/* 1 and the view while a replay's race is on. */
int runtime_replay_view(RrView *out);

/* Time Trial runs for the leaderboards (runtime_ttrun.c): a personal best saved at path goes to the server
 * when it places on its board; at startup, the Time Trials recorded before (each file once). */
void runtime_ttrun_saved(const char *path);
void runtime_ttrun_backfill(void);

#ifdef __cplusplus
}
#endif

#endif
