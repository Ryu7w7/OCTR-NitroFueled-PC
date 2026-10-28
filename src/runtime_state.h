#ifndef RUNTIME_STATE_H
#define RUNTIME_STATE_H

/* SacredFire: ryunet_state.txt next to bbport.ini (src/runtime_state.c), the launcher's Discord
 * status and the state it shows the player: online, nickname and the peers of this room. */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

void runtime_state_start(void);
void runtime_state_bind(const unsigned char *image, size_t size);
/* A bounded snapshot; empty when this executable is unsupported. */
void runtime_state_game(char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_STATE_H */
