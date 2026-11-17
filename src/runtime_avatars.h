/* Tacho: the players' Discord pictures for the menu (friends, room). The server sends each one as
 * a small PNG (GET /ryunet/avatar, ryunet_avatar.go); a background thread asks for the names the
 * menu wants, once each per session, and hands the PNGs over. */
#ifndef RUNTIME_AVATARS_H
#define RUNTIME_AVATARS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RA_NAME 17

/* The menu wants this player's picture (a nickname; case does not matter). Cheap; asks once. */
void runtime_avatar_want(const char *who);
/* A picture that arrived: 1 with who (lower case), *png (free with runtime_avatar_free) and *size;
 * *png is NULL when the player has none to show (the menu draws its own). 0 when nothing new. */
int runtime_avatar_take(char who[RA_NAME], unsigned char **png, size_t *size);
void runtime_avatar_free(unsigned char *png);

#ifdef __cplusplus
}
#endif

#endif
