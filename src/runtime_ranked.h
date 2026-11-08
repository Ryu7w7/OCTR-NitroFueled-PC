#ifndef RUNTIME_RANKED_H
#define RUNTIME_RANKED_H

/* Ranked race results: what this module sends and shows. The racer struct
 * lives in runtime.h (runtime_online_mode.c fills it). */

#ifdef __cplusplus
extern "C" {
#endif

/* The worker thread that posts reports and reads answers (runtime_net.c). */
void runtime_ranked_start(void);

/* Game thread, at the validated host results event: hands the finish order to
 * the worker. 0 when this race is not a ranked race. */
void runtime_ranked_race_end(void);

/* The lobby card's suffix for the last scored race in this room ("" none). */
const char *runtime_ranked_result_text(void);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_RANKED_H */
