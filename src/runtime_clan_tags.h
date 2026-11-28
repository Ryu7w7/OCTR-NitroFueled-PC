/* Clan tags in front of the players' names (runtime_clan_tags.c). SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RUNTIME_CLAN_TAGS_H
#define RUNTIME_CLAN_TAGS_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The nickname's clan tag (2 or 3 letters or digits, "" none) as far as known; asks the server when it is not
 * (never blocks). 1 when the server has answered for it. */
int runtime_clan_lookup(const char *nick, char tag[4]);
/* "(MC) Ryu7w7", or the nickname as it is while it has no tag (or it is not known yet). */
void runtime_clan_label(const char *nick, char *label, size_t cap);
/* The nickname inside a shown name ("(MC) Ryu7w7" or "[MC] Ryu7w7" -> "Ryu7w7", copied into account); any other text as it is. */
const char *runtime_clan_identity(const char *text, char account[17]);

#ifdef __cplusplus
}
#endif
#endif
