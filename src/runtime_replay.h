#ifndef RUNTIME_REPLAY_H
#define RUNTIME_REPLAY_H

/* Native race replays (spectator build): the recorder. Files keep the format of Erluinzo's Replay Mode
 * (github.com/Erluinzo/ctrnf-replay-mode, MIT), so they play in it and in OCTR's own player. */

#include <stddef.h>
#include <stdint.h>

#define RR_MAGIC "CTRRPLAY"
#define RR_VERSION 5
#define RR_RACERS 8
#define RR_RATE 60
#define RR_MAX_FRAMES (RR_RATE * 60 * 31)
#define RR_KEEP_AUTO 50

struct rr_racer {
    char name[32];
    char driver[32];
    char kart[32];
    int8_t player_id;           /* -1: AI */
    uint8_t kind;               /* RR_* below; 0 = empty slot */
    uint8_t finished;           /* finish rank (1..8), 0 = did not finish */
    uint8_t look_filled;
    uint16_t look[6];           /* [0] wheels [1] paint [2] decal [3] sticker [4] ? [5] outfit index + 1 */
    float finish_time;
};
_Static_assert(sizeof(struct rr_racer) == 116, "racer layout");
enum { RR_EMPTY, RR_LOCAL, RR_REMOTE, RR_AI };

struct rr_header {
    char magic[8];
    uint32_t version, header_size, frame_size, frames;
    float go_time;              /* recording time of GO, s (-1 not yet) */
    float duration;
    int32_t mode;
    int32_t laps;
    char level[64];
    char player[32];
    uint8_t date[8];            /* year - 2000, month, day, hour, minute, second (UTC) */
    struct rr_racer racer[RR_RACERS];
    uint32_t packed;
    char track[40];
    uint8_t mirror;
    uint8_t reserved[19];
};

#define RR_POSITION 16.0f
#define RR_VELOCITY 4.0f
#define RR_STEER 16384.0f
struct rr_sample {
    int32_t position[3];
    int16_t velocity[3];
    int16_t angles[3];
    uint16_t flags;
    uint8_t rank;
    uint8_t lap;
    uint32_t buttons;
    int16_t steer;
    int8_t item;
    uint8_t item_count;
    uint16_t reserves;
    uint16_t boost_speed;
    uint8_t wumpa;
    uint8_t drift_state;
    uint8_t damage;
    uint8_t juiced;
    uint16_t slide_timer;
    uint16_t air_time;
    uint16_t ghost_bits;
    uint8_t respawn;
    uint8_t item_uses;
};
_Static_assert(sizeof(struct rr_sample) == 52, "sample layout");
enum { RS_PRESENT = 1, RS_FINISHED = 2, RS_ON_GROUND = 4, RS_BOOSTING = 8, RS_DRIFTING = 16, RS_HONK = 32,
       RS_SHUFFLING = 64, RS_WRONG_WAY = 128, RS_PAD_BOOST = 256, RS_DRIFT_BOOST = 512, RS_TNT = 1024, RS_SHIELD = 2048 };

struct rr_frame {
    float time;
    float race_time;
    struct rr_sample racer[RR_RACERS];
};

/* Installs the recorder's hooks (probe.c, before the game runs). 1 = recording, 0 = not this game. */
int runtime_replay_start(unsigned char *image, size_t size);

/* Packing of the replay file (shared with the player and the live stream). */
uint8_t *rr_pack_frame(uint8_t *out, const struct rr_frame *f, const struct rr_frame *p1, const struct rr_frame *p2);
int rr_unpack_frame(struct rr_frame *f, const struct rr_frame *p1, const struct rr_frame *p2,
                    const uint8_t **in, const uint8_t *end, int version);

#endif
