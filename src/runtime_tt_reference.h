#ifndef BB_TT_REFERENCE_H
#define BB_TT_REFERENCE_H
#include <stdint.h>
#define TT_REFERENCE_ALIGNED 1024u
#define TT_REFERENCE_DRIFT_HOP 2048u
#define TT_REFERENCE_BANK_BEFORE_HOP 4096u
#define TT_REFERENCE_BELOW_ROUTE 16384u
#define TT_REFERENCE_REJOIN_READY 32768u
#define TT_REFERENCE_BOX_BLOCK (1u << 24)
/* Owned by race simulation. This module changes targets, never entity transforms. */
int runtime_tt_reference_load(const char *path);
int runtime_tt_reference_guide(const char *map, uintptr_t owner, unsigned id, int lap, int level,
                              const float position[3], const float velocity[3],
                              float targets[2][3], unsigned *intent);
float runtime_tt_reference_boost_interval(unsigned id, unsigned completed);
#endif
