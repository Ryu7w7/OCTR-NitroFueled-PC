#ifndef RUNTIME_REPLAY_INTERNAL_H
#define RUNTIME_REPLAY_INTERNAL_H

/* Shared by the native recorder (runtime_replay.c) and player (runtime_replay_play.c). */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include "runtime.h"

#define RR_AT(object, offset, type) (*(type *)((char *)(object) + (offset)))
static inline int rr_count(void *list) { return list ? RR_AT(list, 0xc, int32_t) : 0; }
static inline void *rr_item(void *list, int i) { return RR_AT(list, 0x20, void **)[i]; }

/* the game's data (USA and EU 1.21: the same addresses), read as pointers */
enum {
    RR_D_GAME = 0x2acde48, RR_D_FRAME = 0x2ab4100, RR_D_KART_MANAGER = 0x2ac57c0, RR_D_KART_CONTROLLER_META = 0x2ac4580,
    RR_D_RACER_MANAGER = 0x2ad4b70, RR_D_CUSTOMIZATION = 0x2ac4ec8, RR_D_BOOST_META = 0x2ac4018,
    RR_D_POWER_UP_META = 0x2addd90, RR_D_HORN_META = 0x2add660, RR_D_DAMAGE_META = 0x2ac5018,
    RR_D_RESPAWN_META = 0x2ac5948, RR_D_ZONE_SYSTEM = 0x2ae3c18, RR_D_RACE_SETTINGS = 0x2ad4e30,
    RR_D_RACE_CLOCK_META = 0x2ad3ec8, RR_D_SYSTEM_DATA = 0x2ad9c38, RR_D_GHOST_MANAGER = 0x2abc3d0,
    RR_D_SPAWNED_META = 0x2ad70d8,
};
#define RR_DRIVERS 0x130
#define RR_KARTS 0x150

typedef int32_t ABI rr_now_fn(void *timer);

unsigned char *rr_game(size_t offset);      /* an address in the game's image */
void *rr_ptr(size_t offset);                /* the pointer stored there */
void *rr_handle_object(void **slot);
void *rr_kart_of(int i, int *player_id);
void *rr_component(void *kart, int meta_at);
void *rr_find_component(void *entity, void *meta);
void *rr_running_boost(void *boosts);
float rr_boost_length(void *boost);
void *rr_world(void);
float rr_race_clock(void);
void *rr_game_data(int list, const char *name);
int rr_item_type(void *handle);              /* a stored item's EPowerUpType, -1 not known */
void rr_race_dir(char *out, size_t size);
const char *rr_current_level(void);         /* the level loaded now, 0 none */
uint32_t rr_world_id(void);
/* Replaces the call at `site` (to `expected`) by a call to `target`, through a trampoline. 1 = done. */
int rr_redirect_call(unsigned char *site, const unsigned char *expected, void (*target)(void));

/* the player (runtime_replay_play.c) */
int runtime_replay_play_install(int eu);
int runtime_replay_playing(void);
void runtime_replay_play_frame(void);
void runtime_replay_play_level_end(void);
void ABI runtime_replay_level_start(void *manager);
int ABI runtime_replay_ai_input(void *ai);
int runtime_replay_play_file(const char *name);

#endif
