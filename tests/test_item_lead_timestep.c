/* cc -D_GNU_SOURCE -std=c11 -O2 -ffunction-sections -fdata-sections -Isrc
 * tests/test_item_lead_timestep.c -Wl,--gc-sections -lm -o /tmp/test-item-lead */
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

static void setup(char *component, char *entity) {
    memset(items, 0, sizeof items);
    memset(component, 0, 0xc0);
    memset(entity, 0, 0x40);
    RR_AT(component, 0x18, void *) = entity;
    Tracked *t = &items[0];
    t->component = component;
    t->entity = entity;
    t->cap = 6400;
    t->speed = 4000;
    t->has_base = 1;
    t->samples = 2;
    t->sample[0].t = -0.1;
    t->sample[0].p[0] = -640;
    t->sample[1].t = 0;
    cfg.on = 1;
    cfg.fixed_ms = 0;
    test_time = 0;
}

int main(void) {
    _Alignas(16) char component[0xc0], entity[0x40];
    const int rates[] = {30, 60, 72, 90, 120, 144, 240};
    const float reference = 6400.f - 2400.f * powf(.8f, 30);
    /* Old update-count blend reaches a different speed after the same 0.1 s. */
    assert(fabsf((6400 - 2400 * powf(.8f, 3)) - (6400 - 2400 * powf(.8f, 24))) > 1200);
    for (unsigned r = 0; r < sizeof rates / sizeof *rates; ++r) {
        setup(component, entity);
        for (int i = 1; i <= rates[r]; ++i) {
            test_time = (double)i / rates[r];
            lead_item(component, BOMB);
            if (rates[r] == 30 && i == 1) assert(fabsf(items[0].speed - 4480) < .01f);
        }
        assert(fabsf(items[0].speed - reference) < .03f);
        assert(items[0].moves == 0 && items[0].jumps == 0);
        assert(RR_AT(entity, 0x20, float) == 0); /* fixed lead disabled */
        const float before = items[0].speed;
        lead_item(component, BOMB); /* duplicate update at the same time */
        assert(items[0].speed == before);
    }
    setup(component, entity);
    for (int i = 0; i < 120; ++i) {
        test_time += i % 2 ? 1.0 / 80 : 1.0 / 240;
        lead_item(component, BOMB);
    }
    assert(fabsf(items[0].speed - reference) < .03f);
    setup(component, entity);
    test_time = .1; /* retain the original response below 30 FPS */
    lead_item(component, BOMB);
    assert(fabsf(items[0].speed - 4480) < .01f);
    puts("item lead timestep: ok");
}
