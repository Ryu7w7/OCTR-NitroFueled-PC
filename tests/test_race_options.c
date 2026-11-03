#include "../src/runtime_race_options.c"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
static int cup, private_room = 1, itemless, stale_private_ui, battle_menu;
static unsigned char default_race[0xd50];
static void *default_race_ptr = default_race;
int runtime_speed_race_active(void) { return itemless; }
int runtime_online_mode_in_private(void) { return private_room || stale_private_ui; }
int runtime_private_match_active(void) { return private_room; }
int runtime_online_mode_private_single_options(void) { return private_room && !cup && !battle_menu; }
int runtime_online_mode_private_cup(void) { return cup && !battle_menu; }
static void *ABI kind(void *event) { (void)event; return (void *)1; }
static int drops, calls;
static void ABI drop(void *p, void *s) {
    (void)p; ++drops;
    int *refs = (int *)((char *)s + 8);
    if (*refs > 1) --*refs; else free(s);
}
static char *ABI intern(void *p, const char *text) {
    (void)p; char *s = calloc(1, 16 + strlen(text) + 1); assert(s);
    *(int *)(s + 8) = 1; strcpy(s + 16, text); return s + 16;
}
static void ABI append(void *self, char **label, void *owner) {
    (void)owner;
    void *list = ptr(self, 0x1a8), *flags = ptr(self, 0x1b0);
    int count = integer(list, 12);
    put_ptr(ptr(list, 0x20), (size_t)count * 8, *label);
    ++*(int *)(*label - 8);
    ((char *)ptr(flags, 0x20))[count] = 1;
    put_int(list, 12, count + 1); put_int(flags, 12, count + 1);
}
static int ABI difficulty(void *self) { return integer(self, 12); }
static unsigned char boost_component[0x70];
static float last_threshold;
static void *ABI find_component(void *entity, void *type, int unused) {
    (void)entity; (void)type; (void)unused; return boost_component;
}
static float ABI duration(void *entry) { (void)entry; return 0; }
static unsigned char ABI ready(void *controller, float threshold) {
    (void)controller; last_threshold = threshold; return 0; /* Native failure is respected. */
}
static uint32_t timing_clock;
static uint32_t ABI timing_now(void *timer) { (void)timer; return timing_clock; }
static int physics_calls;
static float ABI speed_native_mock(void *kart, void *data, int kind, float speed, float dt) {
    assert(kart && kind==0 && speed==75 && dt==.016f); ++physics_calls;
    return real(data,12);
}
static void ABI modifiers_native_mock(void *kart, float dt) {
    assert(dt==.016f); ++physics_calls;
    float value=.6f; memcpy((unsigned char *)kart+0x190,&value,4);
    put_int(kart,0x194,9999);
}
static void test_ai_physics(void) {
    unsigned char game[0x150]={0}, diff[32]={0}, race[0xd50]={0};
    unsigned char ai[0x80]={0}, control[0x80]={0}, kart[0x280]={0}, human[0x280]={0};
    unsigned char handle[0x30]={0}, entity[0x80]={0}, speed_data[0x30]={0};
    void *g=game,*d=diff,*r=race;
    client=&g; difficulty_manager=&d; manager=&r; native_difficulty=difficulty;
    native_kart_speed=speed_native_mock; native_speed_modifiers=modifiers_native_mock;
    put_int(race,0xd4c,1); put_ptr(ai,0x18,entity); put_ptr(ai,0x48,control);
    put_ptr(control,0x58,handle); put_ptr(handle,0x28,kart); put_ptr(kart,0x18,entity);
    float base=100; memcpy(speed_data+12,&base,4);
    /* AI component +28 is steering coordinates, not a racer handle. */
    float targets[2][3]={{1200,4500,100},{1500,4700,100}};
    memcpy(ai+0x28,targets,sizeof targets);
    reset_ai_physics(); register_ai_physics(ai); private_room=1;
    const float expected[]={1,1,1.04f,1.06f,1.08f,1.105f};
    for (int mode=8;mode<=16;mode+=8) for(int level=0;level<6;++level) {
        put_int(game,0x144,mode); put_int(diff,12,level>2?2:level); ai_profile=level>2?level-2:0;
        for (int repeat=0;repeat<3;++repeat) {
            assert(fabsf(kart_speed(kart,speed_data,0,75,.016f)-100*expected[level])<.001f);
            assert(real(speed_data,12)==100); /* No shared-data mutation or accumulating scale. */
        }
        assert(kart_speed(human,speed_data,0,75,.016f)==100);
    }
    unsigned char timer[24]={0}; void *vtable[24]={0};
    vtable[0xb0/8]=timing_now; put_ptr(timer,0,vtable); put_ptr(kart,0xf8,timer);
    timing_clock=32768;
    assert(fabsf(kart_speed(kart,speed_data,0,75,.016f)-112)<.001f);
    timing_clock=98304;
    assert(fabsf(kart_speed(kart,speed_data,0,75,.016f)-109)<.001f);
    timing_clock=0;
    unsigned char racers_owner[32]={0}, racers_list[40]={0}, racer[0x170]={0};
    unsigned char racer_entity[0x30]={0}, player_handle[0x30]={0}, player[0xd0]={0};
    void *racers_root=racers_owner, *racers[]={NULL,racer};
    racer_manager=&racers_root; put_ptr(racers_owner,0x10,racers_list);
    put_ptr(racers_list,0x20,racers); put_int(racers_list,12,2);
    put_ptr(racer,0x20,racer_entity); put_ptr(racer_entity,0x28,entity); put_int(racer,0x164,3);
    register_ai_physics(ai); assert(ai_physics[0].id==3);
    assert(fabsf(kart_speed(kart,speed_data,0,75,.016f)-100*ai_speed_scale(5,0,0,3))<.001f);
    put_ptr(racer,0x10,player_handle); put_ptr(player_handle,0x28,player); put_int(player,0xc8,7);
    register_ai_physics(ai); assert(ai_physics[0].id==7);
    peer_decision=1; register_ai_physics(ai); assert(ai_physics[0].id==7); peer_decision=0;
    put_ptr(racer_entity,0x28,human); register_ai_physics(ai); assert(ai_physics[0].id==0);
    put_ptr(racer_entity,0x28,entity); put_int(player,0xc8,99);
    register_ai_physics(ai); assert(ai_physics[0].id==0);
    put_int(racers_list,12,9); register_ai_physics(ai); assert(ai_physics[0].id==0);
    racer_manager=NULL; register_ai_physics(ai);
    peer_decision=1;
    assert(fabsf(kart_speed(kart,speed_data,0,75,.016f)-110.5f)<.001f); /* Same physics for replicated AI. */
    peer_decision=0; private_room=0;
    assert(kart_speed(kart,speed_data,0,75,.016f)==100); private_room=1;
    put_int(race,0xd4c,9); assert(kart_speed(kart,speed_data,0,75,.016f)==100); put_int(race,0xd4c,1);
    unsigned char list[40]={0}, entries[2][32]={{0}}, data[2][32]={{0}};
    void *items[]={entries[0],entries[1]}; put_ptr(list,32,items); put_int(list,12,2); put_ptr(kart,0x188,list);
    for(int i=0;i<2;++i) { put_ptr(entries[i],0x10,data[i]); float v=i?1.2f:.5f; memcpy(data[i]+12,&v,4); memcpy(data[i]+16,&v,4); }
    kart[0x214]=1; float damage=.7f; memcpy(kart+0x198,&damage,4);
    itemless=0; speed_modifiers(kart,.016f); assert(fabsf(real(kart,0x190)-.6f)<.001f);
    put_int(kart,0x194,1234);
    itemless=1; speed_modifiers(kart,.016f);
    assert(integer(kart,0x194)==1234); assert(fabsf(real(kart,0x190)-1.2f)<.001f);
    assert(real(kart,0x198)==damage); /* Damage and native timers remain outside the surface policy. */
    speed_modifiers(human,.016f); assert(fabsf(real(human,0x190)-.6f)<.001f);
    put_int(diff,12,1); ai_profile=0;
    assert(kart_speed(kart,speed_data,0,75,.016f)==100);
    speed_modifiers(kart,.016f); assert(fabsf(real(kart,0x190)-.6f)<.001f);
    reset_ai_physics(); assert(kart_speed(kart,speed_data,0,75,.016f)==100);
    itemless=0; client=difficulty_manager=manager=NULL; ai_profile=0;
}

static void test_dynamic_speed(void) {
    const float low[]={1,1,1.03f,1.05f,1.07f,1.09f};
    const float high[]={1,1,1.05f,1.07f,1.09f,1.12f};
    for (int level=0;level<6;++level) for (unsigned id=0;id<8;++id) {
        float min=2,max=0,previous=ai_speed_scale(level,0,0,id);
        for (uint32_t tick=0;tick<=131072;tick+=128) {
            float scale=ai_speed_scale(level,0,tick,id);
            assert(scale>=low[level]-.000001f && scale<=high[level]+.000001f);
            assert(fabsf(scale-previous)<.0001f); /* No step when cycling or changing speed. */
            assert(scale==ai_speed_scale(level,0,tick+131072,id));
            min=fminf(min,scale); max=fmaxf(max,scale); previous=scale;
        }
        assert(fabsf(min-low[level])<.000001f && fabsf(max-high[level])<.000001f);
        assert(fabsf(ai_speed_scale(level,0,UINT32_MAX,id)-ai_speed_scale(level,0,0,id))<.00001f);
    }
    assert(ai_speed_scale(5,0,32768,0)>ai_speed_scale(5,0,32768,4));
    assert(ai_speed_scale(-1,1,0,0)==1 && ai_speed_scale(6,1,0,0)==1);
    assert(fabsf(ai_speed_scale(1,1,32768,0)-1.02f)<.000001f);
    assert(fabsf(ai_speed_scale(1,1,98304,0)-1.01f)<.000001f);
}

