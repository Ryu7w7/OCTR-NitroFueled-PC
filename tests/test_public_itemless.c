#define SACREDFIRE_RELEASE "v9.9.9"
#define runtime_party_server_matchmaking_leave test_matchmaking_leave
#include "../src/runtime_public_itemless.c"
static int rejoin_player_calls;
static void ABI rejoin_players_mock(void *manager) {
    ++rejoin_player_calls;
    unsigned char *list=ptr(manager,0x10);
    int count=*(int *)(list+12),slot=0;
    void **rows=ptr(list,0x20);
    for (int i=0;i<count;++i) {
        unsigned char *player=rows[i];
        if (*(unsigned *)(player+0x20)>7) continue;
        *(unsigned *)(player+0xb4)=slot++;
        *(unsigned *)(player+0xc8)=*(unsigned *)(player+0xb4);
    }
}
void runtime_online_mode_racer_left(uint64_t pid) { (void)pid; }
#undef runtime_party_server_matchmaking_leave
#include "../src/runtime_text.c"
#include <assert.h>
#include <unistd.h>
static int sent, dialogs, status = 200, stopped, returned, early, forwarded, requests, private_room, ranked_notices;
static int matchmaking_leaves, matchmaking_keep;
void test_matchmaking_leave(int keep) { ++matchmaking_leaves; matchmaking_keep=keep; }
static int private_speed, private_cup, private_teams, modified_difficulty, selector_left_calls;
static int party_guest=1;
static int hidden_quit;
void runtime_online_mode_show_placeable(void *placeable, int shown) { (void)placeable; assert(!shown); ++hidden_quit; }
int runtime_room_mode_following(void) { return party_guest; }
static int frozen_race;
int runtime_room_mode_race_frozen(void) { return frozen_race; }
int runtime_online_mode_race_running(void) { return frozen_race; }
static int kart_racing, departures;
static uint64_t ranked_finished_room;
int runtime_ranked_result_copy(struct runtime_ranked_result *out) {
    memset(out,0,sizeof *out);out->room=ranked_finished_room;
    return ranked_finished_room!=0;
}
static int peer_leave_calls;
static uint64_t ABI peer_leave_mock(void *message) { (void)message; ++peer_leave_calls; return 7; }
static int race_switch_calls;
static void ABI race_switch_mock(void *race,void *connection) { (void)race; (void)connection; ++race_switch_calls; }
void bbgpu_notify(const char *title,const char *name,const char *detail,int kind,const void *image,uint64_t size) {
    (void)title; (void)detail; (void)kind; (void)image; (void)size;
    const char *expected[]={"Test-Racer left","Player 125 left","Guest left","OldHost left"};
    assert(departures < 4 && !strcmp(name, expected[departures]));
    departures++;
}
int runtime_replay_race_active(void) { return kart_racing; }
static int host_lost_return;
void runtime_party_host_lost(int role,int private_match,int announced_return) {
    (void)role;(void)private_match;host_lost_return=announced_return;
}
static void ABI reset_mock(void *party) { (void)party; }
static void *focus_test_button;
static unsigned synthetic_signal;
static int hof_opens;
void runtime_hof_open(void) { ++hof_opens; }
void runtime_hof_close(void) {}
static int hof_input_handled, hof_input_signal;
int runtime_hof_input(void *placeable, unsigned signal) { (void)placeable; hof_input_signal = signal; return hof_input_handled; }
static int ranked_status_state = 1;
int runtime_ranked_status(int fresh) { (void)fresh; return ranked_status_state; }
static int ranked_account_state = 1;
int runtime_ranked_account(void) { return ranked_account_state; }
static const char *online_content_state = "clean";
const char *runtime_online_content(void) { return online_content_state; }
int runtime_private_difficulty_modified(void) { return modified_difficulty; }
int runtime_online_mode_private_cup(void) { return private_cup; }
int runtime_online_mode_private_teams(void) { return private_teams; }
int runtime_speed_mode_enabled(void) { return private_speed; }
int runtime_online_mode_in_private(void) { return private_room; }
static int lobby_open = 1;
int runtime_online_mode_lobby_open(void) { return lobby_open; }
/* The game's own room list (runtime_online_mode.c); 0 = no list, the server's count decides. */
static int room_players;
int runtime_online_mode_room_players(void) { return room_players; }
static void *hof_test_wrapper, *hof_test_content;
void *runtime_online_mode_content(void *placeable) { return placeable == hof_test_wrapper ? hof_test_content : NULL; }
static void *hof_focused_project, *hof_focused_list;
static void ABI hof_focus_mock(void *project, void *list, int controller) {
    assert(controller == 1); hof_focused_project = project; hof_focused_list = list;
}
static int hof_legends;
void runtime_online_mode_hof_legend(void *project) { assert(project); ++hof_legends; }
void runtime_online_mode_selector_left(void) { ++selector_left_calls; }
static int timeout_attempts, timeout_ready=1, timeout_route;
int runtime_online_mode_matchmaking_cancel(int leave_room) { timeout_route=leave_room;++timeout_attempts;return timeout_ready; }
int runtime_online_mode_matchmaking_timeout(void) { return runtime_online_mode_matchmaking_cancel(0); }
static int return_mode;
void runtime_online_mode_set_return_mode(int index) { return_mode = index; }
static int new_sessions, hosted_sessions;
void runtime_online_mode_new_session(void) { ++new_sessions; runtime_public_itemless_new_session(); }
void runtime_room_mode_hosting(void) { ++hosted_sessions; }
static int ranked_labels, hall_labels;
void runtime_online_mode_set_text(void *placeable, const char *text) {
    assert(placeable);
    if (!strcmp(text, "RANKED")) ++ranked_labels;
    else { assert(!strcmp(text, "HALL OF FAME")); ++hall_labels; }
}
static void *test_network=(void *)1;
static void ABI stop_matchmaking(void *self) { assert(self==test_network); ++stopped; }
static void ABI return_to_lobby(void) { assert(stopped==returned+1); ++returned; }
static pthread_mutex_t http_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t http_condition = PTHREAD_COND_INITIALIZER;
static int block_get, entered_get;
static char expected_ids[192];
static char last_room_poll[128];
static const char *room_poll_reply="{\"players\":\"6\"}";
static char payload[4096], message[512], reply[512] = "ok";
const char *runtime_np_get_online_name(void) { return "LocalHost"; }
uint64_t runtime_ryunet_last_sent_ms(void) { return 0; } /* runtime_http.c: no send time here */
int runtime_ryunet_request(const char *method, const char *target, char *body, size_t size) {
    ++requests;
    if (!strcmp(target,"/ryunet/theme")) {
        snprintf(body,size,"%s",reply); return status;
    }
    if (strstr(target,"/ryunet/matchmaking-status?")) { snprintf(body,size,"%s",reply); return status; }
    /* Found-room / party-leave notifications (issue 0042): the client tells RyuNET the instant
     * a player exits a found room, and polls whether a found room is still alive. */
    if (!strcmp(method,"POST") && strstr(target,"/ryunet/room-left")) { snprintf(body,size,"ok"); return 200; }
    if (!strcmp(method,"POST") && strstr(target,"/ryunet/room-short")) { snprintf(body,size,"ok"); return 200; }
    if (!strcmp(method,"GET") && strstr(target,"/ryunet/room-alive")) { snprintf(last_room_poll,sizeof last_room_poll,"%s",target); snprintf(body,size,"%s",room_poll_reply); return 200; }
    if (!strcmp(method,"POST")) {
        assert(strstr(target,"/ryunet/itemless-client?name=LocalHost&version=1&nonce="));
        assert(strstr(target,"&release=" SACREDFIRE_RELEASE));
        assert(strstr(target,"&ranked=2"));
        assert(strstr(target,ranked_device()) && strlen(ranked_device())==64);
        assert(strstr(target,"&collision_rules=1"));
        snprintf(body,size,"ok"); return 200;
    }
    assert(!strcmp(method,"GET"));
    if (early) assert(!strcmp(strstr(target,"&members=")+9, expected_ids));
    pthread_mutex_lock(&http_lock);
    entered_get=1;
    pthread_cond_broadcast(&http_condition);
    while (block_get) pthread_cond_wait(&http_condition,&http_lock);
    pthread_mutex_unlock(&http_lock);
    assert(strstr(target,"/ryunet/itemless-party?name=LocalHost&members=") ||
           strstr(target,"/ryunet/ranked-party?name=LocalHost&members=") ||
           strstr(target,"/ryunet/private-party?name=LocalHost&feature="));
    snprintf(body,size,"%s",strstr(target,"/ryunet/private-party?") && !strcmp(reply,"ok") ? "ok-private-release:" SACREDFIRE_RELEASE : reply); return status;
}
static void ABI show(const char *body, void *options) {
    if (!early && !strstr(body, "Checking Ranked compatibility"))
        assert(stopped==returned && returned==dialogs+1);
    assert(options==NULL); /* Exactly the offline Online-entry notice configuration. */
    snprintf(message,sizeof message,"%s",body); ++dialogs;
}
static void ABI count_notice(const char *body, void *options) { assert(body && !options); ++ranked_notices; }

static void *ABI send_search(void **out, void *self, const char *doc, unsigned n, void *result) {
    (void)self;(void)result; assert(strlen(doc)<n);
    snprintf(payload,sizeof payload,"%s",doc); ++sent; *out=(void *)1; return out;
}
static void *ABI send_info(void **out, void *self, const char *doc, unsigned n) {
    return send_search(out,self,doc,n,NULL);
}
static int ended;
static void ABI end_event(void *self, uint64_t room) { (void)self; (void)room; ++ended; }
static void ABI destroy_owner(void *self) {
    assert(!runtime_public_itemless_active());
    memset(self, 0, 0x960);
    ++ended;
}
static void *ABI send_cancel(void **out, void *self, uint64_t id, void *response) {
    (void)self; (void)id; (void)response; *out = (void *)1; return out;
}
static void *ABI send_leave(void **out, void *self, uint64_t id) {
    return send_cancel(out, self, id, NULL);
}
static int leave_saw_active;
static void *ABI send_leave_observed(void **out, void *self, uint64_t id) {
    leave_saw_active = runtime_public_itemless_active();
    return send_leave(out, self, id);
}
static void ABI merge_event(void *self, uint64_t search, uint64_t from, uint64_t to,
                            const char *host, unsigned hn, const char *doc, unsigned dn) {
    (void)search; (void)from; (void)host; (void)hn; (void)doc; (void)dn;
    memcpy((unsigned char *)self + 0x958, &to, 8);
}
static void confirmation_doc(char *out, size_t n, uint64_t room, uint64_t token) {
    snprintf(out,n,"{\"lobby_id\":%" PRIu64 ",\"octr_itemless_v1\":\"2/%" PRIu64 "/1/%016" PRIx64 "\"}",room,room,token);
}
static void test_full_lobby_confirmation(void) {
    /* Eight solo searches contribute eight request and eight registration tokens. */
    char doc[512];
    size_t n = (size_t)snprintf(doc, sizeof doc, "{\"octr_itemless_v1\":\"2/42/1/");
    for (uint64_t i=1; i<=16; ++i)
        n += (size_t)snprintf(doc+n, sizeof doc-n, "%s%016" PRIx64, i==1 ? "" : ",", i);
    snprintf(doc+n, sizeof doc-n, "\"}");
    for (uint64_t i=1; i<=16; ++i) assert(confirmation(doc,42,i));
    assert(!confirmation(doc,42,17));
    assert(!confirmation(doc,43,16));
    /* Do not silently accept a token list exceeding the eight-player protocol bound. */
    snprintf(doc+n, sizeof doc-n, ",%016" PRIx64 "\"}", (uint64_t)17);
    assert(!confirmation(doc,42,1));
}
static int native_party_teardowns, native_keep_party, native_race_resets, native_lobby_closes, native_session_releases, native_session_stops;
static void ABI native_close_lobby(int start_again) { assert(!start_again && native_keep_party==1 && native_race_resets);++native_lobby_closes; }
static void ABI native_reset_session(void *self) { (void)self;assert(native_keep_party==1);++native_race_resets; }
static void ABI native_stop_part(void *self) { (void)self; }
static void ABI native_stop_party(void *self) { (void)self; ++native_party_teardowns; }
static void ABI native_stop_session(void *self, int keep_party) { (void)self; native_keep_party=keep_party; }
static void ABI native_release_session(void *self) { assert(self); ++native_session_releases; }
static void ABI native_stop_session_object(void *self) { assert(self); ++native_session_stops; }
static uint32_t ABI session_init_mock(void *session,void *second,void *third,int host) {
    assert(ptr(session,0x10)==second && third==session && host==1);
    memcpy((char *)second+0xb0,&native_session_admission,sizeof native_session_admission);
    return 0x12340001u;
}

