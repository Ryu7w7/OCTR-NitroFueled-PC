#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_itemless.c"
static int active, responses, removals, nested;
static unsigned char owner[32], other[32], ai[32], ai2[32], obstacle[32], human_racer[32], ai_racer[32];
static unsigned char boost[32], wall_context[40], collision_data[16];
static int boost_alive;
int runtime_speed_race_active(void) { return active; }
static void put(void *p, size_t off, void *v) { memcpy((char *)p+off, &v, sizeof v); }
static void *ABI lookup(void *manager, void *entity) {
    (void)manager;
    if (entity == owner || entity == other) return human_racer;
    return entity == ai || entity == ai2 ? ai_racer : NULL;
}
static void ABI remove_boost(void *b) { assert(b == boost); ++removals; boost_alive = 0; }
static void ABI respond(void *self, void *ctx, void *c) {
    (void)ctx; (void)c;
    ++responses;
    if (nested) {
        nested = 0;
        contact(self, wall_context, collision_data);
        assert(contact_owner == owner);
    }
    stop_boost(boost);
}
int main(void) {
    unsigned char self[32] = {0}, context[40] = {0}, world[40] = {0}, bodies[0x120] = {0};
    void *manager = self;
    racers = &manager; find_racer = lookup; native_contact = respond; native_stop = remove_boost;
    put(human_racer, 0x10, human_racer);
    put(self, 0x18, owner); put(boost, 0x18, owner);
    put(context, 0, world); put(world, 0x20, bodies);
    *(int *)(collision_data+12) = 1;
    bodies[0x90+0x42] = 4;
    put(bodies, 0x90+0x88, other);
    active = 1; boost_alive = 1;
    contact(self, context, collision_data);
    assert(responses == 1 && removals == 0 && boost_alive && !contact_owner);
    active = 0;
    contact(self, context, collision_data);
    assert(responses == 2 && removals == 1 && !boost_alive);
    active = 1; boost_alive = 1;
    put(bodies, 0x90+0x88, ai);
    contact(self, context, collision_data);
    assert(removals == 1 && boost_alive); /* Player -> AI. */
    put(self, 0x18, ai); put(boost, 0x18, ai);
    put(bodies, 0x90+0x88, owner);
    contact(self, context, collision_data);
    assert(removals == 1 && boost_alive); /* AI -> player. */
    put(bodies, 0x90+0x88, ai2);
    contact(self, context, collision_data);
    assert(removals == 1 && boost_alive); /* AI -> AI, no human handles. */
    put(self, 0x18, owner); put(boost, 0x18, owner);
    put(bodies, 0x90+0x88, obstacle);
    contact(self, context, collision_data);
    assert(removals == 2 && !boost_alive); /* Non-racer entity. */
    put(bodies, 0x90+0x88, other);
    bodies[0x90+0x42] = 0;
    contact(self, context, collision_data);
    assert(removals == 3);
    bodies[0x90+0x42] = 4;
    nested = 1;
    contact(self, context, collision_data);
    assert(removals == 4 && !contact_owner); /* Nested wall contact remains native. */
    assert(responses == 9); /* Every collision still reaches native response. */
    stop_boost(boost);
    assert(removals == 5); /* No protection leaks into later contacts. */
    puts("Itemless contact tests passed");
}