static void test_reserves(void) {
    unsigned char game[0x150] = {0}, diff[0x20] = {0}, controller[0x218] = {0};
    unsigned char list[0x28] = {0}, boosts[2][0x50] = {{0}}, data[2][0x40] = {{0}};
    void *g = game, *d = diff, *type = (void *)1, *items[] = {boosts[0], boosts[1]};
    client = &g; difficulty_manager = &d; native_difficulty = difficulty;
    component = find_component; maintained_duration = duration; native_boost_ready = ready; boost_type = &type;
    put_ptr(controller, 0x18, controller); put_ptr(boost_component, 0x28, list);
    put_ptr(list, 0x20, items); put_int(list, 12, 2);
    for (int i = 0; i < 2; ++i) {
        put_ptr(boosts[i], 0x10, data[i]);
        float speed = 100.0f + i * 100.0f, reserve = 5.0f;
        memcpy(data[i] + 12, &speed, 4); memcpy(boosts[i] + 0x28, &reserve, 4);
    }
    for (int mode = 8; mode <= 16; mode += 8) {
        put_int(game, 0x144, mode);
        for (int level = 0; level < 6; ++level) {
            put_int(diff, 12, level > 2 ? 2 : level); ai_profile = level > 2 ? level - 2 : 0;
            assert(!boost_decision(controller, .8f, 1));
            const float expected[] = {.8f,.8f,.8f,.8f,.8f,.8f};
            assert(last_threshold == expected[level]);
        }
    }
    /* A long lower-speed reserve cannot conceal expiring Blue Fire. */
    float low = .2f; memcpy(boosts[1] + 0x28, &low, 4);
    assert(fire_remaining(controller) == low);
    boosts[1][0x30]=1;
    assert(fire_remaining(controller)==low); /* Pad contact is not infinite post-pad reserves. */
    boosts[1][0x30]=0; boosts[1][0x31]=1;
    assert(fire_remaining(controller)==low); /* Paused ageing still has a finite carry bank. */
    boosts[1][0x31]=0;
    boost_decision(controller, .8f, 1); assert(last_threshold == .8f);
    float plenty = 5; memcpy(boosts[1] + 0x28, &plenty, 4);
    reference_intent = 8;
    boost_decision(controller, .8f, 1); assert(last_threshold == .8f); /* Without a style/clock, keep the native policy. */
    reference_intent = 2|TT_REFERENCE_ALIGNED;
    boost_decision(controller, .8f, 1); assert(last_threshold == .8f); /* Bank before takeoff. */
    reference_intent = 0;
    put_int(list, 12, 0); assert(fire_remaining(controller) == 0);
    boost_decision(controller, .8f, 1); assert(last_threshold == .8f); /* No guided style: native timing. */
    reference_intent = 2|TT_REFERENCE_ALIGNED;
    boost_decision(controller, .8f, 1); assert(last_threshold == .8f); /* Plain hop without fire is no reserve emergency. */
    reference_intent = 0;
    put_int(list, 12, 2);
    boost_decision(controller, .8f, 0); assert(last_threshold == .8f); /* Human call. */
    put_int(game, 0x144, 1);
    boost_decision(controller, .8f, 1); assert(last_threshold == .8f); /* Adventure. */
    assert(real(boosts[1], 0x28) == plenty); /* No synthetic refill. */
    put_int(game,0x144,16);
    unsigned char handle[0x30]={0}, archetype[0xb0]={0}, drift_data[0x30]={0}, timer[24]={0};
    void *vtable[24]={0}; vtable[0xb0/8]=timing_now;
    put_ptr(controller,0xb8,handle); put_ptr(handle,0x28,archetype); put_ptr(archetype,0xa8,drift_data);
    float maximum=1; memcpy(drift_data+0x20,&maximum,4);
    put_ptr(controller,0xf8,timer); put_ptr(timer,0,vtable); timer[0x14]=1;
    timing_clock=3277; reference_slot=0; reference_intent=0;
    boost_decision(controller,.8f,1); assert(last_threshold==.8f);
    memcpy(boosts[1]+0x28,&low,4);
    boost_decision(controller,.8f,1);
    assert(last_threshold>.56f && last_threshold<.57f); /* Actual charge + reserve deadline, below perfect. */
    reference_intent=512;
    boost_decision(controller,.8f,1);
    assert(last_threshold>.39f && last_threshold<.41f); /* Bank the available meter for takeoff. */
    float valid_minimum=.5f;
    memcpy(drift_data+0x24,&valid_minimum,4);
    boost_decision(controller,.8f,1);
    assert(last_threshold==valid_minimum); /* Pre-hop may not consume an empty boost. */
    low=.001f; memcpy(boosts[1]+0x28,&low,4); reference_intent=0;
    boost_decision(controller,.8f,1);
    assert(last_threshold==valid_minimum); /* Neither may an expiring turbo-pad reserve. */
    assert(!bank_slide_boost(controller) && last_threshold==valid_minimum);
    for (int mode=8;mode<=16;mode+=8) {
        put_int(game,0x144,mode);
        for (int level=0;level<6;++level) {
            put_int(diff,12,level>2 ? 2 : level); ai_profile=level>2 ? level-2 : 0;
            boost_decision(controller,.8f,1);
            assert(last_threshold==valid_minimum); /* Physics floor, not a universal Perfect target. */
        }
    }
    valid_minimum=NAN; memcpy(drift_data+0x24,&valid_minimum,4);
    boost_decision(controller,.8f,1); assert(last_threshold==.8f);
    assert(!bank_slide_boost(controller));
    reference_intent=0; reference_slot=-1;
    client = difficulty_manager = boost_type = NULL; ai_profile = 0;
}
static unsigned char ABI drift_gate(void *self, float angle) {
    return real(self, 0x44) * angle > 0 && fabsf(real(self, 0x44)) > .15f &&
        real(self, 0x48) > real(self, 0x4c) && fabsf(angle) > .15f;
}
static float tt_speed = .9f;
static int tt_speed_reads;
static float ABI tt_speed_mock(void *kart) { assert(ptr(kart,0x18)); ++tt_speed_reads; return tt_speed; }
static unsigned char ABI tt_boost_mock(void *kart, float threshold) { (void)kart; assert(threshold == .97f || threshold == .8f); return 1; }
static unsigned char tt_slide_complete, tt_oversteer;
static unsigned char ABI tt_oversteer_mock(void *kart, float margin) { (void)kart; assert(margin == 1.f / 30.f); return tt_oversteer; }
static unsigned char ABI tt_slide_complete_mock(void *kart) { (void)kart; return tt_slide_complete; }
static int applied_tt_inputs;
static void ABI apply_inputs_mock(void *self, void *input) { (void)self; (void)input; ++applied_tt_inputs; }
static void ABI technique_update_mock(void *self, void *input) {
    (void)input;
    unsigned char *control = ptr(self,0x48);
    control[0x50] = control[0x51] = 1;
    put_int(control,0x74,1);
    ((unsigned char *)ptr(self,0x70))[0x10] |= 8;
    reference_slot = 0; reference_intent = 1|128|(127u<<16);
}
static void apply_tt_test(void *ai) {
    previous_ai_actions=((unsigned char *)ptr(ai,0x70))[0x10];
    apply_tt_technique(ai,NULL);
}
static void test_tt_technique(void) {
    unsigned char game[0x150] = {0}, diff[32] = {0}, ai[0x90] = {0}, control[0x80] = {0};
    unsigned char kart[0x218] = {0}, handle[0x30] = {0}, wire[0x18] = {0};
    unsigned char item_use[0x80] = {0}, item_before[0x80];
    item_use[0x50] = 77; item_use[0x51] = 88;
    memcpy(item_before,item_use,sizeof item_use);
    void *g=game, *d=diff;
    client=&g; difficulty_manager=&d; native_difficulty=difficulty; native_apply_ai_input=apply_inputs_mock;
    put_int(game,0x144,16); put_int(diff,12,2); ai_profile=3; followed_ai_profile=-1;
    put_ptr(ai,0x48,control); put_ptr(ai,0x58,item_use); put_ptr(ai,0x70,wire); put_ptr(control,0x58,handle); put_ptr(handle,0x28,kart);
    put_ptr(ai,0x18,ai); put_ptr(kart,0x18,ai);
    reference_slot=0; reference_intent=16|32|64; wire[0x10]=0xf1; applied_tt_inputs=0;
    kart[0x214]=1; apply_tt_test(ai); assert(applied_tt_inputs==0 && wire[0x10]==0xf1);
    kart[0x214]=0; apply_tt_test(ai);
    assert(applied_tt_inputs==0 && wire[0x10]==0xf1 && real(wire,0xc)==0); /* Ghost U-turn cannot overwrite native controls. */
    reference_intent=16; apply_tt_test(ai); assert(wire[0x10]==0xf1 && real(wire,0xc)==0);
    peer_decision=1; wire[0x10]=0xc1; apply_tt_test(ai); assert(wire[0x10]==0xc1);
    peer_decision=0; put_int(game,0x144,1); apply_tt_test(ai); assert(wire[0x10]==0xc1);
    put_int(game,0x144,8); kart[0x214]=1; reference_intent=2|TT_REFERENCE_ALIGNED; wire[0x10]=0xf9;
    apply_tt_test(ai); assert(wire[0x10]==0xf1); /* Release held slide first. */
    apply_tt_test(ai); assert(wire[0x10]==0xf9); /* One new jump press. */
    wire[0x10]=0xf1; apply_tt_test(ai); assert(wire[0x10]==0xf1); /* No repeated press. */
    reference_intent=0; apply_tt_test(ai);
    reference_intent=2|TT_REFERENCE_ALIGNED; kart[0x214]=0; apply_tt_test(ai);
    kart[0x214]=1; apply_tt_test(ai); assert(wire[0x10]==0xf1); /* No late landing jump. */
    reference_intent = 0; apply_tt_test(ai);
    reference_intent = 512 | TT_REFERENCE_ALIGNED; wire[0x10] = 0xf9;
    apply_tt_test(ai); assert(wire[0x10] == 0xe1 && tt_jumps[0].phase == 1);
    apply_tt_test(ai); assert(wire[0x10] == 0xe1); /* Keep released until the takeoff. */
    reference_intent = 2|TT_REFERENCE_ALIGNED; apply_tt_test(ai);
    assert(wire[0x10] == 0xe9 && tt_jumps[0].phase == 0); /* Press on the marker, not one tick late. */
    kart[0x214] = 0; reference_intent = TT_REFERENCE_ALIGNED;
    apply_tt_test(ai); assert(wire[0x10] == 0xe9);
    kart[0x214] = 1;
    reference_intent=0; apply_tt_test(ai);
    reference_intent=2|TT_REFERENCE_ALIGNED; wire[0x10]=0xf9;
    apply_tt_test(ai); assert(wire[0x10]==0xf1);
    reference_intent=TT_REFERENCE_ALIGNED; /* One-shot edge already consumed. */
    apply_tt_test(ai); assert(wire[0x10]==0xf9);
    reference_intent=0; apply_tt_test(ai);
    reference_intent=2|TT_REFERENCE_ALIGNED; wire[0x10]=0xf9;
    apply_tt_test(ai); assert(wire[0x10]==0xf1);
    reference_intent=0; apply_tt_test(ai); assert(wire[0x10]==0xf1); /* Off-route cancellation. */
    reference_intent=2|TT_REFERENCE_ALIGNED; wire[0x10]=0xf9;
    previous_ai_actions=0xf1; /* Native has just pressed R1 during this tick. */
    apply_tt_technique(ai,NULL);
    assert(wire[0x10]==0xf9 && tt_jumps[0].phase==0); /* Do not undo a fresh press. */
    normalized_kart_speed = tt_speed_mock; native_boost_ready = tt_boost_mock;
    float minimum = .5f; memcpy(control + 0x4c, &minimum, 4);
    reference_intent = 1|128; wire[0x10] = 0xe1;
    int reads_before = tt_speed_reads, inputs_before = applied_tt_inputs;
    put_ptr(kart,0x18,NULL); apply_tt_test(ai);
    assert(tt_speed_reads == reads_before && applied_tt_inputs == inputs_before && wire[0x10] == 0xe1);
    put_ptr(kart,0x18,control); apply_tt_test(ai);
    assert(tt_speed_reads == reads_before && applied_tt_inputs == inputs_before);
    put_ptr(kart,0x18,ai);
    apply_tt_test(ai);
    assert(wire[0x10] == 0xe1); /* Native refusal is authoritative. */
    float countersteer = .2f;
    memcpy(wire + 0xc,&countersteer,4);
    put_int(control,0x74,1);
    /* Opposite recorded steering must not flip native entry, airborne landing,
     * or active countersteer. Exercise every native driving state. */
    for (int state=0; state<=4; ++state) {
        put_int(kart,0xe0,state);
        for (int direction=0; direction<2; ++direction) {
            reference_intent = 1 | (direction ? 128u|(127u<<16) : 256u|(129u<<16));
            wire[0x10] = 0xe9;
            apply_tt_test(ai);
            assert(real(wire,0xc) == countersteer);
            assert(wire[0x10] == (state>=3 ? 0xf9 : 0xe9));
        }
    }
    put_int(kart,0xe0,3); wire[0x10]=0xe1;
    apply_tt_test(ai);
    assert(wire[0x10]==0xe1); /* Do not resurrect a cancelled native slide. */
    put_int(control,0x74,2); wire[0x10]=0xe9;
    apply_tt_test(ai);
    assert(wire[0x10]==0xe9); /* A hop is not a powerslide. */
    put_int(control,0x74,1);
    reference_intent = 1; wire[0x10] = 0xe9; apply_tt_test(ai);
    assert(wire[0x10] == 0xf9);
    reference_intent = 0; wire[0x10] = 0xe9; apply_tt_test(ai);
    assert(wire[0x10] == 0xf9); /* Keep boosting the actual slide across ghost release gaps. */
    reference_intent = 1;
    /* A ready perfect boost is banked before the hop release, not discarded
     * by clearing drift on the same frame as the boost button. */
    reference_intent=512|TT_REFERENCE_ALIGNED|TT_REFERENCE_BANK_BEFORE_HOP; wire[0x10]=0xe9;
    tt_jumps[0].active=tt_jumps[0].phase=0;
    apply_tt_test(ai); assert(wire[0x10]==0xf9 && tt_jumps[0].phase==4);
    apply_tt_test(ai); assert(wire[0x10]==0xe1 && tt_jumps[0].phase==1);
    reference_intent=2|TT_REFERENCE_ALIGNED;
    apply_tt_test(ai); assert(wire[0x10]==0xe9 && tt_jumps[0].phase==0);
    reference_intent=1; apply_tt_test(ai);
    reference_intent=512|TT_REFERENCE_ALIGNED; wire[0x10]=0xe9;
    apply_tt_test(ai); assert(wire[0x10]==0xe1 && tt_jumps[0].phase==1); /* Too late to bank: release now. */
    reference_intent=1; apply_tt_test(ai);
    native_drift_complete = tt_slide_complete_mock; tt_slide_complete = 1;
    wire[0x10] = 0xf9; apply_tt_test(ai);
    assert(wire[0x10] == 0xe1); /* Release exhausted native boost chain. */
    /* A completed slide must not cancel the shortcut release/press pair. */
    reference_intent = 1 | 2 | TT_REFERENCE_ALIGNED;
    wire[0x10]=0xf9; apply_tt_test(ai);
    assert(!(wire[0x10]&8) && tt_jumps[0].phase==3);
    reference_intent = 1 | TT_REFERENCE_ALIGNED;
    wire[0x10]=0xf9; apply_tt_test(ai);
    assert(wire[0x10]&8);
    reference_intent = 1;
    tt_slide_complete = 0; native_drift_oversteer = tt_oversteer_mock; tt_oversteer = 1;
    wire[0x10] = 0xf9; apply_tt_test(ai); assert(wire[0x10] == 0xe1);
    unsigned char archetype[0xb0]={0}, drift_data[0x30]={0}, archetype_handle[0x30]={0};
    put_ptr(kart,0xb8,archetype_handle); put_ptr(archetype_handle,0x28,archetype);
    put_ptr(archetype,0xa8,drift_data);
    float minimum_charge=.8f; memcpy(drift_data+0x24,&minimum_charge,4);
    tt_oversteer=0; reference_intent=1|TT_REFERENCE_ALIGNED;
    put_int(control,0x74,1); put_int(kart,0xe0,3); tt_jumps[0].phase=0;
    wire[0x10]=0xe1; previous_ai_actions=0xe9; apply_tt_technique(ai,NULL);
    assert(wire[0x10]==0xf9); /* One bank tick before the heading correction releases drift. */
    wire[0x10]=0xe1; previous_ai_actions=0xf9; apply_tt_technique(ai,NULL);
    assert(wire[0x10]==0xe1); /* No repeated hold or boost. */
    tt_oversteer=1; previous_ai_actions=0xe9; apply_tt_technique(ai,NULL);
    assert(wire[0x10]==0xe1); /* Physical cancellation cannot be overridden. */
    put_ptr(kart,0xb8,NULL);
    tt_oversteer = 0; put_int(kart,0xe0,0); reference_intent = 1|128;
    apply_tt_test(ai); assert(wire[0x10] == 0xe1); /* Native must approve the next entry. */
    tt_speed = .1f; wire[0x10] = 0xe1; apply_tt_test(ai);
    assert(wire[0x10] == 0xe1); /* Respect minimum speed after recovery. */
    tt_speed = .9f; reference_intent = 0; apply_tt_test(ai);
    assert(wire[0x10] == 0xe1);
    /* Full AI wrapper uses the real native layout, with a distinct item-use
     * component at +58. The old fixture incorrectly put driving there. */
    native_ai_input = technique_update_mock; put_int(kart,0xe0,3);
    wire[0x10] = 0xe1; control[0x50] = 0; control[0x51] = 0;
    int applied_before = applied_tt_inputs;
    ai_input(ai,NULL);
    assert(applied_tt_inputs == applied_before + 1 && wire[0x10] == 0xf9);
    assert(control[0x50] == 0 && control[0x51] == 0);
    assert(!memcmp(item_use,item_before,sizeof item_use));
    native_boost_ready = ready; wire[0x10] = 0xf1;
    ai_input(ai,NULL);
    assert(wire[0x10] == 0xe9); /* Hold drift but clear boost until native meter accepts it. */
    assert(!memcmp(item_use,item_before,sizeof item_use));
    unsigned slide_marker = 1 | 2 | 512 | TT_REFERENCE_ALIGNED | TT_REFERENCE_DRIFT_HOP;
    ai[0x7d]=1;
    assert((guide_hop(ai,slide_marker) & (2|512))==0 && ai[0x7d]==0);
    assert(guide_hop(ai,TT_REFERENCE_ALIGNED)==TT_REFERENCE_ALIGNED && ai[0x7d]==0);
    assert(guide_hop(ai,2|TT_REFERENCE_ALIGNED)==(2|TT_REFERENCE_ALIGNED) && ai[0x7d]==1);
    ai[0x7d]=1;
    assert(guide_hop(ai,0)==0 && ai[0x7d]==0); /* Rejoining never inherits a native route hop. */
    /* Every gas/reverse/brake/fire combination survives TT hints unchanged. */
    put_int(kart,0xe0,0); kart[0x214]=0;
    for (unsigned bits=0; bits<64; ++bits) {
        wire[0x10]=0xc0|bits; reference_intent=16|32|64;
        float turn=.3f; memcpy(wire+0xc,&turn,4);
        apply_tt_test(ai);
        assert((wire[0x10]&0xe7)==((0xc0|bits)&0xe7) && real(wire,0xc)==turn);
    }
    normalized_kart_speed = NULL; native_boost_ready = NULL; native_drift_complete = NULL; native_drift_oversteer = NULL;
    client=difficulty_manager=NULL; reference_intent=0; reference_slot=-1; ai_profile=0;
}
static int continue_safety_stop;
static unsigned char ABI continue_gate(void *self, int direction, float angle) {
    (void)self;
    if (continue_safety_stop || tt_slide_complete || tt_oversteer) return 0;
    return direction == 0 ? angle > -.125f : direction == 1 ? angle < .125f : fabsf(angle) < .1f;
}
static void test_drift_frequency(void) {
    unsigned char game[0x150] = {0}, diff[32] = {0}, controller[0x60] = {0};
    void *g = game, *d = diff;
    client = &g; difficulty_manager = &d; native_difficulty = difficulty;
    native_start_drift = drift_gate;
    float curve = .3f, speed = .8f, minimum = .5f;
    memcpy(controller + 0x44, &curve, 4); memcpy(controller + 0x48, &speed, 4);
    memcpy(controller + 0x4c, &minimum, 4);
    for (int mode = 8; mode <= 16; mode += 8) {
        put_int(game, 0x144, mode);
        int previous = 0;
        for (int level = 0; level < 6; ++level) {
            put_int(diff, 12, level > 2 ? 2 : level); ai_profile = level > 2 ? level - 2 : 0;
            int starts = 0;
            for (int a = 1; a <= 30; ++a) starts += start_drift(controller, a * .01f);
            assert(starts > previous); previous = starts;
            assert(!start_drift(controller, -.3f)); /* Opposite direction. */
            assert(!start_drift(controller, 0)); /* No straight-line drifting. */
        }
    }
    native_continue_drift=continue_gate; reference_slot=0;
    reference_intent=1|TT_REFERENCE_ALIGNED;
    assert(continue_drift(controller,0,-.20f));
    assert(continue_drift(controller,1,.20f));
    assert(continue_drift(controller,0,-.30f));
    assert(continue_drift(controller,1,.30f));
    assert(!continue_drift(controller,0,-.6f));
    assert(!continue_drift(controller,1,.6f)); /* Large heading error must release. */
    continue_safety_stop=1; assert(!continue_drift(controller,0,0));
    continue_safety_stop=0;
    reference_intent=1; assert(continue_drift(controller,0,-.20f));
    reference_intent=TT_REFERENCE_ALIGNED; assert(continue_drift(controller,1,.20f));
    reference_intent=TT_REFERENCE_ALIGNED|4; assert(continue_drift(controller,1,.20f)); /* Keep boost until takeoff. */
    reference_intent=TT_REFERENCE_ALIGNED|2; assert(!continue_drift(controller,1,.20f));
    reference_intent=TT_REFERENCE_ALIGNED|16; assert(!continue_drift(controller,1,.20f));
    reference_intent=1|TT_REFERENCE_ALIGNED;
    peer_decision=1; assert(!continue_drift(controller,0,-.20f)); peer_decision=0;
    put_int(game,0x144,1); assert(!continue_drift(controller,0,-.20f));
    put_int(game,0x144,16);
    reference_intent=0;
    float shallow = .04f;
    memcpy(controller + 0x44,&shallow,4);
    reference_intent = 1|TT_REFERENCE_ALIGNED;
    assert(start_drift(controller,.04f)); /* Both native angular gates relaxed. */
    assert(real(controller,0x44)==shallow); /* Live feedback restored. */
    reference_intent=1|256|TT_REFERENCE_ALIGNED;
    assert(start_drift(controller,.04f));
    shallow=-.04f; memcpy(controller+0x44,&shallow,4);
    assert(!start_drift(controller,-.04f)); /* Lateral correction cannot reverse the recorded slide. */
    reference_intent=1|128|TT_REFERENCE_ALIGNED;
    assert(start_drift(controller,-.04f));
    shallow=.04f; memcpy(controller+0x44,&shallow,4);
    assert(!start_drift(controller,.04f));
    reference_intent=1|TT_REFERENCE_ALIGNED;
    assert(!start_drift(controller,-.04f)); /* Still rejects wrong-way entry. */
    assert(!start_drift(controller,0)); /* Still rejects a straight heading. */
    reference_intent = 0;
    assert(!start_drift(controller,.04f)); /* Plain hop gets no recorded-slide assist. */
    curve = .3f; memcpy(controller + 0x44,&curve,4);
    assert(!start_drift(controller, .10f)); /* No guided slide outside the reference window. */
    reference_intent=1; assert(!start_drift(controller,.10f)); /* Off-path slide suppressed. */
    reference_intent=1|TT_REFERENCE_ALIGNED;
    assert(start_drift(controller,.10f));
    reference_slot=-1;
    assert(start_drift(controller,.10f)); /* Unguided native behavior retained. */
    put_int(game, 0x144, 1);
    assert(!start_drift(controller, .10f)); /* Adventure unchanged. */
    put_int(game, 0x144, 16);
    minimum = .95f; memcpy(controller + 0x4c, &minimum, 4);
    assert(!start_drift(controller, .3f)); /* Native stuck-recovery speed gate. */
    reference_slot=0; reference_intent=1|TT_REFERENCE_ALIGNED;
    assert(continue_drift(controller,0,-.3f) && slide_decision.reference_hold);
    tt_slide_complete=1;
    assert(!continue_drift(controller,0,-.3f));
    tt_slide_complete=0; tt_oversteer=1;
    assert(!continue_drift(controller,0,-.3f));
    tt_oversteer=0; reference_intent=0;
    assert(continue_drift(controller,0,-.3f)); /* Alignment jitter cannot break an active chain. */
    reference_intent=1; assert(continue_drift(controller,0,-.3f));
    reference_intent=1|TT_REFERENCE_ALIGNED;
    assert(!continue_drift(controller,2,.3f)); /* Unknown direction stays native. */
    reference_slot=-1; reference_intent=0;
    assert(isnan(drift_entry_angle(NAN, 5)));
    client = difficulty_manager = NULL; ai_profile = 0;
}
static void *branch_list_seen, *branch_selected;
static void *ABI branch_mock(void *self, void **list, unsigned char lap, unsigned char force) {
    (void)self; assert(lap == 1 && force == 0);
    branch_list_seen = *list;
    branch_selected = ptr(ptr(*list, 32), 0);
    return branch_selected;
}
static void test_shortcuts(void) {
    unsigned char list[40] = {0}, segments[4][0x68] = {{0}}, lines[4][40] = {{0}}, records[4][32] = {{0}};
    void *items[] = {segments[0], segments[1], segments[2], segments[3]};
    void *record_items[] = {records[0], records[1], records[2], records[3]};
    const int occupancy[] = {0,1,3,0}; const float times[] = {10,8,5,2};
    put_int(list, 12, 4); put_ptr(list, 32, items);
    for (int i = 0; i < 4; ++i) {
        put_int(segments[i], 0x60, occupancy[i]); put_ptr(segments[i], 0x28, lines[i]);
        put_int(lines[i], 12, 1); put_ptr(lines[i], 32, record_items + i);
        memcpy(records[i] + 12, times + i, 4);
    }
    segments[3][0x65] = 1;
    assert(!shortcut_branch(list, 0) && !shortcut_branch(list, 1));
    assert(shortcut_branch(list, 2) == segments[1]);
    assert(shortcut_branch(list, 3) == segments[1]);
    assert(shortcut_branch(list, 4) == segments[2]);
    assert(shortcut_branch(list, 5) == segments[2]);
    /* Nightmare takes a valid authored shortcut even when crowded and its
     * recorded route time is slower than an ordinary connected branch. */
    segments[1][0x64] = 1; put_int(segments[1], 0x60, 8);
    assert(shortcut_branch(list, 5) == segments[1]);
    assert(shortcut_branch(list, 4) == segments[2]);
    segments[2][0x64] = 1;
    assert(shortcut_branch(list, 5) == segments[2]); /* Fastest valid shortcut. */
    segments[2][0x65] = 1;
    assert(shortcut_branch(list, 5) == segments[1]); /* Hazardous shortcut skipped. */
    segments[2][0x65] = 0;
    unsigned char game[0x150] = {0}, diff[32] = {0}; void *g = game, *d = diff, *input = list;
    client = &g; difficulty_manager = &d; native_difficulty = difficulty;
    native_choose_branch = branch_mock; put_int(diff, 12, 2); ai_profile = 3;
    for (int mode = 8; mode <= 16; mode += 8) {
        put_int(game, 0x144, mode);
        assert(choose_branch(NULL, &input, 1, 0) == segments[0]);
        assert(branch_list_seen == list); /* Normal leaves native route selection intact. */
    }
    itemless=1; put_int(game,0x144,16);
    assert(choose_branch(NULL,&input,1,0)==segments[2] && branch_list_seen!=list);
    itemless=0;
    put_int(game, 0x144, 1);
    assert(choose_branch(NULL, &input, 1, 0) == segments[0] && branch_list_seen == list);
    float invalid = NAN; memcpy(records[2] + 12, &invalid, 4);
    assert(shortcut_branch(list, 5) == segments[1]);
    segments[1][0x65] = 1; assert(!shortcut_branch(list, 5));
    segments[0][0x65] = 1; assert(!shortcut_branch(list, 5));
    client = difficulty_manager = NULL; ai_profile = 0;
}
static void test_route_variation(void) {
    unsigned char list[40] = {0}, segments[3][0x68] = {{0}};
    unsigned char lines[3][40] = {{0}}, records[3][32] = {{0}};
    unsigned char transition[0x70] = {0}, next[40] = {0};
    void *items[] = {segments[0], segments[1], segments[2]};
    void *entries[] = {records[0], records[1], records[2]};
    put_int(list, 12, 2); put_ptr(list, 32, items);
    float time = 10;
    for (int i = 0; i < 3; ++i) {
        put_ptr(segments[i], 0x28, lines[i]); put_int(lines[i], 12, 1);
        put_ptr(lines[i], 32, entries + i); memcpy(records[i] + 12, &time, 4);
    }
    for (int level = 0; level < 6; ++level) {
        int seen[2] = {0};
        for (uintptr_t racer = 0; racer < 64; ++racer) {
            void *chosen = varied_branch(list, level, racer * 0x100);
            assert(chosen == segments[0] || chosen == segments[1]);
            seen[chosen == segments[1]] = 1;
            assert(chosen == varied_branch(list, level, racer * 0x100));
        }
        assert(seen[0] && seen[1]);
        put_int(segments[0], 0x60, 1);
        assert(varied_branch(list, level, 0) == segments[1]);
        put_int(segments[0], 0x60, 0);
    }
    /* Equivalent data at new addresses gives the same choice after migration. */
    unsigned char moved_segments[3][0x68];
    memcpy(moved_segments, segments, sizeof segments);
    void *moved_items[] = {moved_segments[0], moved_segments[1], moved_segments[2]};
    unsigned char moved_list[40]; memcpy(moved_list, list, sizeof list);
    put_ptr(moved_list, 32, moved_items);
    for (uintptr_t racer = 0; racer < 64; ++racer)
        assert((varied_branch(list, 5, racer) == segments[1]) ==
               (varied_branch(moved_list, 5, racer) == moved_segments[1]));
    /* An unflagged jump approach must be chosen BEFORE the flagged shortcut. */
    put_ptr(segments[0], 0x18, transition); put_ptr(segments[1], 0x18, transition);
    put_ptr(transition, 0x48, next); put_int(next, 12, 1); put_ptr(next, 32, items + 2);
    segments[2][0x64] = 1;
    float x = 20; memcpy(segments[0] + 0x48, &x, 4);
    for (uintptr_t racer = 0; racer < 8; ++racer)
        assert(varied_branch(list, 5, racer) == segments[1]);
    segments[2][0x65] = 1; /* No hazardous shortcut planning. */
    assert(!isfinite(shortcut_approach(segments[1])));
    assert(!varied_branch(list, -1, 0));
}
static int owner_decisions, follower_reads, input_level;
static void ABI input_mock(void *self, void *input) {
    unsigned char *entity = ptr(self, 0x18), *wire = ptr(self, 0x70);
    if (entity[0x3b] & 1) {
        ++owner_decisions; input_level = driving_level();
        wire[0x10] = (wire[0x10] & 0xc0) | (*(unsigned char *)input & 0x3f);
    } else {
        ++follower_reads;
        assert(driving_level() == -1); /* No custom decisions on a peer. */
        *(unsigned char *)input = wire[0x10] & 0x3f;
    }
}
static unsigned char difficulty_owned;
static void *ABI difficulty_context_mock(void) { return (void *)1; }
static unsigned char ABI difficulty_owned_mock(void *context, void *object) {
    assert(context && object); return difficulty_owned;
}
static void test_difficulty_mode_scope(void) {
    unsigned char game[0x150] = {0}, race[0xd50] = {0};
    void *g = game, *r = race;
    client = &g; manager = &r;
    for (int mode = 0; mode <= 16; ++mode)
        for (int next = 0; next <= 13; ++next)
            for (int current = 0; current <= 13; ++current) {
                put_int(game, 0x144, mode);
                put_int(race, 0xd40, next); put_int(race, 0xd4c, current);
                private_room = 1; cup = next == 6; battle_menu = next != 1 && next != 6;
                assert(custom_ai_allowed() == ((mode == 8 && (current == 1 || current == 6)) ||
                       (mode == 16 && (current == 1 || current == 6))));
                assert(custom_ai_menu_allowed() == ((mode == 8 && (next == 1 || next == 6)) ||
                       (mode == 16 && (next == 1 || next == 6))));
                if (mode == 16) {
                    private_room = 0;
                    assert(!custom_ai_allowed() && !custom_ai_menu_allowed());
                }
            }
    client = manager = NULL; private_room = 1; cup = battle_menu = 0;
}

