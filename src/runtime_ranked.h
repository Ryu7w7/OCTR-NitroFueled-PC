#ifndef RUNTIME_RANKED_H
#define RUNTIME_RANKED_H

/* Ranked race results: what this module sends and shows. The racer struct
 * lives in runtime.h (runtime_online_mode.c fills it). */

#ifdef __cplusplus
extern "C" {
#endif

/* One race's report, owned by its sender thread (runtime_ranked.c). */
struct ranked_outbox {
    uint64_t room, generation;
    int mode;                    /* 0 Normal, 1 Itemless */
    int count;                   /* racers in the order, 2..8 */
    char track[48];
    struct runtime_ranked_racer order[8];
};

/* The sender thread's body: posts the outbox with its retry budget and leaves
 * the delta for the lobby card. Exposed so the test can run it inline. */
void *ranked_sender(void *arg);

/* Start the background Ranked status/profile worker from runtime_net.c. */
void runtime_ranked_start(void);

/* Game thread, at the validated host results event: hands the finish order to
 * a sender thread. 0 when this race is not a ranked race. */
void runtime_ranked_race_end(void);

struct runtime_ranked_result {
    uint64_t room, generation;
    int mode, ready, ranked, ranked_before;
    int mmr, before, delta, tier, tier_before, placements;
};
void runtime_ranked_race_begin(void);
int runtime_ranked_result_copy(struct runtime_ranked_result *out);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_RANKED_H */
