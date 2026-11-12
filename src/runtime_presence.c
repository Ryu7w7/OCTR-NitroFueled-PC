/* What the player is doing, for the launchers' Discord Rich Presence (OCTR v0.8.5).
 *
 * Once a second, on the game's thread (runtime_replay_tick, the pad read), the facts below go to
 * BB_PRESENCE_FILE (run.py: out/presence.txt) when one of them changed, as key=value lines:
 *   game=     the game mode: CClient (0x2ab3cd8) +0x90, EGameMode: Story 1 (Adventure), Arcade 8,
 *             OnlinePlay 16
 *   race=     the race mode: the race manager (0x2acde48) +0xd40, EOctaneRaceModes: 1 SingleRace,
 *             2 TimeTrial, 3 RelicRace, 4 BossFight, 5 CtrChallenge, 6 CupRace, 7 HUB,
 *             8 CrystalChallenge, 9 BattleArena .. 13 CrystalGrabBattle, 14 Sports, 15 ModeR
 *   level=    the level loaded (its zone's name, runtime_replay.c rr_current_level)
 *   room=     public or private while in an online match or room, else empty
 *   lobby=    1 while an online lobby is on screen
 *   itemless= 1 in an Itemless match; speed= 1 in a Speed race
 * The launchers turn them into text (and the zone into the track's own name, scripts/track_names.py). */
#include "runtime.h"
#include "runtime_replay_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define D_CLIENT 0x2ab3cd8
#define D_RACE_MANAGER 0x2acde48

static char last[512];
static time_t last_at;

static int int_at(void *object, size_t offset) {
    int value = 0;
    if (object) memcpy(&value, (char *)object + offset, sizeof value);
    return value;
}

void runtime_presence_tick(void) {
    static const char *path;
    static int checked;
    if (!checked) {
        checked = 1;
        path = getenv("BB_PRESENCE_FILE");
        if (path && !*path) path = NULL;
    }
    if (!path) return;
    const time_t now = time(NULL);
    if (now == last_at) return;                  /* once a second */
    last_at = now;
    const int game = int_at(rr_ptr(D_CLIENT), 0x90);
    const int race = int_at(rr_ptr(D_RACE_MANAGER), 0xd40);
    const char *level = rr_current_level();
    const char *room = runtime_online_mode_room_on() ? "private"
                       : runtime_public_match_active() ? "public" : "";
    char text[512];
    snprintf(text, sizeof text, "game=%d\nrace=%d\nlevel=%.200s\nroom=%s\nlobby=%d\nitemless=%d\nspeed=%d\n",
             game, race, level ? level : "", room, runtime_online_mode_lobby_open(),
             runtime_public_itemless_active(), runtime_speed_race_active());
    if (!strcmp(text, last)) return;
    snprintf(last, sizeof last, "%s", text);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}
