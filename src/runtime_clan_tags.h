#ifndef RUNTIME_CLAN_TAGS_H
#define RUNTIME_CLAN_TAGS_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* UTF-8 bytes, including NUL. A tag has at most six grapheme clusters;
 * RyuNET owns the Unicode segmentation and moderation rules. */
enum { CLAN_TAG_CAP = 97, CLAN_LABEL_CAP = 16 + 3 + CLAN_TAG_CAP,
       CLAN_RASTER_WIDTH = 224, CLAN_RASTER_HEIGHT = 40,
       CLAN_TILE_FIRST = 0xe4e0, CLAN_TILE_LEVELS = 256,
       CLAN_TILE_ADVANCE = CLAN_TILE_FIRST + CLAN_RASTER_HEIGHT / 2 * CLAN_TILE_LEVELS,
       CLAN_TILES_CAP = (CLAN_RASTER_WIDTH / 2) * (1 + CLAN_RASTER_HEIGHT / 2) * 3 + 1,
       CLAN_VISUAL_CAP = 3 * 16 + 3 + CLAN_TILES_CAP,
       CLAN_MARKER_CAP = CLAN_VISUAL_CAP + 4 };
enum ClanSave { CLAN_IDLE, CLAN_SAVING, CLAN_SAVED, CLAN_INVALID, CLAN_OWNER, CLAN_UNAVAILABLE };
/* Nicknames remain the server identity. Only UI labels use the decorated form. */
/* A speaking racer has this space and U+E013 after the visible name. */
#define RUNTIME_NAME_SPEAKER_SUFFIX " \xee\x80\x93"
int runtime_clan_color(const char *label, float rgb[3]);
const char *runtime_clan_identity(const char *label, char account[17]);
/* Write coverage into a zeroed CLAN_RASTER_WIDTH by CLAN_RASTER_HEIGHT buffer. */
int bbgpu_clan_raster(const char *tag, unsigned char *pixels);
int bbgpu_clan_tiles(const char *tag, char out[CLAN_TILES_CAP]);
void runtime_clan_visual_label(const char *name, char *label, size_t cap);
void runtime_clan_label(const char *name, char *label, size_t cap);
int runtime_clan_lookup(const char *name, char tag[CLAN_TAG_CAP]); /* 1 when the server has answered */
enum { NAME_SWATCH_COUNT = 19, NAME_SWATCH_FIRST = 0xe050, NAME_SWATCH_STRIDE = 64 };
const char *runtime_swatch_label(int id);
unsigned runtime_swatch_rgb(int id);
int runtime_clan_swatch(const char *name); /* -1 when unavailable on this server */
int runtime_player_swatch(const char *name); /* administrator styles have priority */
enum ClanSave runtime_clan_save_profile(const char *tag, int swatch);
enum ClanSave runtime_clan_save(const char *tag);
enum ClanSave runtime_clan_status(void);
/* Game thread selects the native menu row. The overlay consumes it on its own thread. */
void runtime_clan_request_editor(int open);
int runtime_clan_editor_pending(void);
int runtime_clan_take_editor(void);
#ifdef __cplusplus
}
#endif
#endif
