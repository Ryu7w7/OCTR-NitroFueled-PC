/* cc -std=c11 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
 * -Isrc tests/test_online_race_transition.c -Wl,--gc-sections -lm -o out/online-race-transition-test */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_online_mode.c"

static int public_match;
int runtime_public_match_active(void) { return public_match; }
static int ranked_match;
int runtime_ranked_active(void) { return ranked_match; }
static int found_room;
int runtime_public_found_room(void) { return found_room; }
int runtime_speed_race_active(void) { return 0; }
static const char *test_level;
const char *rr_current_level(void) { return test_level; }
static unsigned char test_state[0x148];
static void *ABI race_state(void) { return test_state; }
static int ABI race_state_kind(int state) { return state == 16; }

static int choice_calls, choice_clears;
static void ABI clear_choices(unsigned char *names) { *(int *)(names+12)=0; ++choice_clears; }
static void ABI append_choices(void *lobby, void *event) {
    assert(event==(void *)1);
    unsigned char *names=pointer_at(lobby,0x48), *modes=pointer_at(lobby,0x50);
    assert(!*(int *)(names+12) && !*(int *)(modes+12));
    *(int *)(names+12)+=3;*(int *)(modes+12)+=3;++choice_calls;
}
static void test_map_choices_replace_previous_room(void) {
    unsigned char lobby[0x58]={0},names[0x28]={0},modes[0x28]={0};
    *(void **)(lobby+0x48)=names;*(void **)(lobby+0x50)=modes;
    original_map_choices=append_choices;clear_map_choice_names=clear_choices;
    found_room=1;
    for(int i=0;i<3;++i) {
        receive_map_choices(lobby,(void *)1);
        assert(*(int *)(names+12)==3 && *(int *)(modes+12)==3);
    }
    assert(choice_calls==3 && choice_clears==3);
    found_room=0;
    receive_map_choices(lobby,(void *)1);
    assert(choice_calls==3 && choice_clears==3);
}

static void test_menu_is_not_race(void) {
    network_state= race_state;network_race=race_state_kind;
    *(int *)(test_state+0x144)=16;
    test_level="octane\\Octane_StartScreen\\Octane_StartScreen";
    assert(!runtime_online_mode_race_running());
    test_level="octane/octane_onlinelobby/octane_onlinelobby";
    assert(!runtime_online_mode_race_running());
    test_level="octane/t232_clockwork_wumpa/t232_clockwork_wumpa";
    assert(runtime_online_mode_race_running());
    *(int *)(test_state+0x144)=8;
    assert(!runtime_online_mode_race_running());
    network_state=NULL;network_race=NULL;test_level=NULL;
}

static void test_partial_finish_times(void) {
    unsigned char manager[0x40]={0},list[0x28]={0},settings[0x30]={0},event[0x40]={0};
    unsigned char racers[2][0x170]={{0}},players[2][0xd0]={{0}},handles[2][0x30]={{0}},laps[2][0x28]={{0}};
    void *items[]={racers[0],racers[1]},*manager_ptr=manager;
    unsigned char *settings_ptr=settings;
    int times[2][3]={{100,200,0},{110,210,310}};
    *(void **)(manager+0x10)=list;*(int *)(list+12)=2;*(void ***)(list+0x20)=items;
    *(int *)(settings+12)=3;racer_manager=&manager_ptr;race_settings=&settings_ptr;
    for(int i=0;i<2;++i) {
        *(void **)(racers[i]+0x10)=handles[i];*(void **)(handles[i]+0x28)=players[i];
        *(int *)(players[i]+0xc8)=i+10;*(uint64_t *)(players[i]+0xa8)=i+100;
        *(void **)(racers[i]+0x90)=laps[i];*(int *)(laps[i]+12)=3;
        *(void **)(laps[i]+0x20)=times[i];*(int *)(event+0x20+i*4)=i+10;
    }
    for(int mode=0;mode<2;++mode) {
        public_match=1;ranked_match=mode;
        /* Missing winner timestamp must not promote second place on this client. */
        assert(capture_finish_order(event));
        assert(finish_order[0].id==10 && finish_order[1].id==11);
        racers[1][0x8c]=1;
        *(int *)(racers[1]+0x6c)=0;
        synchronize_finish_time(racers[1]);
        assert(*(int *)(racers[1]+0x6c)==310);
        racers[0][0x8c]=1;
        synchronize_finish_time(racers[0]);
        assert(*(int *)(racers[0]+0x6c)==0); /* An incomplete lap record stays unknown. */
    }
    times[0][2]=300;
    synchronize_finish_time(racers[0]);
    assert(*(int *)(racers[0]+0x6c)==300); /* Late remote lap repairs the displayed time. */
    times[0][2]=400;
    assert(capture_finish_order(event));
    assert(finish_order[0].id==11); /* Complete timestamps still correct late finish messages. */
    private_cup_race_active=1;
    assert(capture_finish_order(event));
    assert(finish_order[0].id==10); /* CUP awards retain the host permutation. */
    private_cup_race_active=0;
    racers[1][0x8c]=0;*(int *)(racers[1]+0x6c)=0;
    synchronize_finish_time(racers[1]);
    assert(*(int *)(racers[1]+0x6c)==0 && !racers[1][0x8c]);
    racer_manager=NULL;race_settings=NULL;finish_order_count=0;public_match=ranked_match=0;
}