static void test_installed_hooks(const char *region) {
    size_t size=0x2af3000;
    unsigned char *im=mmap(NULL,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(im!=MAP_FAILED);
    const size_t sites[]={0xda300,0xd8fb0,0xda4f0,0xda7f0};
    const size_t slots[]={0x2549ea8,0x2549ee0,0x2549ee8,0x2549ec0,0x2549eb0,0x2549eb8};
    const size_t targets[]={0xa14890,0xa14910,0xa14e20,0xa148c0,0xa148a0,0xa148b0};
    const unsigned char prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec};
    for (int i=0;i<4;++i) {
        unsigned char *at=im+sites[i];
        memcpy(at,prefix,16); at[16]=i==0?0x68:0x58;
        memcpy(at+17,"\x48\x83\xc4",3);at[20]=at[16];
        memcpy(at+21,"\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d",10);
        jump(at+31,i==1?(void *)send_info:(void *)send_search);
    }
    for (int i=0;i<6;++i) { void *target=im+targets[i];memcpy(im+slots[i],&target,8); }
    memcpy(im+0xa14550,"\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x48\x89\xfb",14);
    memcpy(im+0xa14550+14,"\x5b\x41\x5c\x41\x5e\x41\x5f\x5d",8);
    jump(im+0xa14550+22,(void *)destroy_owner);
    size_t stop_quit_site=0x104b8f9-(!strcmp(region,"1277df0") ? 0x10 : 0);
    int32_t stop_relative=(int32_t)(0xa9ffd0-stop_quit_site-5);
    im[stop_quit_site]=0xe8;memcpy(im+stop_quit_site+1,&stop_relative,4);
    memcpy(im+0xa5b680,"\x55\x48\x89\xe5\x41\x56\x53\x48\x83\xec\x30\x4c\x8b\x35\x06\xba\xda\x01",18);
    memcpy(im+0xa5b692,"\x48\x83\xc4\x30\x5b\x41\x5e\x5d\xb0\x01\xc3",11);
    memcpy(im+0xa18210,"\x55\x48\x89\xe5\x53\x50\x48\x89\xfb\x83\x7b\x54\x0a\x0f\x84\x93\x00\x00\x00",19);
    const unsigned char bind_prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x50,0x49,0x89,0xfd,0x49,0x89,0xf6,0x49,0x8b,0x45,0x20};
    const unsigned char leave_prefix[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18};
    memcpy(im+0xa44190,bind_prefix,sizeof bind_prefix);
    memcpy(im+0xa44d34,"\xe8\x57\xf4\xff\xff",5);
    memcpy(im+0xa17526,"\xe8\x85\xe8\xff\xff",5);
    memcpy(im+0xa170a8,"\x0f\x84\x86\x01\x00\x00",6);
    memcpy(im+0xa90b10,"\x48\x83\x3d\x98\xbc\x03\x02\x00\x74\x19\x55\x48\x89\xe5",14);
    memcpy(im+0xa54060,leave_prefix,sizeof leave_prefix);
    memcpy(im+0xc22160,"\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x48\x83\xec\x10",15);
    memcpy(im+0xbd0ad0,"\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x18",17);
    memcpy(im+0xbd0ae1,"\x48\x83\xc4\x18\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d",14);
    jump(im+0xbd0aef,(void *)rejoin_players_mock);
    const unsigned char reset_ids_code[]="\x48\x63\x47\x0c\x48\x85\xc0\x7e\x43\x48\x8b\x4f\x20\x48\x8b\x11\x83\x7a\x20\x07\x77\x0c\x8b\xb2\xb0\x00\x00\x00\x89\xb2\xc8\x00\x00\x00\x83\xf8\x02\x7c\x25\x48\xff\xc8\x48\x83\xc1\x08\x48\x8b\x11\x83\x7a\x20\x07\x77\x0c\x8b\xb2\xb0\x00\x00\x00\x89\xb2\xc8\x00\x00\x00\x48\x83\xc1\x08\x48\xff\xc8\x75\xe2\xc3";
    memcpy(im+0xc23120,reset_ids_code,sizeof reset_ids_code-1);
    memcpy(im+0xa53d70,"\x55\x48\x89\xe5\x53\x50\x48\x8b\x05\x5b\x76\x07\x02",13);
    memcpy(im+0xa53dad,"\xe9\x3e\xb5\xff\xff",5);
    /* Both reset branches unwind the stolen prologue; quit does the same. */
    memcpy(im+0xa18223,"\x48\x8b\x7b\x18\x48\x85\xff\x74\x1f\x48\x8b\x07\xff\x90\xb8\x00\x00\x00",18);
    memcpy(im+0xa182b6,"\x58\x5b\x5d\xc3",4);
    memcpy(im+0xa18235,"\x48\x8b\x7b\x18\x48\xc7\x43\x18\x00\x00\x00\x00\x48\x85\xff\x74\x05\xe8",18);
    int32_t release_relative=(int32_t)(0x149d260-0xa1824b);
    memcpy(im+0xa18247,&release_relative,4);
    memcpy(im+0xa1824b,"\x58\x5b\x5d\xc3",4);
    jump(im+0x149d260,(void *)native_release_session);
    memcpy(im+0xa9ffd0,prefix,16);im[0xa9ffe0]=0x28;
    memcpy(im+0xa9ffe1,"\x48\x83\xc4\x28\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d\xc3",15);
    memcpy(im+0xaa06a0,"\x55\x48\x89\xe5\x41\x57\x41\x56\x53\x48\x83\xec\x18",13);
    const unsigned char stop_native[]={0x55,0x48,0x89,0xe5,0x53,0x50,0x48,0x89,0xfb,0x48,0x8b,0x7b,0x58,0xe8,0xde,0xec,0x01,0x00,0x48,0x8b,0xbb,0x98,0x00,0x00,0x00,0xe8,0x52,0xba,0xf8,0xff,0x48,0x8b,0xbb,0x90,0x00,0x00,0x00,0x31,0xf6,0x48,0x83,0xc4,0x08,0x5b,0x5d,0xe9,0x9e,0xfd,0xf7,0xff};
    memcpy(im+0xa97a00,stop_native,sizeof stop_native);
    jump(im+0xab66f0,(void *)native_stop_part);
    jump(im+0xa23470,(void *)native_stop_party);
    jump(im+0xa177d0,(void *)native_stop_session);
    jump(im+0xa18380,(void *)native_reset_session);
    jump(im+0xa5d550,(void *)native_close_lobby);
    unsigned char manager[0xa0]={0};
    int before_teardown=native_party_teardowns;
    ((void (ABI *)(void *))(im+0xa97a00))(manager);
    assert(native_party_teardowns==before_teardown+1 && native_keep_party==0);
    memcpy(im+0xa9cbe0,"\x55\x48\x89\xe5\x53\x50",6);
    memcpy(im+0x24de10,prefix,16);im[0x24de20]=0x48;
    memcpy(im+0xa548e0,prefix,16);im[0xa548f0]=0x38;
    setenv("BB_ONLINE_MODE_HOOK",region,1);
    int trace=!strcmp(region,"1277e00");
    setenv("BB_HOST_EXPERIMENT",trace ? "1" : "0",1);
    const size_t init_slots[]={0x254edc0,0x255e6c8};
    SessionInit init=(SessionInit)(im+0xa52710);
    jump(im+0xa52710,(void *)session_init_mock);
    for (int i=0;i<2;++i) memcpy(im+init_slots[i],&init,sizeof init);
    assert(runtime_public_itemless_start(im,size));
    assert(native_party_reset_trace && native_race_switch_original && native_peer_leave_original);
    assert(native_remove_player_original && !memcmp(im+0xc22160,"\xff\x25",2));
    unsigned char id_players[3][0xcc]={{0}}, id_list[0x28]={0};
    void *id_rows[]={id_players[0],id_players[1],id_players[2]}, *id_items=id_rows;
    memcpy(id_list+0x20,&id_items,sizeof id_items);
    *(int *)(id_list+12)=3;
    for (int i=0;i<3;++i) {
        *(int *)(id_players[i]+0xb0)=i;
        *(int *)(id_players[i]+0xc8)=7-i;
    }
    *(unsigned *)(id_players[1]+0x20)=UINT32_MAX;
    void (ABI *reset_ids)(void *)=(void (ABI *)(void *))(im+0xc23120);
    int saved_lobby_open=lobby_open;
    left_room_pending=73;kart_racing=1;local_race_quitting=0;lobby_open=0;
    reset_ids(id_list);
    for (int i=0;i<3;++i) assert(*(int *)(id_players[i]+0xc8)==7-i);
    lobby_open=1;reset_ids(id_list);
    assert(*(int *)(id_players[0]+0xc8)==0 && *(int *)(id_players[2]+0xc8)==2);
    lobby_open=0;
    *(int *)(id_players[0]+0xc8)=7;*(int *)(id_players[2]+0xc8)=5;
    local_race_quitting=1;reset_ids(id_list);
    assert(*(int *)(id_players[0]+0xc8)==0 && *(int *)(id_players[1]+0xc8)==6 && *(int *)(id_players[2]+0xc8)==2);
    local_race_quitting=0;kart_racing=0;left_room_pending=0;
    *(int *)(id_players[0]+0xc8)=7;*(int *)(id_list+12)=1;reset_ids(id_list);
    assert(*(int *)(id_players[0]+0xc8)==0);
    kart_racing=1;*(int *)(id_players[0]+0xc8)=7;reset_ids(id_list);
    assert(*(int *)(id_players[0]+0xc8)==0); /* Offline races still reset. */
    kart_racing=0;
    *(int *)(id_list+12)=0;reset_ids(id_list);
    lobby_open=saved_lobby_open;
    unsigned char id_manager[0x20]={0}, id_all[8][0xcc]={{0}};
    void *id_all_rows[8], *id_all_items=id_all_rows;
    memcpy(id_manager+0x10,&(void *){id_list},8);
    memcpy(id_list+0x20,&id_all_items,8);
    void (ABI *rejoin)(void *)=(void (ABI *)(void *))(im+0xbd0ad0);
    left_room_pending=73;kart_racing=1;lobby_open=0;
    for (int count=2;count<=8;++count) {
        *(int *)(id_list+12)=count;
        for (int local=0;local<count;++local) {
            for (int i=0;i<count;++i) {
                id_all_rows[i]=id_all[i];
                *(uint64_t *)(id_all[i]+0xa8)=100+i;
                *(unsigned *)(id_all[i]+0xc8)=i;
                *(unsigned *)(id_all[i]+0x20)=i==local ? 0 : UINT32_MAX;
            }
            for (int departed=0;departed<count;++departed) if (departed!=local) {
                int calls=rejoin_player_calls;
                rejoin(id_manager);
                assert(rejoin_player_calls==calls+1);
                assert(*(unsigned *)(id_all[local]+0xb4)==0);
                for(int i=0;i<count;++i) assert(*(unsigned *)(id_all[i]+0xc8)==(unsigned)i);
            }
        }
        /* Multiple local controllers are reinitialized by the same native loop. */
        for(int i=0;i<count;++i) {
            *(unsigned *)(id_all[i]+0x20)=i;
            *(unsigned *)(id_all[i]+0xc8)=count-1-i;
        }
        rejoin(id_manager);
        for(int i=0;i<count;++i) assert(*(unsigned *)(id_all[i]+0xc8)==(unsigned)(count-1-i));
    }
    lobby_open=1;rejoin(id_manager);assert(*(unsigned *)(id_all[7]+0xc8)==7);
    lobby_open=0;local_race_quitting=1;*(unsigned *)(id_all[7]+0xc8)=4;
    rejoin(id_manager);assert(*(unsigned *)(id_all[7]+0xc8)==7);
    local_race_quitting=0;left_room_pending=0;*(unsigned *)(id_all[7]+0xc8)=4;
    rejoin(id_manager);assert(*(unsigned *)(id_all[7]+0xc8)==7);
    kart_racing=0;lobby_open=saved_lobby_open;
    assert(native_join_match_host && !memcmp(im+0xa170a8,"\x90\x90\x90\x90\x90\x90",6));
    assert(!memcmp(im+0xa90b10,"\xff\x25",2));
    unsigned char init_connection[0x120]={0};
    void *session[3]={NULL,NULL,init_connection};
    for (int i=0;i<2;++i) {
        SessionInit entry=(SessionInit)ptr(im,init_slots[i]);
        assert(entry==traced_session_init);
        assert(entry(session,session[2],session,1)==0x12340001u);
        assert(ptr(init_connection,0xb0)==(void *)admit_race_rejoin);
    }
    assert(im[stop_quit_site]==0xe8); /* Do not alter an unrelated native sync-stop wrapper. */
    int resets=native_race_resets,closes=native_lobby_closes;
    native_stop_matchmaking(manager);
    assert(native_party_teardowns==before_teardown+1 && native_keep_party==1 && native_race_resets==resets+1 && native_lobby_closes==closes+1);
    assert(!memcmp(im+0xa97a00,stop_native,sizeof stop_native));
    unsigned char trace_party[0x80]={0};
    ((void (ABI *)(void *))(im+0xa18210))(trace_party);
    unsigned char dead_session[0x120]={0};
    *(void **)(trace_party+0x18)=dead_session;
    int releases=native_session_releases;
    ((void (ABI *)(void *))(im+0xa18210))(trace_party);
    assert(!*(void **)(trace_party+0x18) && native_session_releases==releases+1);
    void *live_vtable[24]={0};
    jump(im+0x100000,(void *)native_stop_session_object);
    live_vtable[23]=im+0x100000;
    *(void **)dead_session=live_vtable;
    *(void **)(trace_party+0x18)=dead_session;
    int stops=native_session_stops;
    ((void (ABI *)(void *))(im+0xa18210))(trace_party);
    assert(!*(void **)(trace_party+0x18) && native_session_stops==stops+1 && native_session_releases==releases+2);
    *(int *)(trace_party+0x54)=10;
    ((void (ABI *)(void *))(im+0xa18210))(trace_party);
    ((void (ABI *)(void *))(im+0xa9ffd0))(trace_party);
    assert(((unsigned char (ABI *)(void *,void *,void *))(im+0xa5b680))(NULL,NULL,NULL)==1);
    assert(native_notice==(Notice)(im+0xa548e0));
    native_notice=show;
    runtime_public_itemless_select(0);
    void *result=NULL;
    const char doc[]="{\"members\":[1,2]}";
    ((Search)(im+sites[0]))(&result,NULL,doc,sizeof doc,NULL);
    assert(result && !strcmp(payload,doc));
    ((Register)(im+sites[1]))(&result,NULL,doc,sizeof doc);
    assert(registration_nonce && strstr(payload,"octr_itemless"));
    assert(strstr(payload,"\"collision_rules\":1"));
    unsigned char owner[0x960]={0}; uint64_t room=42; char doc2[128];
    memcpy(owner+0x958,&room,8);
    confirmation_doc(doc2,sizeof doc2,room,registration_nonce);
    assert(accept(owner,room,doc2,(unsigned)strlen(doc2)));
    assert(runtime_public_itemless_active());
    ((void (ABI *)(void *))(im+0xa14550))(owner);
    assert(!runtime_public_itemless_active() && room_owner==NULL);
    munmap(im,size);
    race_racers=race_object_root=NULL;
    native_take_racer_ownership=NULL;
}
static void await_check(void);
static void *party_return_expected_target;
static void ABI forward_event(void *self, void *event) {
    if (self==party_return_expected_target && self) assert(!menu_party_returned && matchmaking_search && search_mark);
    (void)self; (void)event; ++forwarded;
    if (self == focus_test_button && event && *(void **)((char *)event + 0x10))
        synthetic_signal = *(unsigned *)((char *)event + 0x18);
    if (dispatch_approved_party) {
        int before=requests;
        assert(runtime_private_party_check());
        assert(requests==before); /* Same dispatch/roster never repeats HTTP. */
        if (!modified_difficulty) {
            modified_difficulty = 1;
            assert(runtime_private_party_check()==-1);
            await_check();
            assert(runtime_private_party_check());
            assert(requests > before); /* Different feature must be checked again. */
            modified_difficulty = 0;
        }
    }
}
static void *ABI object_type(void *object) { return *(void **)((char *)object + 8); }
static void *ABI graph_type(void *object) { return *(void **)((char *)object + 0x20); }
static void await_check(void) {
    for (int i=0;i<2000;++i) {
        pthread_mutex_lock(&check_lock); int running=check_running; pthread_mutex_unlock(&check_lock);
        if (!running) return;
        struct timespec delay={0,1000000}; nanosleep(&delay,NULL);
    }
    assert(!"compatibility worker stalled");
}
static void expire_check(void) {
    await_check(); pthread_mutex_lock(&check_lock); check_done=0; pthread_mutex_unlock(&check_lock);
}
/* Handle stubs model an engine object disappearing while a request is pending. */
static int handles;
static void ABI make_test_handle(void **slot, void *object) {
    unsigned char *handle=calloc(1,0x30); assert(handle);
    *(unsigned *)handle=2;
    memcpy(handle+0x28,&object,sizeof object);
    *slot=handle; ++handles;
}
static void ABI destroy_test_handle(void **slot) { free(*slot); *slot=NULL; --handles; }
static void ABI resolve_test_handle(void **slot) { (void)slot; }
/* One press waits and resumes automatically; pending is never a modal error. */
static void settled_event(void *self,void *event) {
    expire_check();
    int d=dialogs,f=forwarded;
    placeable_event(self,event);
    if (pending_private.target) {
        assert(forwarded==f && dialogs==d);
        await_check();
        (void)runtime_private_party_update();
    }
    assert(!pending_private.target && handles==0);
}
static int resumed_starts;
static void ABI resume_test_start(void *self) {
    assert(self && runtime_private_party_check()==1);
    ++resumed_starts;
}
static void prepare_public(void) {
    if ((!selected && !ranked_selected) || rejection[0]) return;
    expire_check(); char body[512], target[128];
    snprintf(target,sizeof target,"/ryunet/ranked-party?name=LocalHost&members=1,2&mode=%d",selected);
    (void)compatibility_check(target,body,sizeof body);
    await_check();
}
static void test_ranked(void) {
    const char doc[] = "{\"members\":[1,2],\"ruleset_payload\":{\"filter\":{\"playlist_id\":0}}}";
    char confirmation_doc[256];
    unsigned char owner[0x960] = {0};
    uint64_t room = 91;
    void *result = NULL;
    memcpy(owner + 0x958, &room, 8);
    rejection[0] = 0;
    status = 200;
    snprintf(reply, sizeof reply, "ok");
    runtime_public_itemless_select(0);
    ranked_selected = 1;
    expire_check();
    int before = sent;
    /* A cold check never waits and never pops a modal: the press starts the search and the
     * server validates the party in its own asynchronous startSearch. */
    start_search(&result, NULL, doc, sizeof doc, NULL);
    assert(result && sent == before + 1 && !rejection[0]);
    assert(strstr(payload, "\"octr_ranked\"") && !strstr(payload, "\"octr_itemless\""));
    assert(strstr(payload, "\"mode\":0"));
    assert(strstr(payload, "\"version\":2"));
    assert(!confirmation_mode("{\"octr_ranked_v1\":\"1/91/0/0000000000000001\"}",
                              "octr_ranked_v1", 91, 1, 0));
    snprintf(confirmation_doc, sizeof confirmation_doc,
             "{\"octr_ranked_v1\":\"2/91/0/%016" PRIx64 "\"}", request_nonce);
    assert(accept(owner, room, confirmation_doc, (unsigned)strlen(confirmation_doc)));
    assert(runtime_ranked_active() && !runtime_public_itemless_active());
    runtime_public_itemless_select(1);
    prepare_public();
    start_search(&result, NULL, doc, sizeof doc, NULL);
    assert(result && strstr(payload, "\"octr_ranked\"") && strstr(payload, "\"octr_itemless\""));
    snprintf(confirmation_doc, sizeof confirmation_doc,
             "{\"octr_ranked_v1\":\"2/91/1/%016" PRIx64 "\",\"octr_itemless_v1\":\"2/91/1/%016" PRIx64 "\"}",
             request_nonce, request_nonce);
    assert(accept(owner, room, confirmation_doc, (unsigned)strlen(confirmation_doc)));
    assert(runtime_ranked_active() && runtime_public_itemless_active());
    uint64_t unrelated_room = room + 1;
    memcpy(owner + 0x958, &unrelated_room, 8);
    assert(!runtime_ranked_active() && !runtime_public_itemless_active());
    memcpy(owner + 0x958, &room, 8);
int mode_before_ui_reset = -1;
selected = 0;
assert(runtime_public_itemless_lobby_mode() == 1);
assert(runtime_ranked_session(NULL, &mode_before_ui_reset) && mode_before_ui_reset == 1);
selected = 1;
    runtime_public_itemless_reset();
    assert(!runtime_ranked_active());
    status = 404;
    expire_check();
    prepare_public(); /* the lobby tick caches the refused answer */
    int before_unsupported = sent;
    start_search(&result, NULL, doc, sizeof doc, NULL);
    assert(!result && sent == before_unsupported && strstr(rejection, "Ranked server"));
    rejection[0] = 0;
    status = 200;
    ranked_selected = 0;
    runtime_public_itemless_select(0);
}
/* A guest learns Ranked from the host's confirmation, then the host leaving fires a takeover:
 * new_session wipes ranked_selected before takeover_host, so the sticky mode must restore it. */
