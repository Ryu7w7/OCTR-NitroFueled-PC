#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_itemless.c"
void runtime_race_options_stop_boost(void *b, void (ABI *stop)(void *)) { stop(b); }
static int active, responses, removals, nested;
static unsigned char owner[32], other[32], ai[32], ai2[32], obstacle[32], human_racer[32], ai_racer[32];
static unsigned char boost[32], wall_context[40], collision_data[16];
static int boost_alive;
int runtime_speed_race_active(void) { return active; }
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
    find_component=ground_component; kart_controller_type=&type;
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
int main(void) {
    test_banked_wall();
    test_track_rules();
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