static void test_ranked_intro(void) {
    unsigned char manager[0xa8] = {0}, replacement[0xa8] = {0};
    unsigned char *current = manager;
    cutscene_manager = &current;
    ranked_intro_active = 1;
    ranked_match = 0;
    ranked_intro_skip();
    assert(manager[0x7a] == 0);
    ranked_match = 1;
    ranked_intro_skip();
    assert(manager[0x7a] == 1);
    ranked_intro_skip();
    ranked_intro_active = 0;
    ranked_intro_skip();
    assert(manager[0x7a] == 0 && !ranked_cutscene_owner);
    /* Preserve an existing skip request; restore on cancellation too. */
    manager[0x7a] = 1;ranked_intro_active = 1;
    ranked_intro_skip();
    ranked_match = 0;ranked_intro_skip();
    assert(manager[0x7a] == 1 && !ranked_cutscene_owner);
    ranked_match = 1;ranked_intro_skip();
    current = replacement;ranked_intro_skip();
    assert(replacement[0x7a] == 1);
    ranked_intro_active = 0;ranked_intro_skip();
    assert(replacement[0x7a] == 0);
    current = NULL;ranked_intro_active = 1;ranked_intro_skip();
    assert(!ranked_cutscene_owner);
    cutscene_manager = NULL;ranked_intro_active = ranked_match = 0;
}

static void test_departed_race_identity(void) {
    unsigned char manager[0x40]={0},list[0x28]={0},settings[0x30]={0},event[0x40]={0};
    unsigned char racers[8][0x170]={{0}},players[8][0xd0]={{0}},handles[8][0x30]={{0}},laps[8][0x28]={{0}};
    void *items[8],*manager_ptr=manager;
    unsigned char *settings_ptr=settings;
    char names[8][32];int times[8][3];
    *(void **)(manager+0x10)=list;*(void ***)(list+0x20)=items;
    *(int *)(settings+12)=3;racer_manager=&manager_ptr;race_settings=&settings_ptr;
    public_match=ranked_match=1;
    for (int count=2;count<=8;++count) for (int departed=0;departed<count;++departed) {
        race_identity_count=ranked_report_count=finish_order_count=0;
        *(int *)(list+12)=count;
        for (int i=0;i<count;++i) {
            memset(racers[i],0,sizeof racers[i]);memset(players[i],0,sizeof players[i]);
            items[i]=racers[i];snprintf(names[i],sizeof names[i],"Player%d",i);
            *(void **)(racers[i]+0x10)=handles[i];*(void **)(handles[i]+0x28)=players[i];
            *(void **)(players[i]+0x68)=names[i];
            *(int *)(players[i]+0xc8)=i;*(uint64_t *)(players[i]+0xa8)=100+i;
            *(void **)(racers[i]+0x90)=laps[i];*(int *)(laps[i]+12)=3;
            *(void **)(laps[i]+0x20)=times[i];
            times[i][0]=100+i;times[i][1]=200+i;times[i][2]=300+i;
            *(int *)(event+0x20+i*4)=i;
            racers[i][0xb0]=i==(departed+1)%count;
        }
        /* Native placement initially exposes only the local racer; remote racers arrive later. */
        *(int *)(list+12)=1;snapshot_race_identity();assert(race_identity_count==1);
        for (int arrived=2;arrived<=count;++arrived) {
            *(int *)(list+12)=arrived;snapshot_race_identity();
            assert(race_identity_count==arrived);
        }
        snapshot_race_identity();assert(race_identity_count==count);
        for(int i=0;i<count;++i) racers[i][0xb0]=1; /* Rehost grants simulation ownership, not local identity. */
        /* Any slot may be a departing host or guest. Its AI later finishes, but the human did not. */
        times[departed][2]=0;
        runtime_online_mode_racer_left(100+departed);
        unsigned char entity[0x40]={0}, entity_handle[0x30]={0};
        *(void **)(entity_handle+0x28)=entity;*(void **)(racers[departed]+0x20)=entity_handle;
        assert(runtime_online_mode_departed_kart(entity));
        assert(!runtime_online_mode_departed_kart(players[departed]));
        public_match=0;assert(!runtime_online_mode_departed_kart(entity));public_match=1;
        *(void **)(racers[departed]+0x10)=NULL;
        times[departed][2]=600;racers[departed][0x8c]=1;
        runtime_online_mode_racer_left(100+departed);
        assert(capture_finish_order(event));
        assert(ranked_report_count==count && finish_order[count-1].online_id==(uint64_t)(100+departed));
        int selves=0;
        for (int i=0;i<count;++i) {
            struct runtime_ranked_racer *r=&ranked_report[i];selves+=r->self;
            if (!strcmp(r->name,names[departed])) {
                assert(r->dc && !r->finished && !r->time && r->pos==count);
            } else assert(!r->dc && r->finished && r->time>0);
        }
        assert(selves==1);
        /* Multiple departures keep every original participant and completed human lap evidence. */
        for (int i=0;i<count;++i) if (i!=departed && !race_identity[i].report.self) {
            runtime_online_mode_racer_left(100+i);
            *(void **)(racers[i]+0x10)=NULL;
        }
        assert(capture_finish_order(event) && ranked_report_count==count);
        for(int i=0;i<count;++i) if (ranked_report[i].dc && ranked_report[i].finished)
            assert(ranked_report[i].time>=300 && ranked_report[i].time<308);
        /* A malformed host permutation still cannot masquerade as a validated result. */
        *(int *)(event+0x24)=*(int *)(event+0x20);
        assert(!capture_finish_order(event));
    }
    race_identity_count=0;
    assert(!saved_race_identity(racers[0]));
    public_match=ranked_match=0;ranked_report_count=finish_order_count=0;
    racer_manager=NULL;race_settings=NULL;
}