static void test_takeover_keeps_ranked(void) {
    unsigned char owner[0x960] = {0};
    uint64_t room = 777;
    char confirmation[160];
    int saved_ranked = ranked_selected, saved_joined = joined_ranked;
    int saved_search_ranked = search_is_ranked;
    ranked_selected = 0; joined_ranked = 0;
    runtime_public_itemless_reset();
    if (!registration_nonce) registration_nonce = 0x1111222233334444ULL;
    request_nonce = registration_nonce; searching = 1; /* a ranked search owns this room */
    matchmaking_room = room; matchmaking_owner = owner;
    snprintf(confirmation, sizeof confirmation,
             "{\"octr_ranked_v1\":\"2/%" PRIu64 "/0/%016" PRIx64 "\"}", room, request_nonce);
    assert(accept(owner, room, confirmation, (unsigned)strlen(confirmation)));
    assert(joined_ranked);
    runtime_public_itemless_new_session();
    assert(!ranked_selected && joined_ranked); /* the auto reset must not lose the sticky mode */
    runtime_public_itemless_takeover_host();
    assert(ranked_selected && search_is_ranked); /* re-search starts Ranked, not Casual */
    runtime_public_itemless_reset();
    ranked_selected = saved_ranked; joined_ranked = saved_joined; search_is_ranked = saved_search_ranked;
}
static void test_ranked_pause_quit(void) {
    native_placeable_event=forward_event;
    void *(ABI *vtable[])(void *)={object_type};
    void *meta[3]={0,0,"Scripts.Graph.PauseMenu_Octane_QuitButton"};
    void *behavior[2]={vtable,meta},*items[]={behavior};
    unsigned char button[0xf0]={0},list[0x28]={0},owner[0x960]={0};
    *(void **)(button+0xe8)=list;*(int *)(list+12)=1;*(void ***)(list+0x20)=items;
    void *event_meta[3]={0,0,"igGuiEventProjectGainFocus"};
    void *event[5]={vtable,event_meta,0,(void *)8};
    unsigned char *saved_owner=room_owner;
    uint64_t saved_room=confirmed_room;
    int saved_ranked=confirmed_ranked;
    room_owner=owner;confirmed_room=777;confirmed_ranked=1;
    memcpy(owner+0x958,&confirmed_room,8);
    int old_forwarded=forwarded,old_hidden=hidden_quit;
    placeable_event(button,event);
    assert(forwarded==old_forwarded+1 && hidden_quit==old_hidden+1);
    event_meta[2]="igGuiEventFocusInputPressed";
    placeable_event(button,event);
    assert(forwarded==old_forwarded+1);
    event_meta[2]="igGuiEventProjectUnload";
    placeable_event(button,event);
    assert(forwarded==old_forwarded+2 && hidden_quit==old_hidden+1);
    confirmed_ranked=0;
    assert(!ranked_pause_quit(button));
    confirmed_ranked=1;meta[2]="Scripts.Graph.PauseMenu_Octane_ResumeButton";
    assert(!ranked_pause_quit(button));
    room_owner=saved_owner;confirmed_room=saved_room;confirmed_ranked=saved_ranked;
}
static void test_ranked_button_event(void) {
    native_placeable_event = forward_event;
    void *(ABI *vtable[])(void *) = {object_type};
    void *event_meta[3] = {0, 0, "igGuiEventFocusInputPressed"};
    void *graph_meta[3] = {0, 0, "Scripts.Graph.Octane_Lobby_Main_MatchmakingButton"};
    void *event[5] = {vtable, event_meta, 0, (void *)8};
    void *behavior_meta[3] = {0, 0, "igGuiVscBehavior"};
    void *(ABI *graph_vtable[])(void *) = {graph_type};
    void *graph[5] = {graph_vtable, behavior_meta, 0, 0, graph_meta};
    /* Ranked uses the Matchmaking graph with cloned VSC state. */
    void *behavior_meta2[3] = {0, 0, "CGuiBehaviorOctaneNavigationItem"};
    void *(ABI *nav_vtable[])(void *) = {object_type};
    void *nav[70] = {nav_vtable, behavior_meta2};
    void *behaviors[] = {nav, graph};
    unsigned char button[0xf0] = {0}, list[0x28] = {0}, label[0x50] = {0}, comp[0x50] = {0};
    int two = 2;
    void *p = "OCTR_RANKED_BUTTON"; memcpy(button + 0x28, &p, 8);
    p = list; memcpy(button + 0xe8, &p, 8);
    p = behaviors; memcpy(list + 0x20, &p, 8);
    memcpy(list + 12, &two, 4);
    p = label; memcpy(nav + 30, &p, 8);
    p = comp; memcpy(label + 0x40, &p, 8);
    p = "[LevelListName]"; memcpy(label + 0x28, &p, 8);
    ranked_selected = 0;
    native_make_handle = make_test_handle;
    native_destroy_handle = destroy_test_handle;
    focus_test_button = button;
    p = NULL; memcpy(button + 0x28, &p, 8);
    /* Regression: returning after repeated Down (signal 5) must not replay a copied event. */
    event_meta[2] = (char *)"igGuiEventFocusInputRepeated";
    event[3] = (void *)5;
    placeable_event(button, event);
    p = "OCTR_RANKED_BUTTON"; memcpy(button + 0x28, &p, 8);
    event_meta[2] = (char *)"igGuiEventPlaceableGainFocus";
    placeable_event(button, event);
    event_meta[2] = (char *)"igGuiEventFocusInputPressed";
    event[3] = (void *)8;
    int before_forwarded = forwarded;
    placeable_event(button, event);
    assert(ranked_selected && ranked_labels == 2);
    assert(new_sessions==1 && hosted_sessions==1 && !selected);
    assert(forwarded == before_forwarded + 1);
    p = NULL; memcpy(button + 0x28, &p, 8);
    event_meta[2] = (char *)"igGuiEventPlaceableGainFocus";
    before_forwarded = forwarded;
    placeable_event(button, event);
    assert(forwarded == before_forwarded + 1 && synthetic_signal == 0 && handles == 0);
    assert(selector_left_calls == 2); /* Returning to Matchmaking clears its selector state too. */
    p = "OCTR_RANKED_BUTTON"; memcpy(button + 0x28, &p, 8);
    event_meta[2] = (char *)"igGuiEventProjectGainFocus";
    placeable_event(button, event);
    assert(selector_left_calls == 3);
    /* LoseFocus fires during scene teardown, when the shared comp may already be gone. */
    ranked_selected = 0;
    event_meta[2] = (char *)"igGuiEventPlaceableLoseFocus";
    placeable_event(button, event);
    assert(ranked_labels == 3 && !ranked_selected);
    event_meta[2] = (char *)"igGuiEventFocusInputPressed";
    p = NULL; memcpy(button + 0x28, &p, 8);
    placeable_event(button, event);
    assert(!ranked_selected);
    graph_meta[2] = (char *)"Scripts.Graph.Octane_Lobby_Main_PrivateMatchButton";
    event_meta[2] = (char *)"igGuiEventFocusInputPressed";
    ranked_selected = 1;
    placeable_event(button, event);
    assert(!ranked_selected && !selected && !searching && !confirmed_room);
    assert(new_sessions==3 && hosted_sessions==3);
    event_meta[2] = (char *)"igGuiEventPlaceableGainFocus";
    placeable_event(button, event);
    assert(selector_left_calls == 4);
    focus_test_button = NULL;
}
static void *ABI focus_field(void *meta, const char *name) {
    assert(!strcmp(((const char **)meta)[2], "Scripts.Graph.Octane_Lobby_Main_Root"));
    static unsigned char field[0x18];
    unsigned short offset = !strcmp(name, "Gui_Placeable_id_gto91nsu") ? 0xd0 :
        !strcmp(name, "Gui_Placeable_id_rcxu6uoi") ? 0xe0 : 0xf8;
    memcpy(field + 0x14, &offset, sizeof offset);
    return field;
}
static int ready_events, released_events;
static void *ready_event;
static void *ABI make_ready_event(void *pool) {
    assert(!pool); ready_event = calloc(1, 0x20); assert(ready_event);
    return ready_event;
}
static void ABI deliver_ready_event(void *root, void *event) {
    assert(root && event == ready_event && !((unsigned char *)event)[0x18]);
    ++ready_events;
}
static void ABI release_ready_event(void *event) {
    assert(event == ready_event); free(event); ready_event = NULL; ++released_events;
}
static void test_hof_native_ready(void) {
    native_leaderboard_ready = make_ready_event;
    native_release_object = release_ready_event;
    void (ABI *project_vtable[23])(void *, void *) = {0};
    project_vtable[0xb0 / 8] = deliver_ready_event;
    unsigned char project[0x60] = {0}, root[0x100] = {0}, wrapper[0x100] = {0};
    unsigned char controls[4][0x100] = {{0}}, navigation[4][0x120] = {{0}}, lists[4][0x28] = {{0}};
    const char *tags[] = {"OCTR_HOF_MODE", "OCTR_HOF_POPULATION", "OCTR_HOF_SEASON", "OCTR_HOF_ROW_0"};
    void *children[4], *behaviors[4];
    unsigned char children_list[0x28] = {0};
    *(void ***)project = (void **)project_vtable;
    *(void **)(project + 0x10) = wrapper;
    hof_test_wrapper = wrapper; hof_test_content = root;
    *(void **)(root + 0x48) = children_list;
    *(int *)(children_list + 12) = 4;
    *(void **)(children_list + 0x20) = children;
    for (unsigned i = 0; i < 4; ++i) {
        children[i] = controls[i]; behaviors[i] = navigation[i];
        *(const char **)(controls[i] + 0x28) = tags[i];
        *(void **)(controls[i] + 0xe8) = lists[i];
        *(int *)(lists[i] + 12) = 1;
        *(void **)(lists[i] + 0x20) = behaviors + i;
    }
    runtime_hof_native_ready(project);
    assert(ready_events == 1 && released_events == 1 && !ready_event && hof_legends == 1);
    for (unsigned i = 0; i < 4; ++i) {
        assert(*(void **)(navigation[i] + 0x110) == (i < 3 && i ? children[i - 1] : NULL));
        assert(*(void **)(navigation[i] + 0x118) == (i + 1 < 3 ? children[i + 1] : NULL));
    }
    native_hof_focus = hof_focus_mock;
    *(void **)(controls[2] + 0x20) = project;
    runtime_hof_focus_list(controls[2]);
    assert(hof_focused_project == project && hof_focused_list == controls[3]);
    *(void **)(controls[3] + 0x20) = project;
    *(void **)(project + 0x50) = controls[3];
    runtime_hof_focus_list(controls[3]);
    assert(hof_focused_list == controls[0]);
    hof_test_wrapper = hof_test_content = NULL;
    native_placeable_event = forward_event;
}
static void test_native_return_focus(void) {
    void *(ABI *vtable[])(void *) = {object_type};
    void *(ABI *graph_vtable[])(void *) = {graph_type};
    void *behavior_meta[3] = {0, 0, "igGuiVscBehavior"};
    void *root_meta[3] = {0, 0, "Scripts.Graph.Octane_Lobby_Main_Root"};
    unsigned char graph[0x140] = {0}, root[0xf0] = {0};
    unsigned char *fields = graph + 0x40;
    unsigned char ranked[0xf0] = {0}, hall[0xf0] = {0}, normal[0xf0] = {0};
    *(void ***)graph = (void **)graph_vtable;
    *(void **)(graph + 8) = behavior_meta;
    *(void **)(graph + 0x20) = root_meta;
    native_meta_field = focus_field; /* Runtime fields are inline, +0x18 stays NULL. */
    void *items[] = {graph};
    void *children[] = {normal, ranked, hall};
    unsigned char behaviors[0x28] = {0}, child_list[0x28] = {0};
    *(int *)(behaviors + 12) = 1; *(void ***)(behaviors + 0x20) = items;
    *(int *)(child_list + 12) = 3; *(void ***)(child_list + 0x20) = children;
    *(void **)(root + 0xe8) = behaviors; *(void **)(root + 0x48) = child_list;
    *(const char **)(ranked + 0x28) = "OCTR_RANKED_BUTTON";
    *(const char **)(hall + 0x28) = "OCTR_HALL_OF_FAME_BUTTON";
    *(void **)(ranked + 0x40) = root;
    native_make_handle = make_test_handle; native_destroy_handle = destroy_test_handle;
    native_placeable_event = forward_event;
    make_test_handle((void **)(fields + 0x90), normal);
    make_test_handle((void **)(fields + 0xa0), normal);
    make_test_handle((void **)(fields + 0xa8), hall); /* Private destination sentinel. */
    make_test_handle((void **)(fields + 0xb8), normal);
    void *event_meta[3] = {0, 0, "igGuiEventProjectGainFocus"};
    void *event[5] = {vtable, event_meta};
    for (int selection = 0; selection < 3; ++selection) {
        online_return_button = selection;
        int before = forwarded;
        matchmaking_search=searching=1;
        menu_party_returned=0;
        const int saved_mode=selected,saved_ranked=ranked_selected;
        placeable_event(root, event);
        assert(!matchmaking_search && !searching && selected==saved_mode && ranked_selected==saved_ranked);
        assert(forwarded == before + 1 && synthetic_signal == 0);
        assert(resolve_private_target((void **)(fields + 0xa0)) == children[selection]);
        assert(resolve_private_target((void **)(fields + 0xb8)) == children[selection]);
        assert(resolve_private_target((void **)(fields + 0xa8)) == hall);
        assert(handles == 4);
    }
    void **saved_network=network_manager;network_manager=NULL;
    root_meta[2]="Scripts.Graph.Octane_Lobby_Matchmaking_Root";
    native_return_focus(root,0);assert(!menu_party_returned);
    root_meta[2]="Scripts.Graph.Octane_Lobby_PrivateGame_Root";
    /* Focus back in the private room (a dialog closed, its players in): not an entry. */
    menu_party_returned=1;native_return_focus(root,0);assert(menu_party_returned);
    root_meta[2]="Scripts.Graph.Octane_Lobby_ModeSelect_Root";
    native_return_focus(root,0);assert(menu_party_returned && !search_mark);
    root_meta[2]="Scripts.Graph.Octane_Lobby_WaitingRoom_Root";
    native_return_focus(root,0);assert(menu_party_returned && !search_mark);
    root_meta[2]="Scripts.Graph.Octane_Lobby_Main_Root";
    network_manager=saved_network;
    online_return_button = 1;
    native_return_focus(ranked, 1);
    assert(resolve_private_target((void **)(fields + 0xa0)) == ranked);
    online_return_button = 0;
    native_return_focus(root, 0);
    assert(resolve_private_target((void **)(fields + 0xa0)) == normal);
    for (unsigned off = 0x90; off <= 0xb8; off += 8) release_handle((void **)(fields + off));
    assert(handles == 0);
}
static int account_notices;
static void ABI account_notice(const char *body, void *options) {
    assert(options == NULL && strstr(body, "Discord or Steam"));
    ++account_notices;
}

static int content_notices;
static void ABI content_notice(const char *body, void *options) {
    assert(options == NULL && strstr(body, "character, sound and HUD mods"));
    ++content_notices;
}

/* A "modified" game (scripts/online_content.py) never starts a public search; character mods do. */
static void test_public_content_gate(void) {
    Notice saved = native_notice;
    native_notice = content_notice;
    void *result = (void *)1;
    const int before = sent;
    online_content_state = "modified";
    start_search(&result, NULL, "{}", 3, NULL);
    /* the notice at once; the search left only when its Continue is pressed */
    assert(result == NULL && sent == before && content_notices == 1 && content_return == 1);
    void *(ABI *vtable[])(void *) = {object_type};
    void *pressed_meta[3] = {0, 0, "igGuiEventFocusInputPressed"};
    void *press[5] = {vtable, pressed_meta, 0, (void *)8, 0};
    unsigned char dialog_button[0xf0] = {0};
    native_placeable_event = forward_event;
    placeable_event(dialog_button, press);
    assert(content_return == 2);
    runtime_public_itemless_update();
    assert(content_return == 0);
    assert(!public_content_allowed() && content_notices == 2);      /* Ranked's GO TO LOBBY says it at once */
    online_content_state = "characters";
    assert(public_content_allowed() && content_notices == 2);
    online_content_state = "unknown";            /* not checked (an older launcher): not refused */
    assert(public_content_allowed() && content_notices == 2);
    online_content_state = "clean";
    native_notice = saved;
}

static void test_ranked_season_gate(void) {
    unsigned char manager[0x18] = {0}, roster[0x28] = {0}, player[0xb0] = {0};
    unsigned char party[0x70] = {0}, remote_roster[0x28] = {0};
    void *manager_pointer = manager, *party_pointer = party, *players[] = {player};
    void **saved_participants = participants, **saved_party = party_manager;
    participants = &manager_pointer; party_manager = &party_pointer;
    *(void **)(manager + 0x10) = roster;
    *(void ***)(roster + 0x20) = players; *(int *)(roster + 12) = 1;
    *(uint64_t *)(player + 0xa8) = 1;
    *(void **)(party + 0x30) = remote_roster;
    native_placeable_event = forward_event;
    native_make_handle = make_test_handle;
    native_resolve_handle = resolve_test_handle;
    native_destroy_handle = destroy_test_handle;
    void *(ABI *vtable[])(void *) = {object_type};
    void *(ABI *graph_vtable[])(void *) = {graph_type};
    void *event_meta[3] = {0, 0, "igGuiEventFocusInputPressed"};
    void *graph_meta[3] = {0, 0, "Scripts.Graph.Octane_Lobby_ModeSelect_GoToLobbyButton"};
    void *behavior_meta[3] = {0, 0, "igGuiVscBehavior"};
    void *graph[5] = {graph_vtable, behavior_meta, 0, 0, graph_meta};
    void *event[5] = {vtable, event_meta, 0, (void *)8, 0};
    void *behaviors[] = {graph};
    unsigned char button[0xf0] = {0}, list[0x28] = {0};
    void *p = list; memcpy(button + 0xe8, &p, 8);
    p = behaviors; memcpy(list + 0x20, &p, 8);
    int one = 1; memcpy(list + 12, &one, 4);
    private_room = 0;
    ranked_selected = 1;
    for (selected = 0; selected < 2; ++selected) {
        ranked_status_state = 0;
        int before = forwarded;
        placeable_event(button, event);
        assert(forwarded == before && !pending_private.target);
        runtime_ranked_gate_update(button, 1);
        float alpha; memcpy(&alpha, button + 0xe0, 4);
        assert(alpha == 0.45f);
        ranked_status_state = -1;
        placeable_event(button, event);
        assert(forwarded == before && pending_private.ranked && handles == 1);
    assert(pending_private.event && (void *)pending_private.event != (void *)event);
        assert(!runtime_private_party_update());
        ranked_status_state = 1;
        assert(runtime_private_party_update());
        assert(forwarded == before + 1 && !pending_private.target && handles == 0);
        runtime_ranked_gate_update(button, 1);
        memcpy(&alpha, button + 0xe0, 4);
        assert(alpha == 1.0f);
    }
    /* No Discord or Steam account linked: Ranked is open, but Go to Lobby tells the player and
     * stays (dimmed), directly and after a deferred status check. */
    {
        Notice saved_notice = native_notice;
        native_notice = account_notice;
        ranked_account_state = 0;
        ranked_status_state = 1;
        int before = forwarded, shown = account_notices;
        placeable_event(button, event);
        assert(forwarded == before && account_notices == shown + 1 && !pending_private.target);
        runtime_ranked_gate_update(button, 1);
        float alpha; memcpy(&alpha, button + 0xe0, 4);
        assert(alpha == 0.45f);
        ranked_status_state = -1;
        placeable_event(button, event);
        assert(forwarded == before && pending_private.ranked);
        ranked_status_state = 1;
        assert(!runtime_private_party_update());
        assert(forwarded == before && account_notices == shown + 2 && !pending_private.target && handles == 0);
        ranked_account_state = 1;
        native_notice = saved_notice;
    }
    /* Back cancels the deferred entry even when status later opens. */
    ranked_status_state = -1;
    placeable_event(button, event);
    event[3] = (void *)9;
    placeable_event(button, event);
    int before = forwarded;
    ranked_status_state = 1;
    assert(!runtime_private_party_update());
    assert(forwarded == before && handles == 0);
    ranked_selected = 0;
    selected = 0;
    participants = saved_participants; party_manager = saved_party;
    /* Opening mode selection is not entering a private lobby; keep current party intact. */
    graph_meta[2] = "Scripts.Graph.Octane_Lobby_Main_PrivateMatchButton";
    event[3] = (void *)8;
    private_room = 0;
    menu_party_returned = 1;
    placeable_event(button, event);
    assert(menu_party_returned && !entry_private);
    /* Into the private room from the mode screen: the party is counted there, before anyone joins. */
    graph_meta[2] = "Scripts.Graph.Octane_Lobby_ModeSelect_GoToLobbyButton";
    private_room = 1;
    menu_party_returned = 1; party_at_entry = 5;
    placeable_event(button, event);
    assert(!menu_party_returned && party_at_entry == ranked_party_size() && entry_private);
    private_room = 0; entry_private = 0;
}

