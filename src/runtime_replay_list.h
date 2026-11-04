#ifndef RUNTIME_REPLAY_LIST_H
#define RUNTIME_REPLAY_LIST_H

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
/* 1 while a replay is loading or playing. */
int runtime_replay_playing(void);

#ifdef __cplusplus
}
#endif

#endif