static void test_private_difficulty_gate(void) {
    unsigned char diff[32]={0}, options[0x30]={0};
    void *d=diff,*s=options;
    difficulty_manager=&d;settings=&s;native_difficulty=difficulty;
    for (private_room=0;private_room<2;++private_room)
        for(int ai=0;ai<2;++ai)
            for(int level=0;level<6;++level) {
                options[0x10]=ai;
                put_int(diff,12,level>2?2:level);
                ai_profile=level>2?level-2:0;
                assert(runtime_private_difficulty_modified()==(private_room && ai && level>=2));
            }
    private_room=1; options[0x10]=1; put_int(diff,12,2); battle_menu=1;
    for (cup=0;cup<2;++cup) assert(!runtime_private_difficulty_modified());
    cup=battle_menu=0;
    private_room=1;ai_profile=0;settings=difficulty_manager=NULL;
}

static void test_room_difficulty_restore(void) {
    unsigned char game[0x150] = {0}, diff[32] = {0}, ai[0x80] = {0};
    unsigned char entity[0x40] = {0}, wire[0x18] = {0}, actions = 1;
    void *g = game, *d = diff;
    client = &g; difficulty_manager = &d; native_difficulty = room_difficulty;
    net_context = difficulty_context_mock; owns_object = difficulty_owned_mock;
    native_ai_input = input_mock;
    put_ptr(ai,0x18,entity); put_ptr(ai,0x70,wire); put_int(game,0x144,16);
    for (int personal = 0; personal < 6; ++personal) for (int host = 0; host < 6; ++host) {
        runtime_race_options_leave_room();
        put_int(diff,12,personal > 2 ? 2 : personal); ai_profile = personal > 2 ? personal - 2 : 0;
        put_int(diff,16,host > 2 ? 2 : host);
        difficulty_owned = 0; entity[0x3b] = 0;
        wire[0x10] = 1 | ((host > 2 ? host - 2 : 0) << 6);
        ai_input(ai,&actions);
        assert(driving_level() == host);
        difficulty_owned = 1; entity[0x3b] = 1;
        ai_input(ai,&actions); assert(input_level == host); /* Both entity and manager takeover. */
        assert(integer(diff,12) == (personal > 2 ? 2 : personal));
        /* CUP loads all four maps without a lobby reset, recreating AI each time. */
        const char *tracks[] = {
            "octane/t111_crash_cove/t111_crash_cove",
            "octane/t112_mystery_caves/t112_mystery_caves",
            "octane/t113_sewer_speedway/t113_sewer_speedway",
            "octane/t114_roos_tubes/t114_roos_tubes",
        };
        for (int track = 0; track < 4; ++track) {
            runtime_race_options_map_loading(tracks[track]);
            assert(driving_level() == host);
            assert(followed_ai_base == (host > 2 ? 2 : host));
            assert(followed_ai_profile == (host > 2 ? host - 2 : 0));
            /* Recreated owner publishes the retained host choice. */
            memset(wire, 0, sizeof wire);
            difficulty_owned = 1; entity[0x3b] = 1;
            ai_input(ai, &actions);
            assert(input_level == host);
            assert((wire[0x10] >> 6) == (host > 2 ? host - 2 : 0));
            difficulty_owned = 0; entity[0x3b] = 0;
            ai_input(ai, &actions);
            assert(driving_level() == host);
            assert(integer(diff,12) == (personal > 2 ? 2 : personal));
        }
        runtime_race_options_map_loading("octane/octane_onlinelobby/octane_onlinelobby");
        difficulty_owned = 0;
        assert(driving_level() == personal); /* Back in lobby, personal choice without field writes. */
        followed_ai_profile = 3; followed_ai_base = 2; /* A late race packet cannot change lobby choice. */
        assert(driving_level() == personal);
        runtime_race_options_map_loading("octane/t111_crash_cove/t111_crash_cove");
        entity[0x3b] = 0; ai_input(ai,&actions); assert(driving_level() == host);
    }
    private_room=0; stale_private_ui=1; followed_ai_profile=3; followed_ai_base=2; lobby_personal_difficulty=1;
    put_int(diff,12,0); put_int(diff,16,1);
    difficulty_owned=0; assert(room_difficulty(diff)==1);
    difficulty_owned=1; assert(room_difficulty(diff)==0);
    assert(effective_profile()==0 && driving_level()==-1);
    private_room=1;stale_private_ui=0;
    runtime_race_options_leave_room(); client = difficulty_manager = NULL; ai_profile = 0;
    net_context = NULL; owns_object = NULL; native_difficulty = difficulty;
}
static void test_authority_profile(void) {
    unsigned char game[0x150] = {0}, diff[32] = {0}, ai[0x80] = {0};
    unsigned char entity[0x40] = {0}, wire[0x18] = {0}, actions;
    void *g = game, *d = diff;
    client = &g; difficulty_manager = &d; native_difficulty = difficulty;
    native_ai_input = input_mock;
    put_ptr(ai, 0x18, entity); put_ptr(ai, 0x70, wire);
    put_int(game, 0x144, 16); put_int(diff, 12, 2); put_int(diff, 16, 2);
    for (int profile = 0; profile < 4; ++profile) {
        for (unsigned bits = 0; bits < 64; ++bits) {
            runtime_race_options_leave_room(); ai_profile = profile;
            entity[0x3b] = 1; actions = bits;
            ai_input(ai, &actions);
            assert(wire[0x10] == (bits | ((unsigned)profile << 6)));
            assert(input_level == 2 + profile);
            /* Other client has a different local choice; owner packet wins. */
            ai_profile = (profile + 1) % 4;
            entity[0x3b] = 0; actions = 0xff;
            const int decisions = owner_decisions;
            ai_input(ai, &actions);
            assert(owner_decisions == decisions && actions == bits);
            assert(followed_ai_profile == profile && effective_profile() == profile);
            /* Native transfers entity ownership: retain received profile. */
            entity[0x3b] = 1;
            ai_input(ai, &actions);
            assert(input_level == 2 + profile && wire[0x10] >> 6 == profile);
        }
    }
    /* Public input never advertises/adopts custom profile bits, on either peer role. */
    private_room=0; stale_private_ui=1; ai_profile=3; followed_ai_profile=3; followed_ai_base=2;
    entity[0x3b]=1; wire[0x10]=0x80; actions=9;
    ai_input(ai,&actions);
    assert(input_level==-1 && wire[0x10]==0x89);
    assert(followed_ai_profile==-1 && followed_ai_base==-1);
    entity[0x3b]=0; wire[0x10]=0xc9; actions=0;
    ai_input(ai,&actions);
    assert(actions==9 && wire[0x10]==0xc9 && followed_ai_profile==-1);
    private_room=1;stale_private_ui=0; entity[0x3b]=1;
    runtime_race_options_leave_room(); assert(followed_ai_profile == -1);
    put_int(game, 0x144, 8); ai_profile = 1; followed_ai_profile = 3;
    wire[0x10] = 0x80; actions = 1; ai_input(ai, &actions);
    assert(effective_profile() == 1 && (wire[0x10] & 0xc0) == 0x80);
    unsigned char racers[2][0x170] = {{0}}, handles[2][0x30] = {{0}}, controllers[2][0x30] = {{0}};
    for (int i = 0; i < 2; ++i) {
        put_ptr(controllers[i], 0x28, handles[i]); put_ptr(handles[i], 0x28, racers[i]);
        put_int(racers[i], 0x164, 7); put_int(racers[i], 0x4c, 2);
    }
    assert(route_identity(controllers[0]) == route_identity(controllers[1]));
    put_int(racers[1], 0x4c, 3);
    assert(route_identity(controllers[0]) != route_identity(controllers[1]));
    runtime_race_options_leave_room(); ai_profile = 0; client = difficulty_manager = NULL;
}
static void test_lines(void) {
    unsigned char list[40] = {0}, entries[3][32] = {{0}}, segments[3][0x68] = {{0}};
    void *items[] = {entries[0], entries[1], entries[2]};
    put_ptr(list, 0x20, items); put_int(list, 12, 3);
    const float times[] = {4, 8, 12};
    for (int i = 0; i < 3; ++i) {
        put_ptr(entries[i], 0x10, segments[i]); memcpy(entries[i] + 12, times + i, 4);
    }
    segments[0][0x65] = 1; /* Fast but hazardous, excluded by native policy. */
    float last = 12;
    for (int level = 0; level < 6; ++level) {
        float target = line_target(list, 1, 12, level);
        assert(target <= last && target >= 8); last = target;
    }
    assert(last == 8 && line_target(list, 0, 12, 5) == 4);
    assert(line_target(list, 1, 12, -1) == 12); /* Adventure / inactive. */
    assert(line_target(list, 1, 6, 5) == 6); /* Never slow an already faster target. */
    for (int i = 0; i < 3; ++i) segments[i][0x65] = 1;
    assert(line_target(list, 1, 12, 5) == 12); /* No eligible path: retain native choice. */
}
static void ABI choose_difficulty(unsigned char *self, unsigned char *event, void *owner) {
    (void)owner;
    const int chosen = integer(event, 0x18) - self[0x1b9];
    assert(chosen >= -1 && chosen <= 2); /* No extended enum reaches boss/save/network data. */
    put_int(*difficulty_manager, 12, chosen < 0 ? 0 : chosen);
}
static int difficulty_refreshes;
static void ABI refresh_difficulty(void *self) {
    ++difficulty_refreshes;
    put_int(self, 0x1a0, integer(self, 0x190));
}
static void test_ai(void) {
    unsigned char self[0x1e0] = {0}, choices[40] = {0}, available[40] = {0}, event[32] = {0};
    unsigned char native[0x30] = {0}, root[0x128] = {0}, values[9][32] = {{0}}, game[0x150] = {0};
    void *game_ptr = game; client = &game_ptr; put_int(game, 0x144, 8);
    void *data = root, *diff = native, *vtable[] = {kind};
    void *names[8] = {0}; unsigned char flags[8] = {1,1,1,1,1,1,1,1};
    const float hard[] = {.25f,.25f,1.5f,.01f,3,70,25,5,.75f};
    ai_data = &data; difficulty_manager = &diff; native_difficulty = difficulty;
    intern_string = intern; append_choice = append; difficulty_event_original = choose_difficulty;
    refresh = refresh_difficulty; difficulty_refreshes = 0;
    put_ptr(self, 0x198, self);
    put_ptr(self, 0x1a8, choices); put_ptr(self, 0x1b0, available);
    put_ptr(choices, 0x20, names); put_ptr(available, 0x20, flags); put_ptr(event, 0, vtable);
    for (int i = 0; i < 9; ++i) {
        put_ptr(root, tuning[i].offset, values[i]);
        if (tuning[i].integral) put_int(values[i], 0x14, (int)hard[i]);
        else memcpy(values[i] + 0x14, &hard[i], 4);
        put_int(values[i], 0xc, 123); put_int(values[i], 0x10, 456); put_int(values[i], 0x18, 789);
    }
    for (int no_ai = 1; no_ai <= 1; ++no_ai) {
        self[0x1b9] = (unsigned char)no_ai;
        put_int(choices, 12, 3 + no_ai); put_int(available, 12, 3 + no_ai);
        for (int profile = 1; profile <= 3; ++profile) {
            put_int(event, 0x18, profile + 2 + no_ai);
            difficulty_event(self, event, NULL);
            assert(integer(choices, 12) == 6 + no_ai && integer(native, 12) == 2);
            assert(integer(self, 0x190) == profile + 2 + no_ai);
            assert(integer(event, 0x18) == profile + 2 + no_ai);
            assert(!strcmp(names[2 + no_ai + profile], profile_names[profile - 1]));
            float lock; memcpy(&lock, values[0] + 0x14, 4);
            assert(lock == hard[0] * tuning[0].scale[profile - 1]);
            tune_ai(root); /* Repeated reads must not compound the multiplier. */
            float again; memcpy(&again, values[0] + 0x14, 4); assert(again == lock);
            for (int i = 0; i < 9; ++i) assert(integer(values[i], 24) == 789);
        }
        /* Native navigation updates +1a0 before delivering the change event.
         * Exercise both directions, lower levels, No AI and wrap-around. */
        const int sequence[] = {5,4,3,2,1,0,6,0,1,2,3,4,5,6};
        for (size_t i = 0; i < sizeof sequence / sizeof sequence[0]; ++i) {
            const int selected = sequence[i];
            put_int(self, 0x1a0, selected); put_int(event, 0x18, selected);
            difficulty_event(self, event, NULL);
            assert(integer(self, 0x1a0) == selected);
            assert(integer(self, 0x190) == selected);
            assert(integer(event, 0x18) == selected);
            assert(integer(choices, 12) == 7);
            assert(difficulty_refreshes == 1); /* Only after extending initial labels. */
        }
        put_int(event, 0x18, 2 + no_ai); difficulty_event(self, event, NULL);
        /* Story must restore native data even after a stronger arcade profile. */
        put_int(game, 0x144, 1); tune_ai(root);
        for (int i = 0; i < 9; ++i) {
            float value;
            if (tuning[i].integral) value = (float)integer(values[i], 0x14);
            else memcpy(&value, values[i] + 0x14, 4);
            assert(value == hard[i]);
            assert(integer(values[i], 12) == 123 && integer(values[i], 16) == 456);
        }
        /* Reused private selector must lose extended entries in Public. */
        private_room=0; put_int(game,0x144,16); put_int(event,0x18,6);
        difficulty_event(self,event,NULL);
        assert(integer(choices,12)==4 && integer(available,12)==4);
        assert(integer(native,12)==2 && integer(event,0x18)==6);
        assert(integer(self,0x190)<4 && integer(self,0x1a0)<4);
        for (int i=4;i<7;++i) assert(names[i]==NULL);
        private_room=1; put_int(game,0x144,1);
    }
    /* Adventure selector keeps only its three native entries. */
    self[0x1b9] = 0; put_int(choices, 12, 3); put_int(available, 12, 3);
    put_int(event, 0x18, 2); difficulty_event(self, event, NULL);
    assert(integer(choices, 12) == 3);
    /* Private -> Public -> Private restores every native column without losing the local preference. */
    put_int(game,0x144,16); ai_profile=3; followed_ai_profile=-1;
    unsigned char public_ai[0x80]={0}, public_entity[0x40]={0}, public_wire[0x18]={0}, action=9;
    put_ptr(public_ai,0x18,public_entity); put_ptr(public_ai,0x70,public_wire);
    public_entity[0x3b]=1; native_ai_input=input_mock;
    for (int repeat=0;repeat<3;++repeat) {
        private_room=1; tune_ai(root);
        assert(effective_profile()==3 && selected_driving_level()==5);
        assert(real(values[0],0x14)==hard[0]*tuning[0].scale[2]);
        private_room=0; stale_private_ui=1; followed_ai_profile=3; followed_ai_base=2;
        ai_input(public_ai,&action);
        assert(followed_ai_profile==-1 && followed_ai_base==-1);
        assert(effective_profile()==0 && selected_driving_level()==-1 && ai_profile==3);
        for (int i=0;i<9;++i) {
            const float actual=tuning[i].integral ? (float)integer(values[i],20) : real(values[i],20);
            assert(actual==hard[i]);
            assert(integer(values[i],12)==123 && integer(values[i],16)==456);
        }
        followed_ai_profile=followed_ai_base=-1;
    }
    private_room=1;
    /* Itemless leaves Easy/Medium native, including after Normal tuning. */
    put_int(game,0x144,16); followed_ai_profile=-1;
    for (int repeat=0;repeat<3;++repeat) {
        itemless=0; tune_ai(root);
        itemless=1; tune_ai(root);
        for (int i=0;i<9;++i)
            assert(integer(values[i],12)==123 && integer(values[i],16)==456);
        for (int level=0;level<6;++level) {
            put_int(native,12,level<2 ? level : 2);
            ai_profile=level>2 ? level-2 : 0;
            assert(selected_driving_level()==(level<2 ? -1 : level));
            itemless=0; assert(selected_driving_level()==level); itemless=1;
        }
    }
    itemless=0;
    /* Every excluded Arcade mode and private Battle restores all native tuning. */
    for (int mode = 8; mode <= 16; mode += 8) {
        put_int(game, 0x144, mode);
        for (int race = 0; race <= 13; ++race) {
            put_int(*manager, 0xd4c, race);
            ai_profile = 3; followed_ai_profile = -1;
            tune_ai(root);
            const int allowed = race == 1 || race == 6;
            assert(custom_ai_allowed() == allowed);
            assert(effective_profile() == (allowed ? 3 : 0));
            if (!allowed) for (int i = 0; i < 9; ++i) {
                assert(integer(values[i], 12) == 123 && integer(values[i], 16) == 456);
                assert((tuning[i].integral ? (float)integer(values[i], 20) : real(values[i], 20)) == hard[i]);
            }
        }
    }
    put_int(*manager, 0xd4c, 1);
    /* Reusing the extended selector in private Battle removes custom entries. */
    self[0x1b9] = 1;
    put_int(choices, 12, 4); put_int(available, 12, 4);
    put_int(event, 0x18, 0);
    put_int(game, 0x144, 8); put_int(*manager, 0xd40, 1);
    difficulty_event(self, event, NULL);
    assert(integer(choices, 12) == 7);
    battle_menu = 1; put_int(game, 0x144, 16);
    difficulty_event(self, event, NULL);
    assert(integer(choices, 12) == 4 && integer(available, 12) == 4);
    for (int i = 4; i < 7; ++i) assert(names[i] == NULL);
    battle_menu = 0;
    /* A guest cannot replace an authority profile through a selection event. */
    self[0x1b9] = 1; followed_ai_profile = 3; ai_profile = 0;
    put_int(event, 0x18, 6);
    difficulty_event(self, event, NULL); /* No ownership callback => fail closed. */
    assert(ai_profile == 0 && followed_ai_profile == 3);
    runtime_race_options_leave_room();
    put_int(game, 0x144, 1); tune_ai(root);
    client = NULL;
    ai_data = NULL; difficulty_manager = NULL; baseline_root = NULL;
}
static unsigned char use_box[0x30];
static void *ABI box_component(void *entity, void *type, int unused) {
    (void)entity; (void)type; (void)unused; return use_box;
}
static void ABI position_mock(float *out, void *entity) { memcpy(out, (char *)entity + 0x20, 12); }
static void test_ghost_traffic(void) {
    unsigned char root[32]={0}, list[40]={0}, racer[0x170]={0}, handle[0x30]={0};
    unsigned char self[0x80]={0}, other[0x80]={0};
    void *r=root,*items[]={racer}; racer_manager=&r; world_position=position_mock;
    put_ptr(root,0x10,list); put_ptr(list,32,items); put_int(list,12,1);
    put_ptr(racer,0x20,handle); put_ptr(handle,0x28,other); put_int(racer,0x164,1);
    float pos[]={0,0,0}, peer[]={400,0,0}, line[2][3]={{500,0,0},{1000,0,0}}, target[2][3];
    memcpy(other+0x20,peer,sizeof peer); memcpy(target,line,sizeof line);
    avoid_reference_traffic(self,0,pos,target,0);
    assert(target[0][1]<0 && target[0][1]>=-90 && target[0][0]==500);
    memcpy(target,line,sizeof line); avoid_reference_traffic(self,2,pos,target,0);
    assert(target[0][1]>0); /* Deterministic separation from the other side. */
    memcpy(target,line,sizeof line); avoid_reference_traffic(self,0,pos,target,4);
    assert(!memcmp(target,line,sizeof line)); /* Shortcut geometry is protected. */
    peer[2]=500; memcpy(other+0x20,peer,sizeof peer);
    avoid_reference_traffic(self,0,pos,target,0); assert(!memcmp(target,line,sizeof line)); /* Other bridge level. */
    peer[2]=0; peer[0]=1500; memcpy(other+0x20,peer,sizeof peer);
    avoid_reference_traffic(self,0,pos,target,0); assert(!memcmp(target,line,sizeof line));
    peer[0]=400; memcpy(other+0x20,peer,sizeof peer);
    line[1][0]=500; line[1][1]=500; memcpy(target,line,sizeof line);
    avoid_reference_traffic(self,0,pos,target,0); assert(!memcmp(target,line,sizeof line)); /* Tight corner. */
    racer_manager=NULL;
}
static int targets_recovery_seen;
static void ABI native_targets_mock(unsigned char *self, unsigned char *ai) {
    (void)self; targets_recovery_seen=native_recovery_context;
    float native[2][3] = {{-1000,2000,0},{-2000,3000,0}};
    memcpy(ai+0x28,native,sizeof native);
    ai[0x7b]=1; ai[0x7d]=1;
}
static uint32_t recovery_clock;
static uint32_t ABI recovery_now(void *timer) { (void)timer; return recovery_clock; }
static void test_stuck_recovery(void) {
    float p[3]={0}, v[3]={0};
    void *owner=(void *)1;
    assert(!recover_stuck(0,owner,0,0,0,p,v,0));
    assert(!recover_stuck(0,owner,0,81920,0,p,v,0)); /* Grid/countdown never arms it. */
    assert(!recover_stuck(0,owner,0,81920,1,p,v,0));
    assert(!recover_stuck(0,owner,0,81920+8191,1,p,v,0));
    assert(recover_stuck(0,owner,0,81920+8192,1,p,v,0));
    uint32_t start=81920+8192;
    p[0]=400; v[0]=500;
    assert(recover_stuck(0,owner,0,start+8192,1,p,v,0)); /* Moving off ghost stays native. */
    assert(recover_stuck(0,owner,0,start+16384,1,p,v,(TT_REFERENCE_ALIGNED|TT_REFERENCE_REJOIN_READY)));
    assert(recover_stuck(0,owner,0,start+16384+6143,1,p,v,(TT_REFERENCE_ALIGNED|TT_REFERENCE_REJOIN_READY)));
    assert(!recover_stuck(0,owner,0,start+16384+6144,1,p,v,(TT_REFERENCE_ALIGNED|TT_REFERENCE_REJOIN_READY)));
    v[0]=0;
    assert(!recover_stuck(0,owner,0,start+16384+6145,1,p,v,0)); /* No instant oscillation. */
    assert(!recover_stuck(0,owner,1,start+50000,1,p,v,0)); /* New lap resets state. */
    assert(!recover_stuck(0,(void *)2,1,start+60000,1,p,v,0)); /* New owner too. */
    /* Landing below the shortcut must recover even while still moving. */
    p[0]=0; v[0]=1500;
    assert(!recover_stuck(0,owner,2,100000,1,p,v,0));
    assert(!recover_stuck(0,owner,2,100001,1,p,v,TT_REFERENCE_BELOW_ROUTE|4));
    p[0]=300;
    assert(!recover_stuck(0,owner,2,102048,1,p,v,TT_REFERENCE_BELOW_ROUTE|4));
    assert(!recover_stuck(0,owner,2,102049,1,p,v,TT_REFERENCE_BELOW_ROUTE|4)); /* Fast grounded hill travel is not stuck. */
    v[0]=200;
    assert(recover_stuck(0,owner,2,102050,1,p,v,TT_REFERENCE_BELOW_ROUTE|4));
    p[0]=800; v[0]=500;
    assert(recover_stuck(0,owner,2,120000,1,p,v,(TT_REFERENCE_ALIGNED|TT_REFERENCE_REJOIN_READY)|4));
    assert(recover_stuck(0,owner,2,130000,1,p,v,TT_REFERENCE_ALIGNED|4));
    assert(!recover_stuck(0,owner,2,130001,1,p,v,(TT_REFERENCE_ALIGNED|TT_REFERENCE_REJOIN_READY)|4)); /* Safe 3D alignment can rejoin inside the broad terrain window. */
    /* A brief contact/airborne mismatch does not trigger recovery. */
    assert(!recover_stuck(0,owner,3,140000,1,p,v,0));
    assert(!recover_stuck(0,owner,3,140001,1,p,v,TT_REFERENCE_BELOW_ROUTE|4));
    assert(!recover_stuck(0,owner,3,141000,1,p,v,4));
    assert(!recover_stuck(0,owner,3,142049,1,p,v,TT_REFERENCE_BELOW_ROUTE|4));
    memset(stuck_recovery,0,sizeof stuck_recovery);
}
static void test_boxes(void) {
    unsigned char ai[0x80] = {0}, entity[0x80] = {0}, controller[0x48] = {0}, handle[0x30] = {0};
    unsigned char collection[0x20] = {0}, list[40] = {0}, crate[2][0x30] = {{0}};
    unsigned char data[2][0x20] = {{0}}, objects[2][0x40] = {{0}};
    void *c = collection, *type = (void *)1, *items[] = {crate[0], crate[1]};
    collectible_manager = &c; power_use_type = &type; component = box_component; world_position = position_mock;
    put_ptr(use_box, 0x28, handle); put_ptr(handle, 0x28, controller);
    put_ptr(collection, 0x10, list); put_ptr(list, 32, items); put_int(list, 12, 2);
    for (int i = 0; i < 2; ++i) {
        put_ptr(crate[i], 0x10, data[i]); put_ptr(crate[i], 0x18, objects[i]); crate[i][0x2c] = 1;
        float p[] = {400.f + i * 100, 0, 0}; memcpy(objects[i] + 0x20, p, sizeof p);
    }
    float position[] = {0,0,0}, targets[2][3] = {{200,0,0},{800,0,0}};
    assert(aim_at_box(ai, entity, 0, 5, position, targets, 0, 10));
    assert(targets[0][0] == 400 && box_claims[0] == objects[0]);
    /* Itemless cancels an existing claim without redirecting TT/shortcut targets. */
    itemless = 1;
    float racing_targets[2][3] = {{200,0,0},{800,0,0}};
    for (int kind = 0; kind <= 1; ++kind) {
        put_int(data[0], 0x18, kind);
        box_claims[0] = objects[0];
        assert(!aim_at_box(ai, entity, 0, 5, position, racing_targets, 1, 3));
        assert(!box_claims[0] && box_kind[0] == -1);
        assert(racing_targets[0][0] == 200 && racing_targets[1][0] == 800);
    }
    put_int(data[0], 0x18, 0);
    itemless = 0;
    assert(aim_at_box(ai, entity, 0, 5, position, targets, 0, 10));
    assert(aim_at_box(ai, entity, 1, 5, position, targets, 0, 10));
    assert(targets[0][0] == 500); /* Other AI's claim discourages a convoy. */
    assert(aim_at_box(ai, entity, 0, 5, position, targets, 1, 10)); /* Plan pickup before starting a reference slide. */
    ai[0x7c]=1;
    assert(aim_at_box(ai, entity, 0, 5, position, targets, 0, 10)); /* Normal pickups remain eligible during a slide. */
    ai[0x7c]=0;
    assert(aim_at_box(ai, entity, 0, 5, position, targets, 4, 10)); /* Distant terrain does not veto a grounded pickup. */
    assert(!aim_at_box(ai, entity, 0, 5, position, targets, 2, 10));
    assert(!aim_at_box(ai, entity, 0, 5, position, targets, 512, 10));
    assert(aim_at_box(ai, entity, 0, 5, position, targets, 2|TT_REFERENCE_DRIFT_HOP, 10));
    assert(!aim_at_box(ai, entity, 0, 5, position, targets, TT_REFERENCE_BOX_BLOCK, 10)); /* Immediate takeoff approach. */
    crate[0][0x2c] = 0; crate[1][0x2d] = 1;
    assert(!aim_at_box(ai, entity, 0, 5, position, targets, 0, 10));
    crate[1][0x2d] = 0; put_int(data[1], 0x18, 1);
    assert(!aim_at_box(ai, entity, 0, 5, position, targets, 0, 10)); /* Wumpa is not an item. */
    put_int(data[1], 0x18, 0); controller[0x40] = 1;
    assert(!aim_at_box(ai, entity, 0, 5, position, targets, 0, 10));
    controller[0x40] = 0; put_ptr(controller, 0x30, handle);
    assert(!aim_at_box(ai, entity, 0, 5, position, targets, 0, 10));
    /* Wumpa boxes remain useful with an item already held or during roulette. */
    put_int(data[1],0x18,1);
    assert(aim_at_box(ai,entity,0,5,position,targets,0,3));
    assert(box_kind[0]==1);
    ai[0x7c]=1;
    assert(aim_at_box(ai,entity,0,5,position,targets,4,3) && box_kind[0]==1);
    ai[0x7c]=0;
    assert(!aim_at_box(ai,entity,0,5,position,targets,0,10));
    assert(!aim_at_box(ai,entity,0,5,position,targets,0,-1));
    put_ptr(controller,0x30,NULL); controller[0x40]=1;
    assert(aim_at_box(ai,entity,0,5,position,targets,0,5));
    put_int(data[1],0x18,3); /* Switch/hazard categories are not collectible rewards. */
    assert(!aim_at_box(ai,entity,0,5,position,targets,0,0));
    /* Retain a viable box while another becomes marginally closer. */
    controller[0x40]=0; crate[0][0x2c]=1; put_int(data[1],0x18,0);
    memset(box_claims,0,sizeof box_claims);
    assert(aim_at_box(ai,entity,0,5,position,targets,0,10));
    float closer=390; memcpy(objects[1]+0x20,&closer,4);
    assert(aim_at_box(ai,entity,0,5,position,targets,0,10));
    assert(box_claims[0]==objects[0]);
    for (int peer=1;peer<8;++peer) box_claims[peer]=objects[0];
    assert(aim_at_box(ai,entity,0,5,position,targets,0,10));
    assert(box_claims[0]==objects[0]); /* Late peer claims cannot flip the chosen approach. */
    memset(box_claims+1,0,7*sizeof box_claims[0]);
    float behind[] = {-100,0,0}, upper[] = {100,0,200}, wide[] = {100,500,0};
    assert(isinf(crate_score(position, targets[1], behind, 5, 0)));
    assert(isinf(crate_score(position, targets[1], upper, 5, 0)));
    assert(isinf(crate_score(position, targets[1], wide, 5, 0)));
    float near_box[]={40,100,0};
    assert(isinf(crate_score(position,targets[1],near_box,5,0)));
    assert(isfinite(crate_score(position,targets[1],near_box,5,1))); /* Keep the approach through contact. */
    assert(isinf(crate_score(position,targets[1],behind,5,1))); /* Never chase a missed box backwards. */
    unsigned char drive[0x80]={0}, kart[0x218]={0}, kart_handle[0x30]={0};
    put_ptr(ai,0x48,drive); put_ptr(drive,0x58,kart_handle); put_ptr(kart_handle,0x28,kart);
    put_ptr(kart,0x18,entity);
    assert(!aim_at_box(ai,entity,0,5,position,targets,0,0)); /* Airborne pickup detours are disabled. */
    kart[0x214]=1;
    assert(aim_at_box(ai,entity,0,5,position,targets,0,0));
    put_ptr(ai,0x48,NULL);
    /* Exercise the complete tactical hook: native targets point away from the
     * ghost and request recovery, while a collectible sits on the ghost path. */
    char path[96]; snprintf(path,sizeof path,"/tmp/ghost-route-%ld.bin",(long)getpid());
    FILE *file=fopen(path,"wb"); assert(file);
    const uint32_t count=1, samples=40;
    char map[64]="route_test__developer";
    assert(fwrite("BBGHOST1",1,8,file)==8 && fwrite(&count,4,1,file)==1);
    assert(fwrite(map,1,64,file)==64 && fwrite(&samples,4,1,file)==1);
    for (unsigned i=0;i<samples;++i) {
        unsigned flags=i<20 ? 1u : 0u;
        if (i==1) flags|=8192;
        if (i==4 || i==10 || i==14) flags|=8;
        struct { float time, p[3]; uint32_t flags; } point={i*.1f,{i*100.f,0,0},flags};
        assert(fwrite(&point,sizeof point,1,file)==1);
    }
    fclose(file); assert(runtime_tt_reference_load(path)==1); unlink(path);
    unsigned char game[0x150]={0}, diff[32]={0}, tactical[0x80]={0};
    unsigned char racer[0x180]={0}, racer_handle[0x30]={0};
    void *g=game, *d=diff;
    client=&g; difficulty_manager=&d; native_difficulty=difficulty; ai_profile=3;
    followed_ai_profile=-1; peer_decision=0; put_int(diff,12,2);
    put_ptr(game,0xc0,"route_test"); put_ptr(tactical,0x18,entity);
    put_ptr(tactical,0x28,racer_handle); put_ptr(racer_handle,0x28,racer);
    put_int(racer,0x9c,10); entity[0x3b]=1;
    float velocity[]={1000,0,0}; memcpy(entity+0x60,velocity,sizeof velocity);
    native_race_targets=native_targets_mock;
    for (int mode=8;mode<=16;mode+=8) {
        put_int(game,0x144,mode);
        for (itemless=0;itemless<=1;++itemless) {
            memset(box_claims,0,sizeof box_claims);
            race_targets(tactical,ai);
            if (itemless && mode==16) {
                assert(reference_slot==0 && !ai[0x7b] && !ai[0x7d]);
                assert(!box_claims[0] && real(ai,0x28)==250.f);
            } else {
                assert(reference_slot==-1 && ai[0x7b] && ai[0x7d]);
                assert(real(ai,0x28)==-1000.f); /* Normal/Arcade keep native targets. */
            }
        }
    }
    itemless=0;
    for (int mode=8;mode<=16;mode+=8) for (int profile=0;profile<4;++profile) {
        put_int(game,0x144,mode); ai_profile=profile;
        race_targets(tactical,ai);
        assert(reference_slot==-1 && real(ai,0x28)==-1000.f);
    }
    ai_profile=3;
    itemless=1; crate[0][0x2c]=crate[1][0x2c]=0;
    race_targets(tactical,ai);
    assert(!box_claims[0] && real(ai,0x28)==250.f); /* After pickup, resume ghost. */
    /* Same route, three recorded rhythms: no universal perfect-meter target. */
    unsigned char style_kart[0x218]={0}, style_handle[0x30]={0}, style_archetype[0xb0]={0};
    unsigned char style_data[0x30]={0}, style_timer[24]={0};
    put_ptr(style_kart,0xb8,style_handle); put_ptr(style_handle,0x28,style_archetype);
    put_ptr(style_archetype,0xa8,style_data); put_ptr(style_kart,0xf8,style_timer);
    float maximum=1, along=150; memcpy(style_data+0x20,&maximum,4); memcpy(entity+0x20,&along,4);
    race_targets(tactical,ai); native_boost_ready=ready;
    const float recorded[]={.3f,.6f,.4f};
    for (int completed=0;completed<3;++completed) {
        put_int(style_kart,0xec,completed);
        assert(!boost_decision(style_kart,.8f,1));
        assert(fabsf(last_threshold-recorded[completed])<.001f);
    }
    memset(entity+0x20,0,12);
    unsigned char clock_object[24]={0}; void *clock_vtable[24]={0};
    clock_vtable[0xb0/8]=recovery_now; put_ptr(clock_object,0,clock_vtable);
    put_ptr(tactical,0x68,clock_object); ai[0x78]=1;
    memset(entity+0x60,0,sizeof velocity); recovery_clock=100;
    race_targets(tactical,ai);
    recovery_clock+=8192; race_targets(tactical,ai);
    assert(reference_slot==-1 && ai[0x7b] && real(ai,0x28)==-1000.f); /* Actual native fallback. */
    float escaped[]={400,0,0}, moving[]={500,0,0};
    memcpy(entity+0x20,escaped,sizeof escaped); memcpy(entity+0x60,moving,sizeof moving);
    recovery_clock+=16384; race_targets(tactical,ai);
    assert(reference_slot==-1 && real(ai,0x28)==-1000.f);
    recovery_clock+=6144; race_targets(tactical,ai);
    assert(reference_slot==0 && !ai[0x7b] && real(ai,0x28)>400.f); /* Actual return to ghost. */
    stuck_recovery[0].active=1;
    put_int(racer,0x4c,1); /* Failed previous lap must not bias even the first new-lap native update. */
    race_targets(tactical,ai);
    assert(!targets_recovery_seen && !stuck_recovery[0].active && reference_slot==0);
    assert(!ai[0x7b] && real(ai,0x28)>400.f);
    memset(stuck_recovery,0,sizeof stuck_recovery);
    runtime_tt_reference_load(NULL); client=difficulty_manager=NULL; ai_profile=0;
    reference_slot=-1; reference_intent=0;
    itemless=0;
    collectible_manager = power_use_type = NULL; memset(box_claims, 0, sizeof box_claims);
}
static unsigned char scan_result = 1;
static unsigned char ABI scan_mock(void *s, void *r, const float *d, const float *a) {
    (void)s; (void)r; (void)d; (void)a; return scan_result;
}
static unsigned char ABI enemy_mock(void *r, void *other) {
    return ((unsigned char *)r)[0x90] != ((unsigned char *)other)[0x90];
}
static void test_targeting(void) {
    unsigned char config_settings[0x30]={0}; void *cs=config_settings; settings=&cs;
    put_int(config_settings,0x28,2);
    unsigned char game[0x150] = {0}, diff[32] = {0}, race[0xd50] = {0}, self[32] = {0};
    unsigned char entity[0x80] = {0}, target[0x80] = {0}, racer[0x98] = {0}, other[0x98] = {0};
    unsigned char handle[0x30] = {0}, rm[32] = {0}, list[40] = {0}, root[0x28] = {0}, limits[3][24] = {{0}};
    void *g=game, *d=diff, *m=race, *rm_ptr=rm, *root_ptr=root, *items[]={racer,other};
    client=&g; difficulty_manager=&d; manager=&m; racer_manager=&rm_ptr; ai_data=&root_ptr;
    put_int(race, 0xd4c, 1);
    native_difficulty=difficulty; native_scan_targets=scan_mock; native_enemy_racer=enemy_mock;
    world_position=position_mock; ai_profile=0; followed_ai_profile=-1; peer_decision=0;
    put_int(game,0x144,8); put_ptr(self,0x18,entity); put_ptr(other,0x20,handle); put_ptr(handle,0x28,target);
    put_ptr(rm,0x10,list); put_ptr(list,32,items); put_int(list,12,2);
    float config[]={1000,30,30}, where[]={500,0,0}, direction[]={1,0,0};
    memcpy(target+0x20,where,12);
    for(int i=0;i<3;++i) { put_ptr(root,0x10+i*8,limits[i]); memcpy(limits[i]+12,config+i,4); }
    assert(!scan_targets(self,racer,direction,NULL)); /* Friendly in native cone. */
    put_int(config_settings,0x28,1);
    assert(scan_targets(self,racer,direction,NULL)); /* FFA rivals share a native faction. */
    put_int(config_settings,0x28,2);
    other[0x90]=1; assert(scan_targets(self,racer,direction,NULL));
    /* Actual kartaisystemdata cones are 120/180/240, not all below 90. */
    const float ranges[] = {1000,3000,5000}, radii[] = {75,200,400}, cones[] = {120,180,240};
    for (int i=0;i<3;++i) {
        memcpy(limits[0]+12+i*4,ranges+i,4);
        memcpy(limits[1]+12+i*4,radii+i,4);
        memcpy(limits[2]+12+i*4,cones+i,4);
    }
    for (int level=0;level<6;++level) {
        put_int(diff,12,level<2 ? level : 2); ai_profile=level>2 ? level-2 : 0;
        assert(scan_targets(self,racer,direction,NULL));
    }
    ai_profile=0; put_int(diff,12,0);

    other[0x8c]=1; assert(!scan_targets(self,racer,direction,NULL)); other[0x8c]=0;
    where[0]=-500; memcpy(target+0x20,where,12); assert(!scan_targets(self,racer,direction,NULL));
    direction[0]=-1; assert(scan_targets(self,racer,direction,NULL)); /* Rearward item. */
    scan_result=0; assert(!scan_targets(self,racer,direction,NULL)); scan_result=1;
    other[0x90]=0; put_int(race,0xd4c,9); assert(scan_targets(self,racer,direction,NULL)); /* Native battle. */
    put_int(race,0xd4c,1); put_int(game,0x144,1); assert(scan_targets(self,racer,direction,NULL)); /* Adventure. */
    put_int(game,0x144,16); assert(scan_targets(self,racer,direction,NULL)); /* Guest: native only. */
    entity[0x3b]=1; assert(!scan_targets(self,racer,direction,NULL)); /* Authority enforces filtering. */
    put_int(config_settings,0x28,1);
    assert(scan_targets(self,racer,direction,NULL)); /* Private FFA owner can attack same-faction rivals. */

    client=difficulty_manager=manager=racer_manager=ai_data=settings=NULL;
}
static void ABI original(unsigned char *self, unsigned char *event, void *owner) {
    (void)self; (void)owner; ++calls;
    put_int(*settings, 0x28, integer(event, 0x18) + 1);
}
static uint32_t item_clock;
static unsigned char item_native_result;
static uint32_t ABI item_now(void *timer) { (void)timer; return item_clock; }
static unsigned char ABI item_mock(unsigned char *self, unsigned char *racer, int category) {
    (void)self; (void)racer; (void)category; return item_native_result;
}
static void test_all_items(void) {
    unsigned char game[0x150]={0}, race[0xe00]={0}, diff[32]={0};
    unsigned char self[0x50]={0}, racer[0x180]={0}, entity[0x40]={0}, timer[32]={0};
    unsigned char controller[0x50]={0}, controller_handle[0x30]={0}, item_handle[0x30]={0};
    void *g=game, *r=race, *d=diff, *vtable[24]={0};
    client=&g; manager=&r; difficulty_manager=&d; native_difficulty=difficulty;
    native_item_decision=item_mock; followed_ai_profile=-1; peer_decision=0;
    put_ptr(self,0x18,entity); put_ptr(self,0x28,controller_handle);
    put_ptr(controller_handle,0x28,controller); put_ptr(controller,0x30,item_handle);
    put_ptr(item_handle,0x28,item_handle); put_ptr(self,0x38,timer);
    vtable[0xb0/8]=item_now; put_ptr(timer,0,vtable);
    entity[0x3b]=1; put_int(race,0xd4c,1);
    const float waits[]={6,4,1.5f,.9f,.6f,.3f};
    for (int game_mode=8; game_mode<=16; game_mode+=8)
        for (int level=0; level<6; ++level)
            for (int category=0; category<=4; ++category) {
                put_int(game,0x144,game_mode); put_int(diff,12,level<2?level:2);
                ai_profile=level<3?0:level-2; item_native_result=0;
                memset(item_holds,0,sizeof item_holds); item_clock=1000;
                assert(!item_decision(self,racer,category));
                item_clock += (uint32_t)ceilf(waits[level]*8192)-1;
                assert(!item_decision(self,racer,category));
                ++item_clock; assert(item_decision(self,racer,category));
                assert(!item_decision(self,racer,category)); /* No repeat press spam. */
            }
    /* Native tactical opportunities fire after reaction time, before the cap. */
    const float reactions[]={.6f,.45f,.3f,.2f,.15f,.1f};
    for (int level=0;level<6;++level) {
        put_int(diff,12,level<2?level:2); ai_profile=level<3?0:level-2;
        memset(item_holds,0,sizeof item_holds); item_native_result=1; item_clock=1000;
        assert(!item_decision(self,racer,0));
        item_clock += (uint32_t)ceilf(reactions[level]*8192)-1;
        assert(!item_decision(self,racer,0));
        ++item_clock; assert(item_decision(self,racer,0));
    }
    item_native_result=0;
    item_clock += 8192*20;
    itemless=1; assert(!item_decision(self,racer,0)); itemless=0;
    entity[0x3b]=0; assert(!item_decision(self,racer,0)); entity[0x3b]=1;
    put_int(race,0xd4c,9); assert(!item_decision(self,racer,0));
    put_int(race,0xd4c,1); racer[0x8c]=1; assert(!item_decision(self,racer,0));
    racer[0x8c]=0; item_native_result=1; itemless=1;
    assert(item_decision(self,racer,0)); /* Excluded modes retain native result. */
    itemless=0;
    unsigned char ai[0x80]={0}, control[0x80]={0}, wire[0x18]={0};
    put_ptr(ai,0x48,control); put_ptr(ai,0x58,self); put_ptr(ai,0x70,wire);
    native_apply_ai_input=apply_inputs_mock;
    self[0x30]=1; control[0x48]=1; pending_item_use=self;
    previous_ai_actions=8; wire[0x10]=0xc9; applied_tt_inputs=0;
    apply_item_during_slide(ai,NULL);
    assert(wire[0x10]==0xe9 && applied_tt_inputs==1); /* Drift and profile preserved. */
    apply_item_during_slide(ai,NULL);
    assert(applied_tt_inputs==1); /* No duplicate execution. */
    wire[0x10]=0xc9; previous_ai_actions=32;
    apply_item_during_slide(ai,NULL); assert(wire[0x10]==0xc9);
    previous_ai_actions=8; itemless=1;
    apply_item_during_slide(ai,NULL); assert(wire[0x10]==0xc9);
    itemless=0; private_room=0;
    for (int category=0;category<=4;++category) for (int result=0;result<=1;++result) {
        item_native_result=(unsigned char)result;
        assert(item_decision(self,racer,category)==result);
    }
    pending_item_use=self;
    apply_item_during_slide(ai,NULL); assert(wire[0x10]==0xc9 && applied_tt_inputs==1);
    private_room=1; pending_item_use=NULL;
    apply_item_during_slide(ai,NULL); assert(wire[0x10]==0xc9); /* Stale decisions excluded. */
}