static void test_hall_of_fame_button_event(void) {
    native_placeable_event = forward_event;
    void *(ABI *vtable[])(void *) = {object_type};
    void *event_meta[3] = {0, 0, "igGuiEventFocusInputPressed"};
    void *event[4] = {vtable, event_meta, 0, (void *)8};
    void *nav_meta[3] = {0, 0, "CGuiBehaviorOctaneNavigationItem"};
    void *(ABI *nav_vtable[])(void *) = {object_type};
    void *nav[70] = {nav_vtable, nav_meta};
    void *behaviors[] = {nav};
    unsigned char button[0xf0] = {0}, list[0x28] = {0}, label[0x50] = {0};
    int one = 1;
    void *p = "OCTR_HALL_OF_FAME_BUTTON"; memcpy(button + 0x28, &p, 8);
    p = list; memcpy(button + 0xe8, &p, 8);
    p = behaviors; memcpy(list + 0x20, &p, 8); memcpy(list + 12, &one, 4);
    p = label; memcpy(nav + 30, &p, 8);
    p = "[LevelListName]"; memcpy(label + 0x28, &p, 8);
    int before_forwarded = forwarded;
    placeable_event(button, event);
    assert(hall_labels == 1);
    assert(forwarded == before_forwarded + 1 && hof_opens == 1);
}
static void test_early_gate(void) {
    early=1;
    native_make_handle=make_test_handle;
    native_resolve_handle=resolve_test_handle;
    native_destroy_handle=destroy_test_handle;
    native_placeable_event=forward_event;
    void *(ABI *vtable[])(void *) = {object_type};
    void *event_meta[3]={0,0,"igGuiEventFocusInputPressed"};
    void *graph_meta[3]={0,0,"Scripts.Graph.Octane_Lobby_ModeSelect_GoToLobbyButton"};
    void *event[5]={vtable,event_meta,0,(void *)8,0};
    void *behavior_meta[3]={0,0,"igGuiVscBehavior"};
    void *(ABI *graph_vtable[])(void *) = {graph_type};
    void *graph[5]={graph_vtable,behavior_meta,0,0,graph_meta};
    void *behaviors[]={graph};
    unsigned char self[0xf0]={0}, list[0x28]={0}, manager[0x18]={0}, roster[0x28]={0};
    unsigned char players[8][0xb0]={{0}};
    void *members[8];
    unsigned char party[0x70]={0}, party_roster[0x28]={0}, records[8][0x38]={{0}};
    void *remote[8];
    void *party_pointer=party;party_manager=&party_pointer;
    assert(!runtime_private_match_active());
    party[0x6c]=1;
    private_room=0; /* A race has unloaded the private lobby scene. */
    assert(runtime_private_match_active());
    party[0x6c]=0; /* Native leave/reset clears it, even with peer traffic. */
    assert(!runtime_private_match_active());

    void *q=party_roster;memcpy(party+0x30,&q,8);
    q=remote;memcpy(party_roster+0x20,&q,8);
    int n=1;
    void *p=list;memcpy(self+0xe8,&p,8);
    p=behaviors;memcpy(list+0x20,&p,8);memcpy(list+12,&n,4);
    p=roster;memcpy(manager+0x10,&p,8);
    p=members;memcpy(roster+0x20,&p,8);
    void *manager_pointer=manager;participants=&manager_pointer;
    for(int i=0;i<8;++i) { members[i]=players[i];uint64_t pid=(uint64_t)i+1;memcpy(players[i]+0xa8,&pid,8); }
    /* Only one local player: remaining members live in the native party roster. */
    memcpy(roster+12,&n,4);
    for(int i=0;i<8;++i) {remote[i]=records[i];uint64_t pid=(uint64_t)i+1;memcpy(records[i]+0x18,&pid,8);}
    records[0][0x2c]=1; /* local record is already included by the search constructor */
    runtime_public_itemless_select(1);
    int before=sent, stop_before=stopped, return_before=returned;
    /* Public matchmaking must reach its native async task even with no HTTP
     * result. Compatibility is enforced by the server's startSearch handler. */
    int f=forwarded, d=dialogs, r=requests;
    placeable_event(self,event);
    assert(forwarded==f+1 && dialogs==d && requests==r);
    runtime_public_itemless_update();
    assert(requests==r && dialogs==d);
    /* Private ready/start is gated even with public Itemless disabled. */
    private_room=private_speed=1;
    setenv("BB_PUBLIC_ITEMLESS","0",1);
    graph_meta[2]="Scripts.Graph.Octane_Lobby_PrivateGame_ButtonReady";
    for(n=1;n<=8;++n) {
        memcpy(party_roster+12,&n,4);
        size_t len=0;
        for(int j=0;j<n;++j) {
            uint64_t pid=(uint64_t)j+1;memcpy(records[j]+0x18,&pid,8);
            len+=(size_t)snprintf(expected_ids+len,sizeof expected_ids-len,"%s%d",j?",":"",j+1);
        }
        status=409;strcpy(reply,"Update required: OldGuest");
        int d=dialogs;f=forwarded;
        settled_event(self,event);assert(forwarded==f && dialogs==d+1);
        status=200;strcpy(reply,"ok");
        settled_event(self,event);assert(forwarded==f+1);
        assert(!dispatch_approved_party);
        int previous_requests = requests;
        assert(runtime_private_party_check()==-1);
        await_check();
        assert(runtime_private_party_check());
        assert(requests > previous_requests); /* Later Start must revalidate. */
    }
    /* Keep malformed-roster and unrelated-input coverage on the private gate. */
    n=1;memcpy(party_roster+12,&n,4);strcpy(expected_ids,"1");
    settled_event(self,event);
    q=NULL;memcpy(party+0x30,&q,8);
    f=forwarded;r=requests;
    settled_event(self,event);assert(forwarded==f && requests==r);
    q=party_roster;memcpy(party+0x30,&q,8);
    graph[0]=vtable;
    status=409;strcpy(reply,"Update required: OldGuest");
    d=dialogs;settled_event(self,event);
    assert(forwarded==f && dialogs==d+1);
    graph[0]=graph_vtable;
    n=9;memcpy(party_roster+12,&n,4);r=requests;
    settled_event(self,event);assert(forwarded==f && requests==r);
    graph_meta[2]="Scripts.Graph.OtherButton";
    settled_event(self,event);assert(forwarded==++f && requests==r);
    graph_meta[2]="Scripts.Graph.Octane_Lobby_PrivateGame_ButtonReady";
    n=2;memcpy(party_roster+12,&n,4);memcpy(records[1]+0x18,records[0]+0x18,8);
    settled_event(self,event);assert(forwarded==f && requests==r);
    event[3]=(void *)9;settled_event(self,event);assert(forwarded==++f && requests==r);
    event[3]=(void *)8;
    uint64_t second_pid=2;memcpy(records[1]+0x18,&second_pid,8);
    n=1;memcpy(party_roster+12,&n,4);
    strcpy(reply,"ok");
    status=503;f=forwarded;settled_event(self,event);assert(forwarded==f);
    private_speed=0;r=requests;
    settled_event(self,event);assert(forwarded==f+1 && requests==r);
    /* An old server's capability-only success cannot authorize a release-gated feature. */
    private_teams=1; status=200; strcpy(reply,"ok-private-v1");
    f=forwarded; settled_event(self,event); assert(forwarded==f);
    strcpy(reply,"ok-private-release:v0.0.1");
    settled_event(self,event); assert(forwarded==f);
    strcpy(reply,"ok"); private_teams=0;
    /* Cup and Teams require their own capability, even in Normal mode. */
    for (int feature=0;feature<5;++feature) {
        private_cup=feature==0 || feature==3;private_teams=feature==1 || feature==4;
        modified_difficulty=feature>=2;
        for(n=1;n<=8;++n) {
            memcpy(party_roster+12,&n,4);
            size_t len=0;
            for(int j=0;j<n;++j) len+=(size_t)snprintf(expected_ids+len,sizeof expected_ids-len,"%s%d",j?",":"",j+1);
            status=409;strcpy(reply,"Update required: OldGuest");
            f=forwarded;int d=dialogs;
            settled_event(self,event);assert(forwarded==f && dialogs==d+1);
            status=200;strcpy(reply,"ok");
            settled_event(self,event);assert(forwarded==f+1);
        }
        status=503;f=forwarded;settled_event(self,event);assert(forwarded==f);
        private_room=0;r=requests;settled_event(self,event);
        assert(forwarded==f+1 && requests==r); /* No Team/Cup gate on public menus. */
        private_room=1;
    }
    /* Exercise pending actions with HTTP genuinely blocked, not a retry click. */
    private_cup=private_teams=0; private_speed=1; modified_difficulty=1;
    n=2;memcpy(party_roster+12,&n,4);
    strcpy(expected_ids,"1,2");status=200;strcpy(reply,"ok");
    void *sender=NULL;make_test_handle(&sender,self);event[2]=sender;
    for (int scenario=0;scenario<14;++scenario) {
        expire_check();
        pthread_mutex_lock(&http_lock);block_get=1;entered_get=0;pthread_mutex_unlock(&http_lock);
        f=forwarded;d=dialogs;
        int starts=resumed_starts;
        if (scenario==11) {
            assert(runtime_private_party_check()==-1);
            runtime_private_party_defer_start(self,resume_test_start);
        } else placeable_event(self,event);
        pthread_mutex_lock(&http_lock);
        while (!entered_get) pthread_cond_wait(&http_condition,&http_lock);
        pthread_mutex_unlock(&http_lock);
        assert(pending_private.target && forwarded==f && dialogs==d);
        uint64_t deadline=pending_private.deadline;
        if (scenario!=11) {
            placeable_event(self,event); /* Holding Confirm cannot queue another action. */
            assert(pending_private.deadline==deadline && forwarded==f);
            assert(*(unsigned *)sender==3);
        }
        for (int frame=0;frame<20;++frame) assert(!runtime_private_party_update());
        assert(forwarded==f && dialogs==d);
        if (scenario==1) {
            uint64_t changed=3;memcpy(records[1]+0x18,&changed,8);
            strcpy(expected_ids,"1,3");
        } else if (scenario==2) private_cup=1;
        else if (scenario==3) private_room=0;
        else if (scenario==4) runtime_public_itemless_reset();
        else if (scenario==5) memset((char *)pending_private.target+0x28,0,8);
        else if (scenario==6) pending_private.deadline=0;
        else if (scenario==7) {
            event[3]=(void *)9;placeable_event(self,event);event[3]=(void *)8;
            ++f; /* Navigation itself is still forwarded. */
        } else if (scenario==8) {
            event_meta[2]="igGuiEventPlaceableLoseFocus";
            placeable_event(self,event);++f;
            event_meta[2]="igGuiEventFocusInputPressed";
        } else if (scenario==9) { status=409;strcpy(reply,"Update required: OldGuest"); }
        else if (scenario==10) status=503;
        else if (scenario==12) runtime_private_party_cancel();
        else if (scenario==13) {
            unsigned char other[0xf0]={0};
            placeable_event(other,event);++f; /* The same input can propagate to another widget. */
            assert(pending_private.target);
        }
        if (scenario>=1 && scenario<=8) {
            assert(!runtime_private_party_update());
            assert(!pending_private.target);
        }
        pthread_mutex_lock(&http_lock);block_get=0;pthread_cond_broadcast(&http_condition);pthread_mutex_unlock(&http_lock);
        await_check();
        if (scenario==0 || scenario>=9) (void)runtime_private_party_update();
        assert(!pending_private.target && handles==1 && *(unsigned *)sender==2);
        assert(forwarded==f+(scenario==0 || scenario==13));
        assert(resumed_starts==starts+(scenario==11));
        assert(dialogs==d+(scenario==6 || scenario==9 || scenario==10));
        if (scenario==9) assert(strstr(message,"OldGuest"));
        if (scenario==6) assert(strstr(message,"timed out"));
        uint64_t restored=2;memcpy(records[1]+0x18,&restored,8);
        strcpy(expected_ids,"1,2");private_cup=0;private_room=1;status=200;strcpy(reply,"ok");
    }
    event[2]=NULL;release_handle(&sender);assert(handles==0);
    /* An expired success is queued and refreshed, never turned into a popup. */
    assert(runtime_private_party_check()==1);
    pthread_mutex_lock(&check_lock);check_until=0;pthread_mutex_unlock(&check_lock);
    f=forwarded;d=dialogs;
    placeable_event(self,event);assert(pending_private.target && dialogs==d);
    await_check();assert(runtime_private_party_update());
    assert(forwarded==f+1 && dialogs==d && handles==0);
    private_cup=private_teams=modified_difficulty=0;
    private_room=0;setenv("BB_PUBLIC_ITEMLESS","1",1);
    assert(sent==before && stopped==stop_before && returned==return_before);
    participants=NULL;party_manager=NULL;early=0;
}

static void test_hof_project_shortcut(void) {
    unsigned char root[0xf0] = {0};
    *(const char **)(root + 0x28) = "OCTR_HOF_ROOT";
    void *(ABI *vtable[])(void *) = {object_type};
    void *meta[3] = {0, 0, "igGuiEventProjectInputPressed"};
    void *event[5] = {vtable, meta, 0, (void *)10};
    private_room = 0; hof_input_handled = 1; hof_input_signal = 0;
    placeable_event(root, event);
    assert(hof_input_signal == 10);
    hof_input_handled = 0;
}

static int allocated_input_events;
static void *ABI allocate_input_event(void *meta, void *allocator, int zero) {
    assert(meta && !allocator && !zero);
    struct GuiFocusInputEvent *event = calloc(1, sizeof *event);
    static void *(ABI *vtable[])(void *) = {object_type};
    event->vtable = vtable; event->flags = (uintptr_t)meta;
    ++allocated_input_events;
    return event;
}
static void ABI release_input_event(void *object) {
    struct GuiFocusInputEvent *event = object;
    assert(event && allocated_input_events > 0);
    release_handle(&event->sender);
    --allocated_input_events; free(event);
}

static int entry_joins,entry_creates,host_starts;
static void *test_session_manager[6];
static int removed_players, disconnected_players;
static void *expected_remote;
static int ABI remove_remote(void *manager, void *player) {
    assert(manager == *participants && player == expected_remote);
    ++removed_players;
    return 1;
}
static void ABI disconnect_remote(void *session, uint64_t pid) {
    assert(session == ptr(*match_session_manager,0x28) && pid == 321 && removed_players == disconnected_players + 1);
    if (disconnected_players) assert(requeued_room==74 && matchmaking_search);
    ++disconnected_players;
}
static void ABI release_match_session(void *session) {
    assert(__atomic_fetch_sub((int *)((char *)session+8),1,__ATOMIC_SEQ_CST)==1);
}
static int peer_back_messages;
static void ABI peer_back_mock(void) { ++peer_back_messages; }
static int race_binds;
static int race_connection_sets;
static uint64_t expected_switch_departed;
static void ABI bind_race_mock(void *race,void *connection) {
    if (expected_switch_departed) assert(race_switch_departed==expected_switch_departed);
    void **replication=ptr(race,0x20);
    replication[2]=connection;
    ++race_binds;
}
static void ABI set_race_connection_mock(void *replication,void *connection) {
    ((void **)replication)[2]=connection;
    ++race_connection_sets;
}
static void ABI start_match_host_mock(void *manager) {
    assert(manager==test_session_manager);
    ++host_starts;
}
static void ABI entry_join(void *self,uint64_t search,uint64_t room,const char *host,unsigned hn,const char *doc,unsigned dn) {
    (void)search;(void)host;(void)hn;(void)doc;(void)dn;
    __atomic_store_n((uint64_t *)((char *)self+0x958),room,__ATOMIC_RELAXED);
    __atomic_add_fetch(&entry_joins,1,__ATOMIC_RELAXED);
}
static void ABI entry_create(void *self,uint64_t search,uint64_t room,uint64_t version,const char *host,unsigned hn,const char *doc,unsigned dn) {
    (void)version;entry_join(self,search,room,host,hn,doc,dn);
    __atomic_sub_fetch(&entry_joins,1,__ATOMIC_RELAXED);__atomic_add_fetch(&entry_creates,1,__ATOMIC_RELAXED);
}
static void *parallel_create(void *owner) {
    for(int i=0;i<100;++i) created(owner,4,74,1,"",0,"{}",2);
    return NULL;
}
static void test_join_waits_for_native_session(void) {
    unsigned char owner[0x1000]={0};
    void *manager[6]={0}, *manager_pointer=manager;
    void **saved_manager=match_session_manager;
    Join saved_join=native_join;
    Create saved_create=native_create;
    const int before_creates=entry_creates;
    const int saved_guest=party_guest, before=entry_joins;
    runtime_public_itemless_reset();party_guest=1;
    runtime_public_itemless_party_ready();
    match_session_manager=&manager_pointer;native_join=entry_join;
    manager[3]=manager; /* HostSession has not published manager+0x28 yet. */
    char host[]="host snapshot", backend[]="{}";
    joined(owner,1,810,host,sizeof host-1,backend,sizeof backend-1);
    assert(entry_joins==before && pending_lobby_join && matchmaking_room==810);
    host[0]='X';backend[0]='X';
    assert(!strcmp(pending_lobby_join->docs,"host snapshot"));
    assert(!strcmp(pending_lobby_join->docs+pending_lobby_join->host_len+1,"{}"));
    joined(owner,1,810,"",0,"{}",2);
    update_lobby_connection();assert(entry_joins==before);
    manager[3]=NULL;manager[4]=manager;
    update_lobby_connection();assert(entry_joins==before);
    manager[4]=NULL;
    update_lobby_connection();update_lobby_connection();
    assert(entry_joins==before+1 && !pending_lobby_join);
    runtime_public_itemless_reset();runtime_public_itemless_party_ready();
    manager[3]=manager;
    joined(owner,2,811,"",0,"{}",2);
    assert(pending_lobby_join);
    runtime_public_itemless_reset();manager[3]=NULL;
    update_lobby_connection();assert(entry_joins==before+1 && !pending_lobby_join);
    runtime_public_itemless_party_ready();manager[3]=manager;
    joined(owner,3,812,"",0,"{}",2);
    assert(pending_lobby_join);
    native_create=entry_create;
    created(owner,3,812,2,"",0,"{}",2);
    assert(!pending_lobby_join && entry_creates==before_creates+1);
    manager[3]=NULL;update_lobby_connection();
    assert(entry_joins==before+1);
    runtime_public_itemless_reset();
    match_session_manager=saved_manager;native_join=saved_join;party_guest=saved_guest;
    native_create=saved_create;entry_creates=before_creates;entry_joins=before;
}

