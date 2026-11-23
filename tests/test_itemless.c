#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include "../src/runtime_itemless.c"
void runtime_race_options_stop_boost(void *b, void (ABI *stop)(void *)) { stop(b); }
static int active, responses, removals, nested;
static int private_mode, collision_enabled, public_match;
static void *departed;
int runtime_public_match_active(void) { return public_match; }
int runtime_online_mode_departed_kart(void *entity) { return entity && entity==departed; }
static unsigned char owner[32], other[32], ai[32], ai2[32], obstacle[32], human_racer[32], ai_racer[32];
static unsigned char boost[32], wall_context[40], collision_data[16];
static int boost_alive;
int runtime_speed_race_active(void) { return active; }
int runtime_online_mode_in_private(void) { return private_mode; }
int runtime_online_mode_itemless_collision(void) { return collision_enabled; }
int runtime_replay_no_crates(void) { return 0; }
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
static unsigned char ABI filter(void *rule, const char *name, int mode) {
    assert(rule == owner && name == (const char *)other);
    return (unsigned char)mode;
}

static unsigned char placed[0x28];
static void *placed_pointer;
static void ABI record_placement(void *self, void *racer, void *waypoint) {
    assert(self && racer == owner);
    placed_pointer = waypoint;
    memcpy(placed, waypoint, sizeof placed);
}

static void test_start_grid(void) {
    unsigned char self[0x20] = {0}, data[0x20] = {0}, list[0x28] = {0};
    unsigned char points[8][0x28] = {{0}}, handles[8][0x30] = {{0}};
    void *slots[8];
    put(self, 0x10, data); put(data, 0x10, list); put(list, 0x20, slots);
    const int count = 8;
    memcpy(list + 12, &count, 4);
    for (int i = 0; i < 8; ++i) {
        const float pose[] = {-100.0f + 200.0f*(i%4), 1000.0f + 20.0f*(i%4) - 400.0f*(i/4),
                              10.0f + 5.0f*(i%4), 0, 0, 1.5f + i*0.01f};
        memcpy(points[i] + 0x10, pose, sizeof pose);
        put(handles[i], 0x28, points[i]); slots[i] = handles[i];
    }
    unsigned char saved[sizeof points];
    memcpy(saved, points, sizeof points);
    native_place_racer = record_placement;
    for (active = 0; active <= 1; ++active)
        for (private_mode = 0; private_mode <= 1; ++private_mode)
            for (collision_enabled = 0; collision_enabled <= 1; ++collision_enabled)
                for (int i = 0; i < 8; ++i) {
                    place_racer(self, owner, points[i]);
                    if (active && (!private_mode || !collision_enabled)) {
                        float pose[6]; memcpy(pose, placed + 0x10, sizeof pose);
                        assert(fabsf(pose[0] - (-100.0f + 600.0f*i/7.0f)) < 0.001f);
                        assert(fabsf(pose[1] - (1000.0f + 60.0f*i/7.0f)) < 0.001f);
                        assert(fabsf(pose[2] - (10.0f + 15.0f*i/7.0f)) < 0.001f);
                        assert(pose[3] == 0 && pose[4] == 0 && pose[5] == 1.5f);
                    } else assert(placed_pointer == points[i] && !memcmp(placed, points[i], sizeof placed));
                    assert(!memcmp(points, saved, sizeof points));
                }
    active = 1; private_mode = collision_enabled = 0;
    slots[3] = NULL;
    place_racer(self, owner, points[7]);
    assert(placed_pointer == points[7]); /* Incomplete modded grid stays native. */
    active = 0;
}

static unsigned char ground_kart[0x240];
static int missing_ground;
static void *ABI ground_component(void *entity, void *type, int unused) {
    assert(entity==owner && type==other && unused==0);
    return missing_ground ? NULL : ground_kart;
}