int main(void) {
    put_int(default_race, 0xd40, 1); put_int(default_race, 0xd4c, 1);
    unsigned char race[0xd50] = {0}, config[0x30] = {0}, self[0x1d0] = {0};
    unsigned char labels_list[40] = {0}, flags_list[40] = {0}, event[32] = {0};
    void *race_ptr = race, *config_ptr = config, *pool_ptr = NULL, *type_ptr = (void *)1;
    void *vt[] = {kind};
    void *labels[4] = {NULL, NULL, NULL, NULL};
    unsigned char enabled[] = {1, 1, 1, 1};
    char *three = calloc(1, 18); assert(three); labels[2] = three + 16;
    labels[3] = (void *)"4";
    manager = &race_ptr; settings = &config_ptr; pool = &pool_ptr; changed_type = &type_ptr;
    drop_string = drop; team_event_original = original;
    put_int(race, 0xd40, 1);
    put_ptr(self, 0x1a8, labels_list); put_ptr(self, 0x1b0, flags_list);
    put_ptr(labels_list, 0x20, labels); put_ptr(flags_list, 0x20, enabled);
    put_int(labels_list, 12, 4); put_int(flags_list, 12, 4);
    put_ptr(event, 0, vt); put_int(event, 0x18, 3);
    team_event(self, event, NULL);
    assert(drops == 1 && integer(labels_list, 12) == 3 && integer(flags_list, 12) == 3);
    assert(labels[2] == (void *)"4" && labels[3] == NULL && enabled[2]);
    assert(integer(self, 0x190) == 2 && integer(config, 0x28) == 4);
    put_int(event, 0x18, 2);
    team_event(self, event, NULL);
    assert(integer(config, 0x28) == 4 && integer(event, 0x18) == 2 && drops == 1);
    put_int(event, 0x18, 1);
    team_event(self, event, NULL);
    assert(integer(config, 0x28) == 2 && integer(self, 0x190) == 1);
    put_int(race, 0xd40, 9); put_int(labels_list, 12, 4); put_int(flags_list, 12, 4);
    put_int(event, 0x18, 2);
    team_event(self, event, NULL);
    assert(integer(config, 0x28) == 3 && integer(labels_list, 12) == 4 && drops == 1);
    private_room = 0; put_int(race, 0xd40, 1);
    team_event(self, event, NULL);
    assert(integer(config, 0x28) == 3 && integer(labels_list, 12) == 4 && drops == 1);
    assert(calls == 5);
    manager = &default_race_ptr;
    test_ai();
    manager = &default_race_ptr;
    test_dynamic_speed();
    manager = &default_race_ptr;
    test_ai_physics();
    manager = &default_race_ptr;
    test_reserves();
    manager = &default_race_ptr;
    test_lines();
    manager = &default_race_ptr;
    test_shortcuts();
    manager = &default_race_ptr;
    test_route_variation();
    manager = &default_race_ptr;
    test_drift_frequency();
    manager = &default_race_ptr;
    test_authority_profile();
    manager = &default_race_ptr;
    test_difficulty_mode_scope();
    manager = &default_race_ptr;
    test_private_difficulty_gate();
    manager = &default_race_ptr;
    test_room_difficulty_restore();
    manager = &default_race_ptr;
    test_stuck_recovery();
    manager = &default_race_ptr;
    test_ghost_traffic();
    manager = &default_race_ptr;
    test_boxes();
    manager = &default_race_ptr;
    test_targeting();
    manager = &default_race_ptr;
    test_tt_technique();
    manager = &default_race_ptr;
    test_all_items();
}