static void test_cancelled_and_duplicate_lobby_entries(void) {
    unsigned char owner[0x1000]={0};char doc[512];
    Join old_join=native_join;Create old_create=native_create;
    native_join=entry_join;native_create=entry_create;
    runtime_public_itemless_reset();party_guest=0;selected=ranked_selected=0;
    runtime_public_itemless_party_ready();
    confirmation_doc(doc,sizeof doc,71,registration_nonce);
    const int notices=dialogs;
    created(owner,1,71,1,"",0,doc,(unsigned)strlen(doc));
    joined(owner,1,71,"",0,"{}",2);
    assert(!entry_creates && !entry_joins && dialogs==notices && !matchmaking_room);
    matchmaking_search=1;
    created(owner,2,72,1,"",0,"{}",2);created(owner,2,72,1,"",0,"{}",2);
    assert(entry_creates==1);
    runtime_public_itemless_reset();
    created(owner,2,72,1,"",0,"{}",2);assert(entry_creates==1);
    party_guest=1;runtime_public_itemless_party_ready();
    unsigned char follower[0x80]={0};void *follower_pointer=follower;
    void **saved_party_manager=party_manager;
    party_manager=&follower_pointer;follower[0x6c]=1;
    joined(owner,3,73,"",0,"{}",2);joined(owner,3,73,"",0,"{}",2);
    assert(entry_joins==1 && !follower[0x6c]);
    party_manager=saved_party_manager;
    /* A guest may take over as host once; repeated create must not reopen map voting. */
    created(owner,3,73,2,"",0,"{}",2);created(owner,3,73,2,"",0,"{}",2);
    assert(entry_creates==2 && entry_joins==1);
    runtime_public_itemless_reset();party_guest=0;matchmaking_search=1;
    Join old_update=native_update;
    native_update=entry_join;
    joined(owner,4,75,"",0,"{}",2);
    void *manager=test_session_manager;
    match_session_manager=&manager;
    native_start_match_host=start_match_host_mock;
    native_bind_race_connection=bind_race_mock;
    native_set_race_connection=set_race_connection_mock;
    kart_racing=1;
    created(owner,4,76,2,"",0,"{}",2);
    assert(entry_creates==2 && entry_joins==2 && matchmaking_room==75);
    created(owner,4,75,2,"",0,"{}",2);
    created(owner,4,75,2,"",0,"{}",2);
    assert(entry_creates==2 && entry_joins==3 && matchmaking_created);
    unsigned char premade_roster[0x28]={0}, premade_member[0x38]={0};
    void *premade_rows[]={premade_member}, *roster_pointer=premade_roster;
    uint64_t premade_pid=123;
    memcpy(premade_member+0x18,&premade_pid,sizeof premade_pid);
    void **premade_items=premade_rows;
    memcpy(premade_roster+0x20,&premade_items,sizeof premade_items);
    *(int *)(premade_roster+12)=1;
    memcpy(follower+0x30,&roster_pointer,sizeof roster_pointer);
    party_manager=&follower_pointer;
    had_public_race=1;kart_racing=0;
    apply_room_update(75,1,"{\"octr_departed_pid\":123,\"octr_departed_name\":\"Test-Racer\"}",0);
    assert(!departures); /* Race ended, but this player is still in the premade. */
    kart_racing=1;
    ranked_finished_room=75;
    apply_room_update(75,2,"{\"octr_departed_pid\":123,\"octr_departed_name\":\"Test-Racer\"}",0);
    assert(!departures); /* Native race flags survive the result screen and loading. */
    ranked_finished_room=76;last_departure_version=1;last_departure_pid=0;
    apply_room_update(75,2,"{\"octr_departed_pid\":123,\"octr_departed_name\":\"Test-Racer\"}",0);
    ranked_finished_room=0;
    party_manager=saved_party_manager;had_public_race=0;
    apply_room_update(75,2,"{\"octr_departed_pid\":123,\"octr_departed_name\":\"Test-Racer\"}",0);
    apply_room_update(76,3,"{\"octr_departed_pid\":124}",0);
    assert(departures==1);
    apply_room_update(75,3,"{\"octr_departed_pid\":125}",0);
    assert(departures==2);
    room_alive_after=UINT64_MAX;
    test_session_manager[3]=manager;
    runtime_public_itemless_poll_race_host();
    assert(host_starts==0);
    test_session_manager[3]=NULL;
    runtime_public_itemless_poll_race_host();
    runtime_public_itemless_poll_race_host();
    assert(host_starts==1 && pending_host_room==75);
    host_retry_after=0;
    runtime_public_itemless_poll_race_host();
    assert(host_starts==2);
    unsigned char connection[0x120]={0};
    int role=1;
    memcpy(connection+0x118,&role,sizeof role);
    void *session[3]={NULL,NULL,connection};
    test_session_manager[5]=session;
    void **previous_network=network_manager;
    void *replication[3]={NULL,NULL,manager}, *race[5]={NULL,NULL,NULL,manager,replication};
    void *network[19]={0}, *root=network;
    network[15]=race;
    network_manager=&root;
    host_retry_after=0;
    runtime_public_itemless_poll_race_host();
    assert(race_binds==0 && pending_host_room==75);
    network[18]=manager;
    runtime_public_itemless_poll_race_host();
    assert(race_binds==1 && replication[2]==connection && pending_host_room==75);
    runtime_public_itemless_poll_race_host();
    assert(race_binds==1);
    role=2;
    memcpy(connection+0x118,&role,sizeof role);
    connection[0x110]=1;
    race_join_room=75;
    race_join_until=UINT64_MAX;
    race_join_old_connection=manager;
    unsigned char state_call[5]={0};
    unsigned char *saved_state_call=race_join_state_call;
    race_join_state_call=state_call;
    replication[2]=manager;
    void (ABI *saved_switch)(void *,void *)=native_race_switch_original;
    native_race_switch_original=bind_race_mock;
    native_bind_race_connection=race_connection_switch;
    left_room_pending=75;race_handoff_departed_pid=expected_switch_departed=33;
    race[3]=NULL;
    runtime_public_itemless_poll_race_host();
    assert(race_join_room==75 && race_binds==1 && replication[2]==manager);
    race[3]=manager;
    runtime_public_itemless_poll_race_host();
    assert(race_connection_sets==0 && race_binds==2 && replication[2]==connection && !race_join_room);
    assert(!race_switch_departed);
    runtime_public_itemless_poll_race_host();
    assert(race_connection_sets==0 && race_binds==2); /* Bind once, including native peer cleanup. */
    native_race_switch_original=saved_switch;expected_switch_departed=0;
    race_join_state_call=saved_state_call;
    network_manager=previous_network;
    native_bind_race_connection=NULL;
    native_set_race_connection=NULL;
    assert(!pending_host_room && host_starts==2);
    test_session_manager[5]=NULL;
    room_alive_after=0;
    runtime_public_itemless_reset();
    runtime_public_itemless_poll_race_host();
    for (int i=0;i<1000 && __atomic_load_n(&room_alive_busy,__ATOMIC_SEQ_CST);++i) usleep(1000);
    assert(!__atomic_load_n(&room_alive_busy,__ATOMIC_SEQ_CST));
    assert(!strcmp(last_room_poll,"/ryunet/room-alive?name=LocalHost&room=75&race=1"));
    match_session_manager=NULL;
    native_start_match_host=NULL;
    kart_racing=0;
    native_update=old_update;
    frozen_race=0;
    runtime_public_itemless_reset();matchmaking_search=1;
    pthread_t workers[2];
    for(int i=0;i<2;++i) assert(!pthread_create(&workers[i],NULL,parallel_create,owner));
    for(int i=0;i<2;++i) assert(!pthread_join(workers[i],NULL));
    assert(entry_creates==3);
    const char held[]="{\"octr_waiting_for_opponent\":true}";
    assert(accept(owner,74,held,sizeof held-1));
    int elapsed, timeout, range, ranked;
    assert(runtime_matchmaking_view(&elapsed,&timeout,&range,&ranked) && !timeout && !ranked);
    void *cancel_result=NULL;
    native_cancel=send_cancel;
    cancel_search(&cancel_result,NULL,1,NULL);
    assert(matchmaking_room==74);
    native_send_departure=peer_back_mock;
    match_session_manager=&manager;
    test_session_manager[5]=session;
    role=2;
    memcpy(connection+0x118,&role,sizeof role);
    void **saved_participants=participants;
    unsigned char list[0x28]={0}, player[0xb0]={0};
    void *rows[]={player}, *players[3]={NULL,NULL,list}, *roster=players;
    participants=&roster;
    send_confirmed_departure(74);
    assert(peer_back_messages==0);
    *(int *)(list+12)=1;
    void **items=rows;
    memcpy(list+0x20,&items,sizeof items);
    unsigned char lobby_button[0xf0]={0};
    *(const char **)(lobby_button+0x28)="OCTR_PUBLIC_LOBBY";
    void *(ABI *input_vtable[])(void *)={object_type};
    void *meta[]={NULL,NULL,"igGuiEventProjectInputPressed"};
    void *back[]={input_vtable,meta,NULL,(void *)9,NULL};
    native_placeable_event=forward_event;
    int before=forwarded;
    placeable_event(lobby_button,back);
    assert(forwarded==before+1 && !peer_back_messages && matchmaking_room==74 && left_room_pending==74);
    const int previous_leaves=matchmaking_leaves;
    notify_room_left();
    assert(matchmaking_leaves==previous_leaves+1 && matchmaking_keep);
    notify_room_left();
    assert(peer_back_messages==1);
    /* A different client survives the same departure. */
    assert(accept(owner,74,held,sizeof held-1));
    unsigned char remote[0xb0]={0};
    void *pair[]={player,remote};
    uint64_t departed_pid=321;
    memcpy(remote+0xa8,&departed_pid,sizeof departed_pid);
    *(unsigned *)(remote+0x20)=8;
    *(int *)(list+12)=2;
    items=pair;
    memcpy(list+0x20,&items,sizeof items);
    expected_remote=remote;
    native_remove_player=remove_remote;
    native_disconnect_player=disconnect_remote;
    void (ABI *saved_release)(void *)=native_release_object;
    native_release_object=release_match_session;
    apply_lobby_departure(322);
    assert(!removed_players && !disconnected_players);
    const char held_after_leave[]="{\"octr_waiting_for_opponent\":true,\"octr_departed_pid\":321,\"octr_departed_name\":\"Guest\"}";
    native_update=entry_join;
    updated(owner,74,2,"",0,held_after_leave,sizeof held_after_leave-1);
    updated(owner,74,2,"",0,held_after_leave,sizeof held_after_leave-1);
    assert(removed_players==1 && disconnected_players==1 && matchmaking_room==74);
    const int previous_updates=entry_joins;
    updated(owner,74,1,"",0,"{}",2);
    assert(entry_joins==previous_updates && runtime_public_waiting_for_opponent());
    native_update=old_update;
    const char requeue[]="{\"octr_requeue\":true,\"octr_departed_pid\":321,\"octr_departed_name\":\"OldHost\"}";
    memset(owner+0x958,0,8); /* Old native session closed just before the push. */
    updated(owner,74,3,"",0,requeue,sizeof requeue-1);
    assert(!matchmaking_room && matchmaking_search && runtime_public_waiting_for_opponent());
    cancel_search(&cancel_result,NULL,1,NULL);
    runtime_public_itemless_abort_queue();
    do_queue_abort();
    assert(matchmaking_search && !queue_abort_pending);
    assert(removed_players==2 && disconnected_players==2);
    void *leave_result=NULL;
    native_leave=send_leave;
    leave_lobby(&leave_result,NULL,74);
    assert(runtime_matchmaking_view(&elapsed,&timeout,&range,&ranked) && !timeout && !ranked);
    updated(owner,74,3,"",0,requeue,sizeof requeue-1);
    assert(!matchmaking_room && matchmaking_search);
    assert(removed_players==2 && disconnected_players==2);
    native_remove_player=NULL;
    native_disconnect_player=NULL;
    native_release_object=saved_release;
    participants=saved_participants;
    native_send_departure=NULL;
    test_session_manager[5]=NULL;
    match_session_manager=NULL;
    struct SearchPoll *pending=calloc(1,sizeof *pending);
    pending->generation=session_generation;
    pending->serial=search_poll_serial;
    strcpy(pending->target,"/ryunet/matchmaking-status?name=LocalHost");
    strcpy(reply,"{\"state\":\"matched\",\"elapsed\":0,\"timeout\":180,\"range\":0}");
    search_poll(pending);
    assert(runtime_matchmaking_view(&elapsed,&timeout,&range,&ranked));
    strcpy(reply,"ok");
    match_session_manager=&manager;
    native_start_match_host=start_match_host_mock;
    test_session_manager[5]=session;
    created(owner,4,76,1,"",0,"{}",2);
    assert(entry_creates==3 && pending_lobby_create && matchmaking_room==76 && runtime_public_waiting_for_opponent());
    assert(runtime_matchmaking_view(&elapsed,&timeout,&range,&ranked) && !ranked && !timeout);
    int previous_starts=host_starts;
    update_lobby_connection();
    update_lobby_connection();
    assert(entry_creates==3 && host_starts==previous_starts+1 && runtime_public_waiting_for_opponent());
    role=1;
    memcpy(connection+0x118,&role,sizeof role);
    room_players=2;
    update_lobby_connection();
    assert(entry_creates==4 && !pending_lobby_create && !prepare_requeue_host && !runtime_public_waiting_for_opponent());
    assert(!runtime_matchmaking_view(&elapsed,&timeout,&range,&ranked));
    match_session_manager=NULL;
    native_start_match_host=NULL;
    test_session_manager[5]=NULL;
    room_players=0;
    room_poll_reply="{\"alive\":false,\"requeued\":true}";
    room_alive_worker((void *)(uintptr_t)76);
    assert(requeued_room==76 && runtime_matchmaking_view(&elapsed,&timeout,&range,&ranked));
    assert(!matchmaking_room && !timeout && !ranked);
    room_poll_reply="{\"players\":\"6\"}";
    party_guest=1;
    runtime_public_itemless_reset();native_join=old_join;native_create=old_create;
}

