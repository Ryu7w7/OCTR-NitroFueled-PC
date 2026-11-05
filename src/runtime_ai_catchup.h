#ifndef RUNTIME_AI_CATCHUP_H
#define RUNTIME_AI_CATCHUP_H
#include <stddef.h>
#include <stdint.h>
/* RYUPN1 kind 3/4: older ping implementations discard these control packets. */
#define AI_CATCHUP_PACKET 48
uint64_t runtime_ai_catchup_now(void);
void runtime_ai_catchup_reset(const char *map, const char *host, int authority);
/* Required remote participants; zero for an offline/local-only race. */
void runtime_ai_catchup_require_peers(int required);
int runtime_ai_catchup_ready(void);
int runtime_ai_catchup_discovery(void);
void runtime_ai_catchup_publish(unsigned id, float bonus);
void runtime_ai_catchup_level_publish(int level);
int runtime_ai_catchup_level(void);
void runtime_ai_catchup_recovery_publish(unsigned id, int active);
int runtime_ai_catchup_recovery(unsigned id);
/* Final Team result bits follow validated finish order; -1 means not received. */
void runtime_ai_catchup_results_publish(unsigned finished_mask);
int runtime_ai_catchup_results(void);
float runtime_ai_catchup_bonus(unsigned id);
size_t runtime_ai_catchup_send(const char *peer, uint8_t *out, uint64_t now);
int runtime_ai_catchup_receive(const char *peer, const uint8_t *in, size_t len, uint64_t now);
float runtime_ai_catchup_target(int level, float gap, float human_speed, float ai_speed, float previous);
#endif
