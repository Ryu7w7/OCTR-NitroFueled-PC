/* cc -D_GNU_SOURCE -std=c11 -O2 -ffunction-sections -fdata-sections -Isrc
 * tests/test_item_lead_bomb.c -Wl,--gc-sections -lm -lpthread -o /tmp/test-item-lead-bomb
 * Another player's bomb (src/runtime_item_lead.c): led at its own rolling speed (+0x8c), not at the
 * network steps' speed, by a lead of its own, and not at all with BB_ITEM_LEAD_BOMB=0. */
#include <assert.h>
#include <time.h>
static double test_time;
static int test_clock_gettime(clockid_t clock, struct timespec *ts) {
    (void)clock;
    ts->tv_sec = (time_t)test_time;
    ts->tv_nsec = (long)((test_time - ts->tv_sec) * 1e9 + 0.5);
    return 0;
}
#define clock_gettime test_clock_gettime
#include "../src/runtime_item_lead.c"
#undef clock_gettime

void *rr_ptr(size_t offset) { (void)offset; return NULL; }
void *rr_handle_object(void **slot) { (void)slot; return NULL; }
void *rr_racer_of(void *kart) { (void)kart; return NULL; }
int runtime_replay_playing(void) { return 0; }
int runtime_net_ping_view(PingPeer *out, int max) { (void)out; (void)max; return 0; }

/* The network moves the bomb along x at `steps` units/s, 60 frames a second, for `seconds`; returns how
 * far ahead of the network's position it was drawn at the end. */
static float run(char *component, char *entity, float rolls, float steps, int bombs, double seconds) {
    memset(items, 0, sizeof items);
    memset(component, 0, 0xc0);
    memset(entity, 0, 0x40);
    RR_AT(component, 0x18, void *) = entity;
    RR_AT(component, 0x8c, float) = rolls;
    Tracked *t = &items[0];
    t->component = component, t->entity = entity, t->kind = BOMB, t->cap = BOMB_FALLBACK_CAP;
    cfg.on = 1, cfg.bombs = bombs;
    test_time = 0;
    float network = 0.0f;
    for (int i = 1; i <= (int)(seconds * 60); i++) {
        test_time = i / 60.0;
        network = steps * (float)test_time;
        RR_AT(entity, 0x20, float) = network;   /* this frame's position from the network */
        lead_item(component, BOMB);
    }
    return RR_AT(entity, 0x20, float) - network;
}

int main(void) {
    _Alignas(16) char component[0xc0], entity[0x40];
    cfg.fixed_ms = 200;
    /* network steps say 6000 a second, the bomb rolls at 4000: drawn 4000 x 0.2 s ahead, not 6000 x 0.2 */
    float ahead = run(component, entity, 4000.0f, 6000.0f, 1, 1.0);
    assert(fabsf(ahead - 800.0f) < 20.0f);
    assert(items[0].rolls == 4000.0f && items[0].jumps == 0);
    /* thrown backwards: the speed is negative, its size counts */
    ahead = run(component, entity, -4000.0f, 4000.0f, 1, 1.0);
    assert(fabsf(ahead - 800.0f) < 20.0f);
    /* its speed unknown: no faster than the fallback cap */
    ahead = run(component, entity, 0.0f, 9000.0f, 1, 1.0);
    assert(ahead <= BOMB_FALLBACK_CAP * 0.2f + 20.0f);
    /* a network burst (two frames' steps late, then 700 units at once): not a jump, the lead stays */
    ahead = run(component, entity, 4000.0f, 4000.0f, 1, 1.0);
    const int jumps = items[0].jumps;
    float network = RR_AT(entity, 0x20, float) - ahead;
    test_time += 1 / 60.0;
    RR_AT(entity, 0x20, float) = network;           /* stalled */
    lead_item(component, BOMB);
    test_time += 1 / 60.0;
    network += 700.0f;
    RR_AT(entity, 0x20, float) = network;           /* the burst */
    lead_item(component, BOMB);
    assert(items[0].jumps == jumps);
    assert(RR_AT(entity, 0x20, float) - network > 600.0f);
    /* a real teleport (spawn, removal): a jump, and the drawn offset is kept meanwhile */
    test_time += 1 / 60.0;
    network += 20000.0f;
    RR_AT(entity, 0x20, float) = network;
    lead_item(component, BOMB);
    assert(items[0].jumps == jumps + 1);
    assert(RR_AT(entity, 0x20, float) - network > 600.0f);
    /* BB_ITEM_LEAD_BOMB=0: where the network has it */
    ahead = run(component, entity, 4000.0f, 4000.0f, 0, 1.0);
    assert(ahead == 0.0f);
    /* no fixed lead and no round trip known (no ping list): not led */
    cfg.fixed_ms = -1;
    ahead = run(component, entity, 4000.0f, 4000.0f, 1, 1.0);
    assert(ahead == 0.0f);
    puts("item lead bomb: ok");
}