static int party_returns;
static void ABI return_party_mock(void *party) { assert(party); ++party_returns; }
static int party_quits;
static void ABI quit_party_mock(void *party) { assert(party);++party_quits; }
static int ranked_lobby_returns;
static void ABI ranked_return_to_lobby(void) { ++ranked_lobby_returns; }
static void test_native_party_menu_return(void) {
    unsigned char party[0x80]={0},session[0x20]={0},connection[0x120]={0};
    void *party_value=party,*network_value=test_network;
    void **saved_party=party_manager,**saved_network=network_manager;
    void (ABI *saved_return)(void *)=native_party_return;
    void (ABI *saved_quit)(void *)=native_party_quit_trace;
    void (ABI *saved_stop)(void *)=native_stop_matchmaking;
    void (ABI *saved_lobby)(void)=native_return_to_lobby;
    int saved_guest=party_guest,old_stops=stopped,old_returns=party_returns;
    party_manager=&party_value;network_manager=&network_value;
    native_party_return=return_party_mock;native_party_quit_trace=quit_party_mock;
    native_stop_matchmaking=stop_matchmaking;native_return_to_lobby=ranked_return_to_lobby;party_quits=0;
    *(void **)(party+0x18)=session;*(void **)(session+0x10)=connection;
    *(int *)(connection+0x118)=1;party[0x6c]=1;
    party_guest=0;menu_party_returned=0;
    party_at_entry=2; /* a party of two searched together */
    matchmaking_search=1;search_mark=check_clock();search_elapsed_base=99;
    selected=ranked_selected=1;
    struct SearchPoll *job=calloc(1,sizeof *job);
    job->generation=session_generation;job->serial=search_poll_serial;
    strcpy(job->target,"/ryunet/matchmaking-status?name=LocalHost");
    void *(ABI *event_vtable[])(void *)={object_type};
    void *(ABI *behavior_vtable[])(void *)={graph_type};
    void *event_meta[3]={0,0,"igGuiEventProjectInputPressed"};
    void *behavior_meta[3]={0,0,"igGuiVscBehavior"};
    void *root_meta[3]={0,0,"Scripts.Graph.Octane_Lobby_Matchmaking_Root"};
    void *behavior[5]={behavior_vtable,behavior_meta,0,0,root_meta};
    void *items[]={behavior},*event[5]={event_vtable,event_meta,0,(void *)9,0};
    unsigned char root[0xf0]={0},list[0x28]={0};
    *(void **)(root+0xe8)=list;*(int *)(list+12)=1;*(void ***)(list+0x20)=items;
    void (ABI *saved_event)(void *,void *)=native_placeable_event;
    native_placeable_event=forward_event;party_return_expected_target=root;
    placeable_event(root,event);
    assert(party_returns==old_returns && stopped==old_stops && matchmaking_search);
    /* Cancelling the dialog restores lobby focus without ending search. */
    event_meta[2]="igGuiEventProjectGainFocus";event[3]=NULL;
    placeable_event(root,event);
    assert(party_returns==old_returns && stopped==old_stops && matchmaking_search);
    /* Only the accepted exit's destination focus commits cancellation. */
    party_return_expected_target=NULL;
    root_meta[2]="Scripts.Graph.Octane_Lobby_ModeSelect_Root";
    placeable_event(root,event);
    native_placeable_event=saved_event;
    assert(party_returns==old_returns+1 && party_quits==0 && stopped==old_stops+1);
    assert(*party_manager==party && !party[0x6c] && selected && ranked_selected);
    assert(!search_mark && !matchmaking_search && !search_server_active && !matchmaking_room);
    strcpy(reply,"{\"state\":\"searching\",\"elapsed\":99,\"timeout\":180,\"range\":400,\"ranked\":true}");
    search_poll(job);assert(!search_mark && !search_server_active);
    int elapsed,timeout,range,ranked;
    (void)runtime_matchmaking_view(&elapsed,&timeout,&range,&ranked);
    int polls=requests;update_search_clock();
    assert(requests==polls && !search_poll_running);
    runtime_public_itemless_menu_return();assert(party_returns==old_returns+1 && party_quits==0 && stopped==old_stops+1);
    /* ModeSelect can initialize again after the leader already started the next Ranked queue.
     * A duplicate menu-return hook must not erase that fresh search or broadcast another return. */
    matchmaking_search=1;matchmaking_room=57;search_mark=check_clock();
    runtime_public_itemless_menu_return();
    assert(matchmaking_search && matchmaking_room==57 && search_mark);
    assert(party_returns==old_returns+1 && party_quits==0 && stopped==old_stops+1);
    matchmaking_search=matchmaking_room=0;search_mark=0;
    /* A premade leader returns with their party; a follower never broadcasts a host return. */
    menu_party_returned=0;runtime_public_itemless_menu_return();assert(party_returns==old_returns+2 && party_quits==0);
    *(int *)(connection+0x118)=2;party_guest=1;menu_party_returned=0;
    runtime_public_itemless_menu_return();assert(party_returns==old_returns+2 && party_quits==0);
    matchmaking_search=1;search_mark=check_clock();search_elapsed_base=180;
    int attempts=timeout_attempts;runtime_public_itemless_update();
    assert(timeout_attempts==attempts && search_timeout_pending);
    /* Solo hosts leave alone. Public and Ranked premades return together. */
    *(int *)(connection+0x118)=1;party_guest=0;
    {
        const int returns=party_returns;
        menu_party_returned=0;party_at_entry=1;
        runtime_public_itemless_menu_return();
        assert(party_returns==returns && party_quits==1);
        menu_party_returned=0;party_at_entry=2;
        runtime_public_itemless_menu_return();
        assert(party_returns==returns+1 && party_quits==1);
        /* Private rooms keep the existing host-migration rule. */
        menu_party_returned=0;party_at_entry=2;entry_private=1;
        runtime_public_itemless_menu_return();
        assert(party_returns==returns+1 && party_quits==2);
        menu_party_returned=0;queue_return_pending=1;entry_private=0;
        runtime_public_itemless_menu_return();
        assert(party_returns==returns+2 && party_quits==2);
        /* A guest of that room leaves the party on the way out: the host's notification
         * (message 0x29) and the roster reset are the game's way out. */
        const int saved_installed=installed;installed=1;
        menu_party_returned=0;party_at_entry=2;entry_private=0;
        *(int *)(connection+0x118)=2;party[0x6c]=1;
        unsigned char roster[0x28]={0},peer[0x30]={0};
        void *items[1]={peer};
        *(int *)(roster+12)=1;*(void ***)(roster+0x20)=items;
        *(void **)(party+0x30)=roster;
        runtime_public_itemless_menu_return();
        installed=saved_installed;
        assert(party_quits==3 && party_returns==returns+2 && !party[0x6c]);
        *(int *)(connection+0x118)=1;
        native_party_reset_trace=reset_mock;
        menu_party_returned=1;party_return_received=0;host_lost_return=-1;
        traced_party_reset(party);assert(host_lost_return==0);
        party_return_received=1;
        traced_party_reset(party);assert(host_lost_return==1);
        const int sessions=new_sessions;
        entry_private=1;menu_party_returned=1;queue_return_pending=party_return_received=1;
        runtime_public_itemless_takeover_host();
        assert(new_sessions==sessions && !entry_private && !menu_party_returned &&
               !queue_return_pending && !party_return_received);
        runtime_public_itemless_menu_return();
        assert(party_quits==3 && party_returns==returns+3);
        runtime_public_itemless_takeover_host();
        assert(new_sessions==sessions && !menu_party_returned);
        runtime_public_itemless_menu_return();
        assert(party_quits==3 && party_returns==returns+4);
        new_sessions=sessions;
        /* The next search is not a private room any more. */
        arm_party_return();assert(!entry_private && !menu_party_returned);
        party_at_entry=1;
    }
    /* Ranked completion keeps the original premade, including delayed room cleanup. */
    unsigned char ranked_roster[0x28]={0},ranked_peer[0x30]={0};
    void *ranked_members[]={ranked_peer};
    *(int *)(ranked_roster+12)=1;*(void ***)(ranked_roster+0x20)=ranked_members;
    *(void **)(party+0x30)=ranked_roster;
    for (int role=1;role<=2;++role) {
        *(int *)(connection+0x118)=role;
        menu_party_returned=0;entry_private=0;party_at_entry=2;party_exit_sent=0;
        const int quits=party_quits,returns=party_returns;
        runtime_public_itemless_ranked_return();
        assert(party_quits==quits && party_returns==returns+(role==1));
        assert(*(void **)(party+0x18)==session);
        guest_quit_at=0;
        runtime_public_itemless_guest_left("cusa13795-session-PremadeHost");
        guest_room_open=1;runtime_public_itemless_guest_room(0);
        assert(party_quits==quits);
        runtime_public_itemless_ranked_return();
        assert(party_quits==quits && party_returns==returns+(role==1));
    }
    /* Match takeover cannot turn a solo entrant into a premade of room members. */
    menu_party_returned=0;party_at_entry=1;
    runtime_public_itemless_takeover_host();
    assert(party_at_entry==1);
    *(int *)(connection+0x118)=1;
    const int solo_quits=party_quits;
    runtime_public_itemless_ranked_return();
    assert(party_quits==solo_quits+1 && !ranked_return_party);
    arm_private_room();assert(!ranked_return_party && entry_private);
    entry_private=0;menu_party_returned=1;
    runtime_public_itemless_reset();
    party_guest=saved_guest;party_manager=saved_party;network_manager=saved_network;
    native_party_return=saved_return;native_stop_matchmaking=saved_stop;
    native_party_quit_trace=saved_quit;native_return_to_lobby=saved_lobby;
    stopped=old_stops;selected=ranked_selected=0;strcpy(reply,"ok");
}

/* The guest gets back to the main menu without passing the room's own screens: the party must
 * still be left (its Lobby Members row and the host's notice), once per visit. */
static void *ABI missing_field(void *meta,const char *name){(void)meta;(void)name;return NULL;}
static void test_guest_main_menu_leaves_party(void) {
    void *(ABI *graph_vtable[])(void*)={graph_type};
    void *behavior_meta[3]={0,0,"igGuiVscBehavior"};
    void *root_meta[3]={0,0,"Scripts.Graph.Octane_Lobby_PrivateGame_Root"};
    unsigned char graph[0x40]={0},root[0xf0]={0};
    *(void ***)graph=(void **)graph_vtable;
    *(void **)(graph+8)=behavior_meta;
    *(void **)(graph+0x20)=root_meta;
    void *items[]={graph};
    unsigned char behaviors[0x28]={0};
    *(int *)(behaviors+12)=1;*(void ***)(behaviors+0x20)=items;
    *(void **)(root+0xe8)=behaviors;
    unsigned char party[0x80]={0},session[0x20]={0},connection[0x120]={0};
    unsigned char roster[0x28]={0},peer[0x30]={0};
    void *members[1]={peer},*party_value=party;
    *(void **)(party+0x18)=session;*(void **)(session+0x10)=connection;
    *(int *)(connection+0x118)=2;
    *(void **)(party+0x30)=roster;*(int *)(roster+12)=1;*(void ***)(roster+0x20)=members;
    void **saved_party=party_manager;
    void (ABI *saved_quit)(void *)=native_party_quit_trace;
    void *(ABI *saved_field)(void *,const char *)=native_meta_field;
    const int saved_guest=party_guest,saved_quits=party_quits,saved_return=online_return_button;
    party_manager=&party_value;native_party_quit_trace=quit_party_mock;native_meta_field=missing_field;
    party_guest=1;guest_room_open=0;guest_quit_at=0;menu_party_returned=1;online_return_button=1;
    /* ModeSelect is the setup screen before the room: not a room, no party leave. */
    root_meta[2]="Scripts.Graph.Octane_Lobby_ModeSelect_Root";
    native_return_focus(root,0);assert(!guest_room_open && party_quits==saved_quits);
    /* The room itself (host: START GAME, guest: READY) marks the guest as in it. */
    root_meta[2]="Scripts.Graph.Octane_Lobby_PrivateGame_Root";
    native_return_focus(root,0);assert(guest_room_open && party_quits==saved_quits);
    /* Back out lands on the room's setup screen: the guest leaves the party there. */
    root_meta[2]="Scripts.Graph.Octane_Lobby_ModeSelect_Root";
    native_return_focus(root,0);assert(party_quits==saved_quits+1 && !guest_room_open);
    /* In the room again and out within 5 s: the native leave already ran once. */
    root_meta[2]="Scripts.Graph.Octane_Lobby_PrivateGame_Root";
    native_return_focus(root,0);assert(guest_room_open);
    root_meta[2]="Scripts.Graph.Octane_Lobby_ModeSelect_Root";
    native_return_focus(root,0);assert(party_quits==saved_quits+1 && !guest_room_open);
    /* Straight to the main menu instead: same leave, the second visit counts. */
    guest_quit_at=0;
    root_meta[2]="Scripts.Graph.Octane_Lobby_PrivateGame_Root";
    native_return_focus(root,0);assert(guest_room_open);
    root_meta[2]="Scripts.Graph.Octane_Lobby_Main_Root";
    native_return_focus(root,0);
    assert(party_quits==saved_quits+2 && !guest_room_open);
    /* In the room again and out within 5 s: the native leave already ran once. */
    root_meta[2]="Scripts.Graph.Octane_Lobby_PrivateGame_Root";
    native_return_focus(root,0);assert(guest_room_open);
    root_meta[2]="Scripts.Graph.Octane_Lobby_ModeSelect_Root";
    native_return_focus(root,0);assert(party_quits==saved_quits+2 && !guest_room_open);
    /* "Waiting on host..." is not the room: it must not trigger a leave by itself. */
    root_meta[2]="Scripts.Graph.Octane_Lobby_WaitingRoom_Root";
    native_return_focus(root,0);assert(!guest_room_open && party_quits==saved_quits+2);
    /* Leaving the host's session the game's own way means the same party leave; our own
     * session's delete (closing a room we host) does not. */
    guest_quit_at=0;
    runtime_public_itemless_guest_left("cusa13795-session-LocalHost");
    assert(party_quits==saved_quits+2);
    runtime_public_itemless_guest_left("cusa13795-session-HostPlayer");
    assert(party_quits==saved_quits+3);
    runtime_public_itemless_guest_left("cusa13795-session-HostPlayer");
    assert(party_quits==saved_quits+3);
    /* The Lobby Members rows of a private room say the guest is inside it whatever screen has
     * focus: out of the room leaves the party once, and a leave with no room open does nothing. */
    guest_quit_at=0;
    runtime_public_itemless_guest_room(0);
    assert(party_quits==saved_quits+3 && !guest_room_open);
    runtime_public_itemless_guest_room(1);
    assert(guest_room_open && party_quits==saved_quits+3);
    guest_quit_at=0;
    runtime_public_itemless_guest_room(0);
    assert(!guest_room_open && party_quits==saved_quits+4);
    /* In the room again and out within 5 s: the native leave already ran once. */
    runtime_public_itemless_guest_room(1);
    runtime_public_itemless_guest_room(0);
    assert(!guest_room_open && party_quits==saved_quits+4);
    party_manager=saved_party;native_party_quit_trace=saved_quit;party_guest=saved_guest;
    party_quits=saved_quits;online_return_button=saved_return;guest_room_open=0;
    guest_quit_at=0;
    native_meta_field=saved_field;
}

static int native_race_party_returns;static unsigned char ABI race_party_return_mock(void *sender,void *message,void *context) {
    assert(sender==(void *)1 && message==(void *)2 && context==(void *)3);
    ++native_race_party_returns;return 1;
}
static void ABI menu_return_to_lobby_mock(void) { ++returned; }
static void test_guest_menu_return_destination(void) {
    unsigned char (ABI *saved_message)(void *,void *,void *)=native_party_return_trace;
    void (ABI *saved_lobby)(void)=native_return_to_lobby;
    void (ABI *saved_stop)(void *)=native_stop_matchmaking;
    void **saved_network=network_manager,**saved_party=party_manager;
    unsigned char party[0x80]={0};void *party_value=party;party_manager=&party_value;
    party[0x6c]=1;
    int old_ready=timeout_ready,old_attempts=timeout_attempts,old_stops=stopped,old_returns=returned;
    void *network=test_network;network_manager=&network;
    native_return_to_lobby=menu_return_to_lobby_mock;native_stop_matchmaking=stop_matchmaking;
    native_party_return_trace=race_party_return_mock;
    frozen_race=0;timeout_ready=1;
    assert(received_party_return((void *)1,(void *)2,(void *)3)==1);
    assert(timeout_attempts==old_attempts+1 && timeout_route==2 && !native_race_party_returns && returned==old_returns && !party[0x6c]);
    /* After Waiting Room closes the matchmaking GUI, repeated returns must not reload. */
    timeout_ready=0;menu_party_returned=1;
    const int waiting_stops=stopped;
    for (int i=0;i<2;++i) {
        assert(received_party_return((void *)1,(void *)2,(void *)3)==1);
        assert(returned==old_returns && stopped==waiting_stops && !native_race_party_returns);
    }
    /* A new lobby lifecycle still permits the fallback. */
    menu_party_returned=0;
    assert(received_party_return((void *)1,(void *)2,(void *)3)==1);
    assert(returned==old_returns+1 && !native_race_party_returns);
    /* Private entry remains a real exit even when no public search ran. */
    party[0x6c]=1;
    assert(received_party_return((void *)1,(void *)2,(void *)3)==1);
    assert(returned==old_returns+2 && !party[0x6c]);
    frozen_race=1;
    unsigned char ranked_owner[0x960]={0};
    unsigned char *saved_owner=room_owner;
    const uint64_t saved_room=confirmed_room;
    const int saved_confirmed_ranked=confirmed_ranked,saved_entry=party_at_entry;
    confirmed_room=4321;confirmed_ranked=1;room_owner=ranked_owner;
    memcpy(ranked_owner+0x958,&confirmed_room,8);
    entry_private=0;party_at_entry=2;ranked_return_party=0;
    assert(received_party_return((void *)1,(void *)2,(void *)3)==1);
    assert(native_race_party_returns==1 && timeout_attempts==old_attempts+5);
    assert(ranked_return_party); /* Leader's message may beat this guest's finish timer. */
    /* Ranked races do not freeze private-room settings. They still need native scene exit. */
    frozen_race=0;kart_racing=1;
    assert(received_party_return((void *)1,(void *)2,(void *)3)==1);
    assert(native_race_party_returns==2 && timeout_attempts==old_attempts+5);
    kart_racing=0;
    ranked_return_party=0;party_at_entry=saved_entry;
    room_owner=saved_owner;confirmed_room=saved_room;confirmed_ranked=saved_confirmed_ranked;
    /* A delayed previous-room return cannot cancel a premade's replacement Ranked search. */
    requeued_room=4321;matchmaking_search=1;matchmaking_room=0;party_at_entry=2;menu_party_returned=0;
    const int requeue_attempts=timeout_attempts,requeue_returns=returned;
    assert(received_party_return((void *)1,(void *)2,(void *)3)==1);
    assert(timeout_attempts==requeue_attempts && returned==requeue_returns && matchmaking_search);
    /* Manual queues have no requeued_room; stale native packets still cannot cancel them. */
    requeued_room=0;
    for (int follower=0;follower<2;++follower) {
        matchmaking_search=!follower;search_server_active=0;
        *(int *)(party+0x54)=follower ? 7 : 0;
        assert(received_party_return((void *)1,(void *)2,(void *)3)==1 && party_return_check==1);
        struct SearchPoll *job=calloc(1,sizeof *job);
        job->generation=session_generation;job->serial=search_poll_serial;
        strcpy(job->target,"/ryunet/matchmaking-status?name=LocalHost");
        strcpy(reply,"{\"state\":\"searching\",\"elapsed\":0,\"timeout\":180,\"range\":400,\"ranked\":true}");
        search_poll(job);
        assert(!party_return_check && search_server_active && timeout_attempts==requeue_attempts);
    }
    /* A real leader cancellation is still applied after server confirmation, on the game thread. */
    assert(received_party_return((void *)1,(void *)2,(void *)3)==1);
    struct SearchPoll *idle=calloc(1,sizeof *idle);
    idle->generation=session_generation;idle->serial=search_poll_serial;
    strcpy(idle->target,"/ryunet/matchmaking-status?name=LocalHost");
    strcpy(reply,"{\"state\":\"idle\",\"elapsed\":0,\"timeout\":180,\"range\":0}");
    search_poll(idle);
    assert(party_return_check==2 && timeout_attempts==requeue_attempts);
    search_poll_after=UINT64_MAX;timeout_ready=1;
    runtime_public_itemless_update();
    assert(!party_return_check && timeout_attempts==requeue_attempts+1 && timeout_route==2);
    *(int *)(party+0x54)=0;
    search_server_active=0;search_poll_after=0;strcpy(reply,"ok");
    requeued_room=0;matchmaking_search=0;party_at_entry=saved_entry;
    frozen_race=0;timeout_ready=old_ready;timeout_attempts=old_attempts;
    stopped=old_stops;returned=old_returns;
    unsigned char owner[0x960]={0};
    uint64_t found=89;
    memcpy(owner+0x958,&found,sizeof found);
    const char party_return[]="{\"octr_party_return\":true}";
    timeout_ready=1;
    int before_return=timeout_attempts;
    updated(owner,88,3,"",0,party_return,sizeof party_return-1);
    assert(timeout_attempts==before_return);
    matchmaking_search=1;
    updated(owner,found,3,"",0,party_return,sizeof party_return-1);
    assert(timeout_attempts==before_return); /* The native object still holds the previous room ID. */
    matchmaking_search=0;
    /* Server return updates must not cancel menu UI while a public race is active. */
    kart_racing=1;
    updated(owner,found,3,"",0,party_return,sizeof party_return-1);
    assert(timeout_attempts==before_return);
    kart_racing=0;
    updated(owner,found,3,"",0,party_return,sizeof party_return-1);
    assert(timeout_attempts==before_return+1 && timeout_route==2);
    timeout_ready=old_ready;timeout_attempts=old_attempts;
    native_party_return_trace=saved_message;native_return_to_lobby=saved_lobby;
    native_stop_matchmaking=saved_stop;network_manager=saved_network;party_manager=saved_party;
}