static void test_native_ai_survives_rehost(void) {
    unsigned char manager[0x20]={0},list[0x28]={0};
    unsigned char racers[3][0x170]={{0}},players[3][0xd0]={{0}};
    unsigned char player_handles[3][0x30]={{0}},entity_handles[3][0x30]={{0}},entities[3][0x40]={{0}};
    void *items[]={racers[0],racers[1],racers[2]},*manager_ptr=manager;
    racer_manager=&manager_ptr;public_match=1;race_identity_count=0;
    *(void **)(manager+0x10)=list;*(void ***)(list+0x20)=items;*(int *)(list+12)=3;
    for(int i=0;i<3;++i) {
        *(void **)(racers[i]+0x20)=entity_handles[i];*(void **)(entity_handles[i]+0x28)=entities[i];
        if(i==1) continue; /* Matchmaking AI has no human player handle. */
        *(void **)(racers[i]+0x10)=player_handles[i];*(void **)(player_handles[i]+0x28)=players[i];
        *(void **)(players[i]+0x68)=i==0 ? "Departed" : "Survivor";
        *(uint64_t *)(players[i]+0xa8)=100+i;*(int *)(players[i]+0xc8)=i;
    }
    snapshot_race_identity();assert(race_identity_count==2);
    runtime_online_mode_racer_left(100);
    *(void **)(racers[0]+0x10)=NULL; /* Former human now driven by AI. */
    for(int rehost=0;rehost<3;++rehost) {
        snapshot_race_identity();runtime_online_mode_racer_left(100);
        assert(runtime_online_mode_departed_kart(entities[0]));
        assert(!runtime_online_mode_departed_kart(entities[1])); /* Even with the same AI slot 0. */
        assert(!runtime_online_mode_departed_kart(entities[2]));
    }
    race_identity_count=0;
    assert(!runtime_online_mode_departed_kart(entities[0])); /* Next race clears the departure. */
    racer_manager=NULL;public_match=0;
}

int main(void) {
    test_map_choices_replace_previous_room();
    test_menu_is_not_race();
    test_ranked_intro();
    test_partial_finish_times();
    test_departed_race_identity();
    test_native_ai_survives_rehost();
    unsigned char racers[3][0x170] = {{0}};
    for (int i = 0; i < 3; ++i) {
        const int id = 10 + i;
        memcpy(racers[i] + 0x164, &id, sizeof id);
    }
    for (int i = 0; i < 3; ++i) finish_order[i] = racer_identity(racers[2 - i]);
    finish_order_count = 3;
    public_match = 1;
    /* The promoted host's native path can supply an uninitialized rank. */
    assert(podium_position(racers[0], 8) == 3);
    assert(podium_position(racers[1], 8) == 2);
    assert(podium_position(racers[2], 8) == 1);
    unsigned char unknown[0x170] = {0};
    assert(podium_position(unknown, 8) == 8);
    /* CUP's aggregate order keeps precedence over individual race results. */
    podium_order[0] = racers[0]; podium_count = 1;
    assert(podium_position(racers[0], 8) == 1);
    podium_count = 0;
    finish_order_count = 0;
    assert(podium_position(racers[0], 8) == 8);
    finish_order_count = 3; public_match = 0;
    assert(podium_position(racers[0], 8) == 8);
    puts("online race transitions: OK");
}