static void test_banked_wall(void) {
    unsigned char response[32]={0};
    void *type=other;
    put(response,0x18,owner);
    get_component=ground_component; kart_controller_type=&type;
    unsigned char *code=mmap(NULL,4096,PROT_READ|PROT_WRITE|PROT_EXEC,
                              MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(code!=MAP_FAILED);
    /* Native-shaped frame: saved response, ray normal, and live XMM0/XMM2/XMM3. */
    memcpy(code,"\x55\x48\x89\xe5\x53\x48\x81\xec\x08\x01\0\0"
                "\x48\x89\xbd\x10\xff\xff\xff\xc5\xf8\x10\x1e"
                "\xc5\xf8\x11\x9d\x20\xff\xff\xff\xf3\x0f\x10\x46\x08"
                "\xb8\0\0\0\x3f\x66\x0f\x6e\xd0\xbb\x34\x12\0\0",50);
    jump(code+50,code+256);
    /* A real wall must restore RBX to response; both exits preserve the actual ray normal. */
    memcpy(code+128,"\x48\x39\xfb\x74\x02\x0f\x0b\xf3\x0f\x11\x02\x0f\x11\x5a\x04"
                    "\xb8\x01\0\0\0\x48\x81\xc4\x08\x01\0\0\x5b\x5d\xc3",30);
    memcpy(code+192,"\xf3\x0f\x11\x02\x0f\x11\x5a\x04\x31\xc0"
                    "\x48\x81\xc4\x08\x01\0\0\x5b\x5d\xc3",20);
    assert(wall_axis_stub(code+256,code+192,code+128)<=192);
    int (ABI *bridge)(void *,const float *,float *)=(void *)code;
    const struct {
        float normal[4],ground[3]; unsigned char on,previous; int wall;
    } cases[]={
        /* Measured Sewer Speedway120FPS false wall, then measured30FPS real wall. */
        {{.020900f,.969664f,.243546f,.25f},{.017168f,.834343f,.550978f},1,1,0},
        {{.833627f,-.419133f,-.359713f,.25f},{.054349f,-.113874f,.992007f},1,1,1},
        {{1,0,0,0},{0,0,1},1,1,1},
        {{0,0,1,0},{0,0,1},1,1,0},
        {{0,0,-1,0},{0,0,1},1,1,0},
        {{1,0,0,0},{0,.8660254f,.5f},1,1,1},
        {{0,0,1,0},{0,1,0},1,1,0}, /* Never turn an existing non-wall into a wall. */
        {{.020900f,.969664f,.243546f,0},{.034336f,1.668686f,1.101956f},1,1,0},
        {{.020900f,.969664f,.243546f,0},{.017168f,.834343f,.550978f},0,1,1},
        {{.020900f,.969664f,.243546f,0},{.017168f,.834343f,.550978f},1,0,1},
        {{.020900f,.969664f,.243546f,0},{0,0,0},1,1,1},
        {{.020900f,.969664f,.243546f,0},{NAN,0,1},1,1,1},
        {{0,0,NAN,0},{0,0,1},1,1,0},
    };
    for (size_t i=0;i<sizeof cases/sizeof *cases;++i) {
        memcpy(ground_kart+0x208,cases[i].ground,sizeof cases[i].ground);
        ground_kart[0x214]=cases[i].on; ground_kart[0x215]=cases[i].previous;
        float out[5]={0};
        assert(bridge(response,cases[i].normal,out)==cases[i].wall);
        assert(!memcmp(out,&cases[i].normal[2],4));
        assert(!memcmp(out+1,cases[i].normal,16));
    }
    missing_ground=1;
    float out[5];
    assert(bridge(response,cases[0].normal,out)==1);
    missing_ground=0;
    munmap(code,4096);
}

static void test_track_rules(void) {
    native_layer_filter = filter;
    /* Execute the emitted mid-function bridge with the native stack alignment. */
    unsigned char *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(code != MAP_FAILED);
    unsigned char manager[0xd50] = {0}, component[32] = {0};
    void *manager_ptr = manager;
    put(component, 0x10, owner);
    /* push r12; mov r12,rdi; jmp bridge. Entry RSP+8 is aligned to 16. */
    memcpy(code, "\x41\x54\x49\x89\xfc", 5);
    jump(code + 5, code + 64);
    /* Confirm the data pointer survived, then return the mode supplied in ECX. */
    memcpy(code + 32, "\x49\x3b\x44\x24\x10\x75\x06\x89\xc8\x41\x5c\xc3\x90\x0f\x0b", 15);
    assert(crate_mode_stub(code + 64, &manager_ptr, code + 32) < 256);
    int (ABI *bridge)(void *) = (void *)code;
    const int transitions[] = {0, 1, 0, 1, 0};
    for (size_t step = 0; step < sizeof transitions / sizeof *transitions; ++step) {
        active = transitions[step];
        for (int mode = 0; mode <= 15; ++mode) {
            int expected = active && (mode == 1 || mode == 6) ? 2 : mode;
            assert(layer_filter(owner, (const char *)other, mode) == expected);
            memcpy(manager + 0xd4c, &mode, 4);
            assert(bridge(component) == expected);
            assert(!memcmp(manager + 0xd4c, &mode, 4));
        }
    }
    munmap(code, 4096);
    active = 0;
}

static unsigned char filter_world[0x320], filter_physics[0x50], filter_node[0x28];
static void *filter_signal, *filter_owner;
static int subscriptions, initializations, uninitializations, flag_updates, signal_queries;
static void ABI init_contact(void *self) { assert(self == filter_owner); ++initializations; }
static void ABI uninit_contact(void *self) {
    assert(self == filter_owner && !subscriptions);
    ++uninitializations;
}
static void *ABI physics_component(void *entity, void *type, int unused) {
    assert(entity == owner && type == filter_physics && !unused);
    return filter_physics;
}
static void *ABI get_signal(void *world, int kind, int body) {
    assert(world == filter_world && kind == 2 && body == 0);
    ++signal_queries;
    return &filter_signal;
}
static void ABI add_signal(void *signal, void *self) {
    assert(signal == &filter_signal && self == filter_owner && !subscriptions);
    filter_signal = filter_node;
    ++subscriptions;
}
static void ABI remove_signal(void *signal, void *self) {
    assert(signal == &filter_signal && self == filter_owner);
    subscriptions = 0; filter_signal = NULL;
}
static void ABI set_flags(void *world, int body, unsigned set, unsigned clear) {
    assert(world == filter_world && body == 0 && set == 0x40 && !clear);
    ++flag_updates;
}

static void test_collision_filter(void) {
    unsigned char self[0x60] = {0}, context[0x28] = {0}, event[0x28] = {0};
    unsigned char bodies[0x120] = {0}, cache[0x60] = {0};
    void *manager = self, *type = filter_physics;
    racers = &manager; find_racer = lookup;
    put(self, 0x18, owner); put(context, 0, filter_world);
    put(filter_world, 0x20, bodies); put(filter_physics, 0x48, filter_world);
    const unsigned capacity = 2; memcpy(filter_world + 0x2c, &capacity, 4);
    const int other_index = 1; memcpy(event + 12, &other_index, 4);
    put(event, 0x20, cache); bodies[0x90+0x42] = 4;
    filter_owner = self; physics_type = &type;
    native_initialize = init_contact; native_uninitialize = uninit_contact;
    get_component = physics_component; body_signal = get_signal;
    subscribe_contact = add_signal; unsubscribe_contact = remove_signal;
    enable_body_flags = set_flags;
    for (private_mode = 0; private_mode <= 1; ++private_mode)
        for (collision_enabled = 0; collision_enabled <= 1; ++collision_enabled) {
            active = 0;
            initialize(self);
            assert(!subscriptions && !flag_updates && !signal_queries);
        }
    active = private_mode = collision_enabled = 1;
    initialize(self);
    assert(!subscriptions && !flag_updates && !signal_queries);
    private_mode = collision_enabled = 0;
    initializations = 0;
    initialize(self);
    assert(subscriptions == 1 && ptr(filter_node, 0x18) == (void *)filter_contact);
    initialize(self); /* A repeated activation must not retain duplicate callbacks. */
    assert(initializations == 2 && subscriptions == 1);
    assert(flag_updates == 2 && signal_queries == 2);

    /* Execute native ba3320: disables the manifold and its cached contact flags. */
    const unsigned char cancel_code[] = {
        0xf6,0x47,0x17,0x10,0x74,0x1c,0x8b,0x47,0x18,0x0f,0xb6,0x4f,0x16,
        0xc1,0xe8,0x0e,0x48,0xc1,0xe1,0x04,0x83,0xe0,0x3c,0x48,0x01,0xc8,
        0x48,0x01,0xf8,0x74,0x03,0x80,0x08,0x03,0x80,0x4f,0x25,0x02,
        0x0f,0xb7,0x47,0x30,0x48,0x03,0x47,0x28,0x74,0x0e,
        0x8b,0xbe,0x10,0x03,0x00,0x00,0x48,0x89,0xc6,0xe9,0xa2,0x6c,0xcd,0xff,0xc3};
    void *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(code != MAP_FAILED); memcpy(code, cancel_code, sizeof cancel_code);
    disable_contact = (Subscribe)code;
    void *entities[] = {other, ai, ai2, obstacle, owner};
    for (active = 0; active <= 1; ++active)
        for (private_mode = 0; private_mode <= 1; ++private_mode)
            for (collision_enabled = 0; collision_enabled <= 1; ++collision_enabled)
                for (int i = 0; i < 5; ++i) {
                    put(bodies, 0x90+0x88, entities[i]);
                    memset(cache, 0, sizeof cache); cache[0x17] = 0x10; cache[0x16] = 4;
                    filter_contact(self, context, event);
                    int disabled = active && (!private_mode || !collision_enabled) && i < 3;
                    assert(cache[0x25] == (disabled ? 2 : 0));
                    assert(cache[0x40] == (disabled ? 3 : 0));
                }
    active = 1; private_mode = collision_enabled = 0;
    put(self, 0x18, ai); put(bodies, 0x90+0x88, owner);
    cache[0x25] = 0; filter_contact(self, context, event); assert(cache[0x25] == 2);
    put(bodies, 0x90+0x88, ai2);
    cache[0x25] = 0; filter_contact(self, context, event); assert(cache[0x25] == 2);
    const int invalid = 2; memcpy(event + 12, &invalid, 4);
    cache[0x25] = 0; filter_contact(self, context, event); assert(!cache[0x25]);
    memcpy(event + 12, &other_index, 4); bodies[0x90+0x42] = 0;
    filter_contact(self, context, event); assert(!cache[0x25]);
    bodies[0x90+0x42] = 4; put(event, 0x20, NULL);
    filter_contact(self, context, event); assert(!cache[0x25]);
    /* Public normal mode: only contacts involving the disconnected human are suppressed. */
    active=0;public_match=1;private_mode=0;collision_enabled=1;
    put(self,0x18,owner);put(event,0x20,cache);
    initialize(self);assert(subscriptions==1);
    for(int side=0;side<2;++side) for(int i=0;i<5;++i) {
        put(bodies,0x90+0x88,entities[i]);
        departed=side ? entities[i] : owner;
        cache[0x25]=0;filter_contact(self,context,event);
        assert(cache[0x25]==(entities[i]!=owner ? 2 : 0));
    }
    departed=NULL;
    put(bodies,0x90+0x88,ai);cache[0x25]=0;filter_contact(self,context,event);
    assert(!cache[0x25]); /* Native matchmaking AI still collides. */
    departed=owner;bodies[0x90+0x42]=0;filter_contact(self,context,event);
    assert(!cache[0x25]); /* Static track and ground remain solid. */
    departed=NULL;public_match=0;bodies[0x90+0x42]=4;
    put(self, 0x18, owner);
    active = 0; /* Cleanup must work even after the room/mode flag has reset. */
    uninitialize(self); assert(uninitializations == 1 && !subscriptions && !filter_signal);
    munmap(code, 4096);
}
int main(void) {
    assert(runtime_itemless_start(NULL, 0));
    unsigned char *invalid_image = calloc(1, 0x2ad4b78);
    assert(invalid_image && !runtime_itemless_start(invalid_image, 0x2ad4b78));
    free(invalid_image);
    test_banked_wall();
    test_start_grid();
    test_track_rules();
    test_collision_filter();
    private_mode = collision_enabled = 1;
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
    active=0;public_match=1;departed=other;
    int dc_responses=responses;
    contact(self,context,collision_data);assert(responses==dc_responses);
    departed=owner;contact(self,context,collision_data);assert(responses==dc_responses);
    departed=NULL;public_match=0;active=1;

    stop_boost(boost);
    assert(removals == 5); /* No protection leaks into later contacts. */
    private_mode = collision_enabled = 0;
    const int previous_responses = responses;
    boost_alive = 1;
    contact(self, context, collision_data);
    assert(responses == previous_responses && removals == 5 && boost_alive && !contact_owner);
    puts("Itemless contact tests passed");
}