static void test_timeout_preserves_party_and_mode(void) {
    int saved_guest=party_guest;party_guest=0;
    const int before_stops=stopped,before_returns=returned,before_dialogs=dialogs;
    unsigned char party[0x80]={0};void *party_value=party;void **previous_party=party_manager;
    party_manager=&party_value;party[0x6c]=1;
    for (int ranked=0;ranked<2;++ranked) for(int mode=0;mode<2;++mode) {
        runtime_public_itemless_reset();selected=mode;ranked_selected=ranked;
        matchmaking_search=1;search_mark=check_clock();search_elapsed_base=180;
        timeout_ready=0;runtime_public_itemless_update();
        assert(search_timeout_pending && selected==mode && ranked_selected==ranked);
        timeout_ready=1;runtime_public_itemless_update();
        assert(!matchmaking_search && !search_timeout_pending);
        assert(selected==mode && ranked_selected==ranked && *party_manager==party && party[0x6c]);
        assert(stopped==before_stops && returned==before_returns && dialogs==before_dialogs);
    }
    selected=ranked_selected=0;party_manager=previous_party;party_guest=saved_guest;
}

static void test_ranked_device_and_search_clock(void) {
    int saved_guest=party_guest;party_guest=0;
    const char *device=ranked_device();
    assert(strlen(device)==64 && !strspn(device,"-"));
    assert(strspn(device,"0123456789abcdef")==64);
    char *extended=extend("{\"members\":[1]}",32,42,1,0);
    assert(extended && strstr(extended,device));free(extended);
    unsigned char party[0x100]={0}, roster[0x40]={0}, peers[4][0x40]={{0}};
    void *items[]={peers[0],peers[1],peers[2],peers[3]}, *party_pointer=party;
    void **saved=party_manager;party_manager=&party_pointer;
    *(void **)(party+0x30)=roster;*(void ***)(roster+0x20)=items;
    *(int *)(roster+12)=3;assert(ranked_party_size()==4);
    *(int *)(roster+12)=4;assert(ranked_party_size()==5);
    assert(!ranked_entry_allowed());
    void *result=(void *)1;
    runtime_public_itemless_reset();
    int sent_before=sent,notices_before=ranked_notices;
    Notice saved_notice=native_notice;native_notice=count_notice;
    ranked_selected=1;
    start_search(&result,NULL,"{\"members\":[1,2,3,4,5]}",25,NULL);
    assert(!result && sent==sent_before && ranked_notices==notices_before+1 && !matchmaking_search);
    native_notice=saved_notice;
    matchmaking_search=ranked_selected=1;
    runtime_public_itemless_update();
    assert(!matchmaking_search && !rejection[0]);
    ranked_selected=0;
    peers[3][0x2c]=1;assert(ranked_party_size()==4);
    int saved_installed=installed;installed=1;
    unsigned char active_session[0x20]={0},active_connection[0x120]={0};
    *(void **)(party+0x18)=active_session;*(void **)(active_session+0x10)=active_connection;
    *(int *)(active_connection+0x30)=1;
    assert(runtime_online_party_active());
    for (int i=0;i<4;++i) peers[i][0x2c]=1;
    assert(ranked_party_size()==1 && runtime_online_party_active());
    *(int *)(active_connection+0x30)=0;
    assert(!runtime_online_party_active());
    *(void **)(party+0x18)=NULL;
    assert(!runtime_online_party_active());
    installed=saved_installed;
    party_manager=saved;
    runtime_public_itemless_reset();
    matchmaking_search=1;search_mark=check_clock();search_elapsed_base=42;
    search_is_ranked=1;search_range=700;search_policy_at=check_clock();
    search_poll_after=UINT64_MAX;
    int elapsed,timeout,range,ranked;
    assert(runtime_matchmaking_view(&elapsed,&timeout,&range,&ranked));
    assert(elapsed==42 && timeout==180 && range==700 && ranked);
    matchmaking_room=42;
    assert(!runtime_matchmaking_view(&elapsed,&timeout,&range,&ranked));
    matchmaking_room=0;
    /* Party followers use server elapsed time; stale worker replies cannot restore a cancelled search. */
    runtime_public_itemless_reset();
    snprintf(reply,sizeof reply,"{\"state\":\"searching\",\"elapsed\":45,\"timeout\":180,\"range\":1000,\"ranked\":true}");
    match_found=1; /* A previous room must not keep a fresh queue in matched state. */
    struct SearchPoll *job=calloc(1,sizeof *job);
    job->generation=session_generation;job->serial=search_poll_serial;
    strcpy(job->target,"/ryunet/matchmaking-status?name=LocalHost");search_poll(job);
    assert(runtime_matchmaking_view(&elapsed,&timeout,&range,&ranked));
    assert(elapsed==45 && range==1000 && ranked);
    assert(!match_found);
    match_found=1;
    snprintf(reply,sizeof reply,"{\"state\":\"timed_out\",\"elapsed\":180,\"timeout\":180,\"range\":1000,\"ranked\":true}");
    job=calloc(1,sizeof *job);job->generation=session_generation;job->serial=search_poll_serial;
    strcpy(job->target,"/ryunet/matchmaking-status?name=LocalHost");search_poll(job);
    assert(!match_found);
    match_found=matchmaking_search=1;
    const uint64_t pending_mark=search_mark;
    snprintf(reply,sizeof reply,"{\"state\":\"idle\",\"elapsed\":0,\"timeout\":180,\"range\":0,\"ranked\":false}");
    job=calloc(1,sizeof *job);job->generation=session_generation;job->serial=search_poll_serial;
    strcpy(job->target,"/ryunet/matchmaking-status?name=LocalHost");search_poll(job);
    assert(!match_found && matchmaking_search && search_mark==pending_mark);
    match_found=1;matchmaking_room=42; /* A poll started before join cannot undo that join. */
    job=calloc(1,sizeof *job);job->generation=session_generation;job->serial=search_poll_serial;
    strcpy(job->target,"/ryunet/matchmaking-status?name=LocalHost");search_poll(job);
    assert(match_found && matchmaking_room==42);
    matchmaking_room=0;
    job=calloc(1,sizeof *job);job->generation=session_generation;job->serial=search_poll_serial;
    strcpy(job->target,"/ryunet/matchmaking-status?name=LocalHost");
    runtime_public_itemless_reset();search_poll(job);
    assert(!runtime_matchmaking_view(&elapsed,&timeout,&range,&ranked));
    runtime_public_itemless_reset();
    matchmaking_search=1;search_mark=check_clock();search_elapsed_base=180;search_poll_after=0;
    party_return_check=1;
    update_search_clock();assert(search_timeout_pending && !rejection[0] && !search_poll_running);
    runtime_public_itemless_reset();rejection[0]=0;snprintf(reply,sizeof reply,"ok");
    party_guest=saved_guest;
}

static int admission_error;
static unsigned char expected_security[24];
static uint32_t ABI security_init_mock(void *session,void *id,void *key,int host) {
    assert(session && host==1);
    assert(!memcmp(id,expected_security,8) && !memcmp(key,expected_security+8,16));
    return 1;
}
static void test_promoted_host_security(void) {
    unsigned char owner[0x968]={0}, job[0xa8]={0}, connection[0x120]={0};
    void *session[3]={NULL,NULL,connection}, *manager[6]={NULL,NULL,owner,job,NULL,NULL};
    void *root=manager;
    void **old_manager=match_session_manager;
    SessionInit old_init=native_session_init;
    native_session_init=security_init_mock;match_session_manager=&root;
    runtime_public_itemless_reset();
    matchmaking_owner=owner;matchmaking_room=pending_host_room=3;
    void *job_session=session;memcpy(job+0xa0,&job_session,sizeof job_session);
    unsigned char reserved[24];memset(reserved,0x11,sizeof reserved);
    memset(owner+0x900,0x22,24);memset(expected_security,0x22,24);
    assert(traced_session_init(session,reserved,reserved+8,1)==1);
    assert(!memcmp(owner+0x900,expected_security,24));
    job_session=owner;memcpy(job+0xa0,&job_session,sizeof job_session);
    memset(expected_security,0x11,24);
    assert(traced_session_init(session,reserved,reserved+8,1)==1);
    job_session=session;memcpy(job+0xa0,&job_session,sizeof job_session);
    runtime_public_itemless_reset();
    assert(traced_session_init(session,reserved,reserved+8,1)==1);
    native_session_init=old_init;match_session_manager=old_manager;
}
static void test_failed_race_join_cleanup(void) {
    unsigned char owner[0x968]={0}, state_call[5]={0x90,0x90,0x90,0x90,0x90};
    unsigned char *old_call=race_join_state_call;
    runtime_public_itemless_reset();
    matchmaking_owner=owner;matchmaking_room=race_join_room=3;
    race_join_state_call=state_call;
    retire(owner,0);
    assert(!matchmaking_room && !race_join_room);
    assert(!memcmp(state_call,"\xe8\x85\xe8\xff\xff",5));
    race_join_state_call=old_call;
}
static unsigned char ABI admission_mock(void *session,void *peer,int *reason) {
    assert(session && peer==session);
    unsigned char *owner=ptr(*match_session_manager,0x10);
    *reason=owner[0x960] ? admission_error : 0x28;
    return *reason==0;
}
static void test_race_handoff_admission(void) {
    unsigned char owner[0x968]={0}, connection[0x120]={0};
    void *session[3]={NULL,NULL,connection}, *manager[6]={NULL,NULL,owner,NULL,NULL,session};
    void *root=manager;
    void **old_manager=match_session_manager;
    SessionAdmission old_admission=native_session_admission;
    match_session_manager=&root;
    native_session_admission=admission_mock;
    runtime_public_itemless_reset();
    matchmaking_owner=owner;matchmaking_room=3;
    *(int *)(connection+0x118)=1;
    kart_racing=1;local_race_quitting=0;
    int reason=0;
    assert(!admit_race_rejoin(session,session,&reason) && reason==0x28);
    host_attempt_until=check_clock()+20000;
    assert(admit_race_rejoin(session,session,&reason) && reason==0 && !owner[0x960]);
    admission_error=1; /* Capacity/authentication refusals must still reach the caller. */
    assert(!admit_race_rejoin(session,session,&reason) && reason==1 && !owner[0x960]);
    admission_error=0;
    kart_racing=0;
    assert(!admit_race_rejoin(session,session,&reason) && reason==0x28);
    kart_racing=1;local_race_quitting=1;
    assert(!admit_race_rejoin(session,session,&reason) && reason==0x28);
    local_race_quitting=0;manager[5]=NULL;
    assert(!admit_race_rejoin(session,session,&reason) && reason==0x28);
    manager[5]=session;host_attempt_until=check_clock();
    assert(!admit_race_rejoin(session,session,&reason) && reason==0x28);
    owner[0x960]=1;
    assert(admit_race_rejoin(session,session,&reason) && owner[0x960]);
    runtime_public_itemless_reset();kart_racing=0;
    match_session_manager=old_manager;native_session_admission=old_admission;
}

static int ownership_calls;
static void ABI host_change_update(void *owner,uint64_t room,uint64_t version,const char *host,unsigned hn,const char *doc,unsigned dn) {
    (void)owner;(void)room;(void)version;(void)host;(void)hn;(void)doc;(void)dn;
    assert(race_handoff_departed_pid);
}
static void test_handoff_without_departure_field(void) {
    unsigned char owner[0x968]={0};
    const char host2[]="{\"listen_server\":{\"host_player_id\":22}}";
    const char host3[]="{\"listen_server\":{\"host_player_id\":44}}";
    Join old_update=native_update;
    native_update=host_change_update;
    runtime_public_itemless_reset();
    *(uint64_t *)(owner+0x958)=3;
    matchmaking_room=3;matchmaking_owner=owner;race_lobby_host_pid=11;kart_racing=1;
    created(owner,1,3,2,host2,sizeof host2-1,"{}",2);
    assert(race_handoff_departed_pid==11 && race_lobby_host_pid==22 && !last_departure_pid);
    updated(owner,3,2,host2,sizeof host2-1,"{}",2);
    assert(race_handoff_departed_pid==11);
    updated(owner,3,3,host3,sizeof host3-1,"{}",2);
    assert(race_handoff_departed_pid==22 && race_lobby_host_pid==44 && !last_departure_pid);
    runtime_public_itemless_reset();assert(!race_handoff_departed_pid);
    native_update=old_update;kart_racing=0;race_lobby_host_pid=left_room_pending=0;
}
static int removed_racers;
static unsigned char departed_racer[0xb0], surviving_racer[0xb0];
static int ABI remove_racer_mock(void *manager,void *player) {
    (void)manager;(void)player;++removed_racers;return 1;
}
static void ABI switch_roster_mock(void *race,void *connection) {
    (void)race;(void)connection;
    remove_race_player(NULL,departed_racer);
    remove_race_player(NULL,surviving_racer);
}
static void test_handoff_roster(void) {
    void *replication[3]={0}, *race[5]={NULL,NULL,NULL,NULL,replication};
    void (*old_switch)(void *,void *)=native_race_switch_original;
    int (*old_remove)(void *,void *)=native_remove_player_original;
    native_race_switch_original=switch_roster_mock;native_remove_player_original=remove_racer_mock;
    *(uint64_t *)(departed_racer+0xa8)=33;*(uint64_t *)(surviving_racer+0xa8)=22;
    left_room_pending=matchmaking_room=last_departure_room=3;last_departure_pid=0;
    race_handoff_departed_pid=33; /* Host-change pushes need not contain octr_departed_pid. */
    kart_racing=1;local_race_quitting=0;removed_racers=0;
    race_connection_switch(race,(void *)1);
    assert(removed_racers==1 && !race_switch_departed);
    /* Genuine departure outside the synchronous transport switch still runs. */
    assert(remove_race_player(NULL,surviving_racer)==1 && removed_racers==2);
    local_race_quitting=1;race_connection_switch(race,(void *)1);assert(removed_racers==4);
    local_race_quitting=0;kart_racing=0;race_connection_switch(race,(void *)1);assert(removed_racers==6);
    native_race_switch_original=old_switch;native_remove_player_original=old_remove;
    runtime_public_itemless_reset();last_departure_room=last_departure_pid=left_room_pending=0;
}
static uint64_t ABI take_racer_mock(void *manager,void *entity) {
    assert(manager);
    ((unsigned char *)entity)[0x3b]|=1;
    ++ownership_calls;
    return 0;
}
static void test_handoff_racer_ownership(void) {
    unsigned char racers[5][0x30]={0}, players[3][0xb0]={0}, entities[5][0x40]={0};
    void *entity_handles[5][6]={{0}}, *player_handles[3][6]={{0}}, *rows[5];
    unsigned char list[0x28]={0}, connection[0x120]={0};
    void *racer_root[3]={NULL,NULL,list}, *racer_manager=racer_root;
    void *object_root[6]={0}, *object_manager=object_root;
    object_root[5]=object_root;
    void *replication[3]={NULL,NULL,connection}, *race[5]={NULL,NULL,NULL,NULL,replication};
    void *network[16]={0}, *network_root=network;
    network[15]=race;
    for (int i=0;i<5;++i) {
        rows[i]=racers[i];entity_handles[i][5]=entities[i];
        void *handle=entity_handles[i];memcpy(racers[i]+0x20,&handle,8);
        if (i>0 && i<4) {
            player_handles[i-1][5]=players[i-1];handle=player_handles[i-1];
            memcpy(racers[i]+0x10,&handle,8);
        }
    }
    /* AI, local human, surviving remote, departed host, unresolved human handle. */
    *(unsigned *)(players[1]+0x20)=9;*(uint64_t *)(players[1]+0xa8)=22;
    *(unsigned *)(players[2]+0x20)=UINT32_MAX;*(uint64_t *)(players[2]+0xa8)=33;
    void *unresolved[6]={0}, *handle=unresolved;
    memcpy(racers[4]+0x10,&handle,8);
    *(int *)(list+12)=5;void *items=rows;memcpy(list+0x20,&items,8);
    *(int *)(connection+0x118)=1;
    race_racers=&racer_manager;race_object_root=&object_manager;network_manager=&network_root;
    native_take_racer_ownership=take_racer_mock;
    matchmaking_room=last_departure_room=3;last_departure_pid=0;race_handoff_departed_pid=33;
    host_attempt_until=100;local_race_quitting=0;kart_racing=1;ownership_calls=0;
    restore_host_racers(3,50);
    assert(ownership_calls==3 && entities[0][0x3b] && entities[1][0x3b] && entities[3][0x3b]);
    assert(!entities[2][0x3b] && !entities[4][0x3b]);
    restore_host_racers(3,50);assert(ownership_calls==3);
    entities[0][0x3b]=0; /* A delayed rejoin can reclaim the AI again. */
    restore_host_racers(3,60);assert(ownership_calls==4);
    entities[0][0x3b]=0;
    *(int *)(connection+0x118)=2;restore_host_racers(3,60);assert(ownership_calls==4);
    *(int *)(connection+0x118)=1;restore_host_racers(3,100);assert(ownership_calls==4);
    restore_host_racers(4,60);assert(ownership_calls==4);
    local_race_quitting=1;restore_host_racers(3,60);assert(ownership_calls==4);
    local_race_quitting=0;kart_racing=0;restore_host_racers(3,60);assert(ownership_calls==4);
    race_racers=race_object_root=network_manager=NULL;native_take_racer_ownership=NULL;
    runtime_public_itemless_reset();last_departure_room=last_departure_pid=0;
}

