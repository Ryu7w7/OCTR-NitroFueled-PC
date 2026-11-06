#ifndef RUNTIME_NAME_COLORS_H
#define RUNTIME_NAME_COLORS_H

/* The nicknames with a color of their own (runtime_name_colors.c), from the server. */

#ifdef __cplusplus
extern "C" {
#endif

void runtime_name_colors_start(void);
/* 1 for a nickname with a color, with the color it has now; any thread, every frame. */
int runtime_name_color(const char *nick, float rgb[3]);

#ifdef __cplusplus
}
#endif

#endif