int main(void) {
    test_handoff_without_departure_field();
    test_handoff_roster();
    test_handoff_racer_ownership();
    test_failed_race_join_cleanup();
    test_promoted_host_security();
    test_race_handoff_admission();
    assert(lobby_host_pid("{\"listen_server\":{\"host_address\":\"abc\",\"host_player_id\":1817964973}}") == 1817964973);
    assert(lobby_host_pid("{\"host_player_id\":123,\"listen_server\":{\"host_player_id\":1817964973}}") == 1817964973);
    matchmaking_room=3; frozen_race=1;
    assert(in_network_game());
    matchmaking_room=0; frozen_race=0;
    test_public_content_gate();
    test_ranked_device_and_search_clock();
    test_guest_main_menu_leaves_party();
    test_native_party_menu_return();
    test_guest_menu_return_destination();
    native_deferred_event_create = allocate_input_event;
    native_deferred_event_release = release_input_event;
    test_hof_project_shortcut();
    installed=1;
    capability_next = UINT64_MAX;
    unsetenv("BB_PUBLIC_ITEMLESS"); assert(!runtime_public_itemless_available());
    setenv("BB_PUBLIC_ITEMLESS","1",1); unsetenv("BB_PRIVATE_ARCHIVE_PATCHES");
    assert(!runtime_public_itemless_available());
    setenv("BB_PRIVATE_ARCHIVE_PATCHES","prepared",1); assert(runtime_public_itemless_available());
    setenv("BB_PUBLIC_ITEMLESS","0",1);
    for (int response = 0; response < 4; ++response) {
        status = response == 0 ? 0 : 200;
        snprintf(reply,sizeof reply,"%s", response == 1 ? "{\"itemless_matchmaking\":2}" :
                 response == 2 ? "{}" : "{\"itemless_matchmaking\":1}");
        capability_running = 1;
        capability_worker(NULL);
        assert(!capability_running);
        assert(runtime_public_itemless_available() == (response == 3));
    }
    capability_supported = 0;
    setenv("BB_PUBLIC_ITEMLESS","1",1);
    snprintf(reply,sizeof reply,"ok"); status=200; requests=0;

    /* The native map-vote handler runs on the game's thread: it never asks the server (a slow
     * answer froze the game at 00), it reads the room-alive thread's latest answer. With none,
     * the race goes ahead; with a short room, it says so (the report goes on a thread). */
    matchmaking_room=77; joined_ranked=1;
    int before_short=requests;
    assert(runtime_public_ranked_short()==0);
    assert(requests==before_short);
    matchmaking_room=0; joined_ranked=0;
    assert(runtime_public_ranked_short()==0);
    matchmaking_room=78; joined_ranked=1;
    pthread_mutex_lock(&ranked_clock_lock);
    ranked_clock_room=78; ranked_clock_ready=-1; ranked_clock_players=2; ranked_clock_at=check_clock();
    pthread_mutex_unlock(&ranked_clock_lock);
    ranked_min_seen=3;
    assert(runtime_public_ranked_short()==1);
    assert(runtime_public_ranked_short()==1);   /* known for this room: no second report */
    for (int wait = 0; wait < 200 && requests == before_short; ++wait) usleep(5000);   /* the report's thread */
    assert(requests==before_short+1);
    /* The server counts its lobby connections, which drop while a player is still in the game: its
     * count below the minimum must not dissolve a room the game's own list holds full (20/11, room
     * 23: WAITING 2/3, the race never started); a room the game's list also holds short is still
     * reported short. */
    matchmaking_room=79; joined_ranked=1;
    pthread_mutex_lock(&ranked_clock_lock);
    ranked_clock_room=79; ranked_clock_ready=-1; ranked_clock_players=2; ranked_clock_at=check_clock();
    pthread_mutex_unlock(&ranked_clock_lock);
    room_players=3;
    assert(runtime_public_ranked_short()==0);
    matchmaking_room=80; joined_ranked=1;
    pthread_mutex_lock(&ranked_clock_lock);
    ranked_clock_room=80; ranked_clock_ready=-1; ranked_clock_players=2; ranked_clock_at=check_clock();
    pthread_mutex_unlock(&ranked_clock_lock);
    room_players=1;
    assert(runtime_public_ranked_short()==1);
    for (int wait = 0; wait < 200 && requests == before_short+1; ++wait) usleep(5000);   /* the report's thread */
    assert(requests==before_short+2);
    room_players=0;
    matchmaking_room=0; joined_ranked=0;
    assert(runtime_public_ranked_short()==0);
    ranked_min_seen=6;

    native_search=send_search; native_register=send_info; native_notice=show;
    network_manager=&test_network; native_stop_matchmaking=stop_matchmaking;
    native_return_to_lobby=return_to_lobby;
    const char doc[]="{\"members\":[1, 2],\"ruleset_payload\":{\"filter\":{\"playlist_id\":0}}}";
    void *result=NULL;
    runtime_public_itemless_select(0);
    start_search(&result,NULL,doc,sizeof doc,NULL);
    assert(sent==1 && !strcmp(payload,doc) && !runtime_public_itemless_active());
    assert(runtime_public_match_active()); /* Normal search is public before a room exists. */
    runtime_public_itemless_select(1);
    assert(!runtime_public_match_active());
    /* Starting public matchmaking does not depend on an HTTP preflight. */
    int before_requests=requests;
    start_search(&result,NULL,doc,sizeof doc,NULL);
    assert(requests==before_requests && dialogs==0 && stopped==0);
    assert(sent==2 && result && request_nonce && strstr(payload,"\"mode\":1"));
    assert(!runtime_public_itemless_active());
    char b[256]; unsigned char owner[0x960]={0}; uint64_t room=42;
    confirmation_doc(b,sizeof b,room,request_nonce);
    assert(accept(owner,room,b,(unsigned)strlen(b)));
    assert(!runtime_public_itemless_active()); /* native handler must accept the same room */
    memcpy(owner+0x958,&room,8); assert(runtime_public_itemless_active());
    /* No map callback: the same confirmed room stays active across frames/races. */
    for (int i=0;i<100;++i) assert(runtime_public_itemless_active());
    uint64_t other=99;
    memcpy(owner+0x958,&other,8); assert(!runtime_public_itemless_active());
    memcpy(owner+0x958,&room,8); assert(runtime_public_itemless_active());
    native_disbanded=end_event; native_kicked=end_event; native_destroy=destroy_owner;
    disbanded(owner,other); assert(runtime_public_itemless_active());
    kicked(owner,other); assert(runtime_public_itemless_active());
    native_merge=merge_event;
    confirmation_doc(b,sizeof b,other,request_nonce);
    merged(owner,1,room,other,"{}",2,b,(unsigned)strlen(b));
    assert(runtime_public_itemless_active() && confirmed_room==other);
    disbanded(owner,room); assert(runtime_public_itemless_active());
    disbanded(owner,other);
    assert(!runtime_public_itemless_active());
    assert(!accept(owner,other,b,(unsigned)strlen(b))); /* late confirmation after disband */
    memcpy(owner+0x958,&room,8);
    const char info[]="{\"listen_server\":{}}";
    register_client(&result,NULL,info,sizeof info);
    assert(registration_nonce && allow_party);
    confirmation_doc(b,sizeof b,room,registration_nonce);
    /* The leader snapshots this nonce before the follower reloads its native lobby. */
    const uint64_t registered=registration_nonce;
    register_client(&result,NULL,info,sizeof info);
    assert(registration_nonce==registered);
    for (int mode=0;mode<2;++mode) {
        char ranked_doc[160];
        snprintf(ranked_doc,sizeof ranked_doc,"{\"octr_ranked_v1\":\"2/%" PRIu64 "/%d/%016" PRIx64 "\"}",room,mode,registered);
        assert(accept(owner,room,ranked_doc,(unsigned)strlen(ranked_doc)));
        assert(confirmed_ranked);
    }
    assert(accept(owner,room,b,(unsigned)strlen(b))); /* compatible invited party member */
    assert(runtime_public_itemless_active());
    kicked(owner,room); assert(!runtime_public_itemless_active());
    assert(!accept(owner,room,b,(unsigned)strlen(b)));
    runtime_public_itemless_party_ready();
    assert(accept(owner,room,b,(unsigned)strlen(b)));
    destroyed(owner);
    assert(!runtime_public_itemless_active() && room_owner==NULL);
    assert(!accept(owner,room,b,(unsigned)strlen(b)));
    memcpy(owner+0x958,&room,8);
    runtime_public_itemless_party_ready();
    assert(accept(owner,room,b,(unsigned)strlen(b)));
    native_leave=send_leave_observed;leave_saw_active=0;
    leave_lobby(&result,NULL,room);
    assert(leave_saw_active);
    assert(!runtime_public_itemless_active());
    assert(!accept(owner,room,b,(unsigned)strlen(b)));
    runtime_public_itemless_update();
    start_search(&result,NULL,doc,sizeof doc,NULL);
    confirmation_doc(b,sizeof b,room,request_nonce);
    native_cancel=send_cancel;
    cancel_search(&result,NULL,1,NULL);
    assert(!accept(owner,room,b,(unsigned)strlen(b)));
    runtime_public_itemless_party_ready();
    confirmation_doc(b,sizeof b,room,registration_nonce);
    assert(accept(owner,room,b,(unsigned)strlen(b)));
    runtime_public_itemless_reset(); /* the transport's disconnect boundary */
    assert(!runtime_public_itemless_active());
    assert(!accept(owner,room,b,(unsigned)strlen(b)));
    assert(!confirmation(b,room+1,registration_nonce));
    assert(!confirmation(b,room,registration_nonce+1));
    assert(!confirmation("{\"octr_itemless_v1\":\"1/42/1/0000000000000001\"}",42,1));
    assert(!confirmation("{\"octr_itemless_v1\":\"2/42/1/0\"}",42,1));
    assert(!confirmation("{\"nested\":{\"octr_itemless_v1\":\"2/42/1/0000000000000001\"}}",42,1));
    assert(!confirmation("{\"octr_itemless_v1\":\"2/42/1/0000000000000001\",\"octr_itemless_v1\":\"bad\"}",42,1));
    runtime_public_itemless_reset();
    runtime_public_itemless_party_ready();
    assert(accept(owner,43,"{}",2) && !runtime_public_itemless_active());
    *(uint64_t *)(owner + 0x958) = 43;
    assert(runtime_public_match_active()); /* Normal party guests never need an Itemless token. */
    disbanded(owner, 42);
    assert(runtime_public_match_active());
    *(uint64_t *)(owner + 0x958) = 44;
    assert(!runtime_public_match_active());
    *(uint64_t *)(owner + 0x958) = 43;
    kicked(owner, 43);
    assert(!runtime_public_match_active());
    runtime_public_itemless_update();
    test_early_gate();
    test_full_lobby_confirmation();
    test_ranked();
    test_takeover_keeps_ranked();
    test_ranked_button_event();
    test_ranked_pause_quit();
    test_native_return_focus();
    test_timeout_preserves_party_and_mode();
    test_join_waits_for_native_session();
    test_cancelled_and_duplicate_lobby_entries();
    test_hof_native_ready();
    test_ranked_season_gate();
    test_hall_of_fame_button_event();
    assert(allocated_input_events == 0);
    test_installed_hooks("1277e00");
    test_installed_hooks("1277df0");
    /* The worker can be stuck in HTTP while repeated frame checks return. */
    early=0; status=200; snprintf(reply,sizeof reply,"ok");
    await_check();
    alarm(5);
    pthread_mutex_lock(&http_lock);
    block_get=1; entered_get=0;
    pthread_mutex_unlock(&http_lock);
    const char *target="/ryunet/itemless-party?name=LocalHost&members=91,92";
    char response[512];
    assert(compatibility_check(target,response,sizeof response)==-1);
    pthread_mutex_lock(&http_lock);
    while (!entered_get) pthread_cond_wait(&http_condition,&http_lock);
    pthread_mutex_unlock(&http_lock);
    for (int i=0;i<100;++i) assert(compatibility_check(target,response,sizeof response)==-1);
    /* A slow or expired HTTP check must never cancel public matchmaking. */
    party_manager=NULL; /* the party tests above pointed at their own stack managers */
    native_search=send_search;
    runtime_public_itemless_select(1);
    int searches=sent, notices=dialogs, stops=stopped, http_requests=requests;
    start_search(&result,NULL,doc,sizeof doc,NULL);
    runtime_public_itemless_update();
    assert(result && sent==searches+1 && !rejection[0]);
    assert(dialogs==notices && stopped==stops && requests==http_requests);

    pthread_mutex_lock(&http_lock);
    block_get=0; pthread_cond_broadcast(&http_condition);
    pthread_mutex_unlock(&http_lock);
    await_check();
    assert(compatibility_check(target,response,sizeof response)==-1); /* old session response discarded */
    await_check();
    assert(compatibility_check(target,response,sizeof response)==200);
    pthread_mutex_lock(&check_lock);
    check_until=0;
    pthread_mutex_unlock(&check_lock);
    http_requests=requests;
    start_search(&result,NULL,doc,sizeof doc,NULL);
    runtime_public_itemless_update();
    assert(result && sent==searches+2 && !rejection[0]);
    assert(dialogs==notices && stopped==stops && requests==http_requests);
    runtime_public_itemless_reset();

    native_peer_leave_original=peer_leave_mock;
    matchmaking_room=74;
    assert(peer_leave_message(NULL)==1 && peer_leave_calls==0);
    kart_racing=1;
    assert(peer_leave_message(NULL)==7 && peer_leave_calls==1);
    kart_racing=0;
    matchmaking_room=0;
    unsigned char network_for_quit[0x80]={0}, race_for_quit[0x28]={0}, replication_for_quit[0x18]={0};
    void *network_pointer=network_for_quit;
    void *race_pointer=race_for_quit;
    memcpy(network_for_quit+0x78,&race_pointer,sizeof race_pointer);
    void *replication_pointer=replication_for_quit;
    memcpy(race_for_quit+0x20,&replication_pointer,sizeof replication_pointer);
    network_manager=&network_pointer;
    native_race_switch_original=race_switch_mock;
    native_set_race_connection=set_race_connection_mock;
    left_room_pending=74;
    lobby_open=0;
    kart_racing=1;
    local_race_quitting=0;
    race_connection_switch(race_for_quit,NULL);
    assert(race_switch_calls==0);
    kart_racing=0; /* Loading after a race frame must not drop the room's connection. */
    had_public_race=0;
    race_connection_switch(race_for_quit,NULL);
    assert(race_switch_calls==1);
    had_public_race=1;
    race_connection_switch(race_for_quit,NULL);
    assert(race_switch_calls==1);
    lobby_open=1;
    memcpy(race_for_quit+0x18,&network_pointer,sizeof network_pointer);
    race_connection_switch(race_for_quit,NULL);
    assert(race_switch_calls==2); /* Back in the lobby: native teardown must run. */
    lobby_open=0;
    local_race_quitting=1;
    race_connection_switch(race_for_quit,NULL);
    assert(race_switch_calls==3); /* Intentional race exit also releases the native roster. */
    memset(race_for_quit+0x18,0,sizeof network_pointer);
    race_switch_calls=1;
    kart_racing=1;
    local_race_quitting=1;
    race_connection_switch(race_for_quit,NULL);
    assert(race_switch_calls==1);
    void *stale_connection=(void *)0x1111;
    memcpy(replication_for_quit+0x10,&stale_connection,sizeof stale_connection);
    local_race_quitting=0;
    left_room_pending=0;
    had_public_race=1;
    race_connection_switch(race_for_quit,NULL);
    memcpy(&stale_connection,replication_for_quit+0x10,sizeof stale_connection);
    assert(!stale_connection && race_switch_calls==1);
    stale_connection=(void *)0x2222;
    memcpy(replication_for_quit+0x10,&stale_connection,sizeof stale_connection);
    network_pointer=NULL; /* Reset detaches the race before clearing its connection. */
    race_connection_switch(race_for_quit,NULL);
    memcpy(&stale_connection,replication_for_quit+0x10,sizeof stale_connection);
    assert(!stale_connection && race_switch_calls==1);
    stale_connection=(void *)0x3333;
    memcpy(replication_for_quit+0x10,&stale_connection,sizeof stale_connection);
    left_room_pending=74;
    race_connection_switch(race_for_quit,NULL);
    memcpy(&stale_connection,replication_for_quit+0x10,sizeof stale_connection);
    assert(!stale_connection && race_switch_calls==1);
    replication_pointer=NULL;
    memcpy(race_for_quit+0x20,&replication_pointer,sizeof replication_pointer);
    race_connection_switch(race_for_quit,NULL);
    assert(race_switch_calls==1);
    kart_racing=0;
    left_room_pending=0;
    had_public_race=0;

    alarm(0);
    puts("Public Itemless request, party rejection, confirmation and lifecycle tests passed");
}
