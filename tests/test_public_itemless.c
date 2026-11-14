#define SACREDFIRE_RELEASE "v9.9.9"
#include "../src/runtime_public_itemless.c"
#include <assert.h>
#include <unistd.h>
static int sent, dialogs, status = 200, stopped, returned, early, forwarded, requests, private_room;
static int private_speed, private_cup, private_teams, modified_difficulty, selector_left_calls;
static int party_guest=1;
int runtime_room_mode_following(void) { return party_guest; }
static int frozen_race;
int runtime_room_mode_race_frozen(void) { return frozen_race; }
static void *focus_test_button;
static unsigned synthetic_signal;
static int hof_opens;
void runtime_hof_open(void) { ++hof_opens; }
void runtime_hof_close(void) {}
static int hof_input_handled, hof_input_signal;
int runtime_hof_input(void *placeable, unsigned signal) { (void)placeable; hof_input_signal = signal; return hof_input_handled; }
static int ranked_status_state = 1;
int runtime_ranked_status(int fresh) { (void)fresh; return ranked_status_state; }
int runtime_private_difficulty_modified(void) { return modified_difficulty; }
int runtime_online_mode_private_cup(void) { return private_cup; }
int runtime_online_mode_private_teams(void) { return private_teams; }
int runtime_speed_mode_enabled(void) { return private_speed; }
int runtime_online_mode_in_private(void) { return private_room; }
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
static char payload[4096], message[512], reply[512] = "ok";
const char *runtime_np_get_online_name(void) { return "LocalHost"; }
int runtime_ryunet_request(const char *method, const char *target, char *body, size_t size) {
    ++requests;
    if (!strcmp(target,"/ryunet/theme")) {
        snprintf(body,size,"%s",reply); return status;
    }
    if (strstr(target,"/ryunet/matchmaking-status?")) { snprintf(body,size,"%s",reply); return status; }
    if (!strcmp(method,"POST")) {
        assert(strstr(target,"/ryunet/itemless-client?name=LocalHost&version=1&nonce="));
        assert(strstr(target,"&release=" SACREDFIRE_RELEASE));
        assert(strstr(target,"&ranked=1"));
        assert(strstr(target,ranked_device()) && strlen(ranked_device())==64);
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
static void ABI merge_event(void *self, uint64_t search, uint64_t from, uint64_t to,
                            const char *host, unsigned hn, const char *doc, unsigned dn) {
    (void)search; (void)from; (void)host; (void)hn; (void)doc; (void)dn;
    memcpy((unsigned char *)self + 0x958, &to, 8);
}
static void confirmation_doc(char *out, size_t n, uint64_t room, uint64_t token) {
    snprintf(out,n,"{\"lobby_id\":%" PRIu64 ",\"octr_itemless_v1\":\"1/%" PRIu64 "/1/%016" PRIx64 "\"}",room,room,token);
}
static void test_full_lobby_confirmation(void) {
    /* Eight solo searches contribute eight request and eight registration tokens. */
    char doc[512];
    size_t n = (size_t)snprintf(doc, sizeof doc, "{\"octr_itemless_v1\":\"1/42/1/");
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
static int native_party_teardowns, native_keep_party, native_race_resets, native_lobby_closes;
static void ABI native_close_lobby(int start_again) { assert(!start_again && native_keep_party==1 && native_race_resets);++native_lobby_closes; }
static void ABI native_reset_session(void *self) { (void)self;assert(native_keep_party==1);++native_race_resets; }
static void ABI native_stop_part(void *self) { (void)self; }
static void ABI native_stop_party(void *self) { (void)self; ++native_party_teardowns; }
static void ABI native_stop_session(void *self, int keep_party) { (void)self; native_keep_party=keep_party; }
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
    /* Both reset branches unwind the stolen prologue; quit does the same. */
    memcpy(im+0xa18223,"\x58\x5b\x5d\xc3",4);
    memcpy(im+0xa182b6,"\x58\x5b\x5d\xc3",4);
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
    assert(runtime_public_itemless_start(im,size));
    assert(im[stop_quit_site]==0xe8); /* Do not alter an unrelated native sync-stop wrapper. */
    int resets=native_race_resets,closes=native_lobby_closes;
    native_stop_matchmaking(manager);
    assert(native_party_teardowns==before_teardown+1 && native_keep_party==1 && native_race_resets==resets+1 && native_lobby_closes==closes+1);
    assert(!memcmp(im+0xa97a00,stop_native,sizeof stop_native));
    unsigned char trace_party[0x80]={0};
    ((void (ABI *)(void *))(im+0xa18210))(trace_party);
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
    unsigned char owner[0x960]={0}; uint64_t room=42; char doc2[128];
    memcpy(owner+0x958,&room,8);
    confirmation_doc(doc2,sizeof doc2,room,registration_nonce);
    assert(accept(owner,room,doc2,(unsigned)strlen(doc2)));
    assert(runtime_public_itemless_active());
    ((void (ABI *)(void *))(im+0xa14550))(owner);
    assert(!runtime_public_itemless_active() && room_owner==NULL);
    munmap(im,size);
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
    if (!selected || rejection[0]) return;
    expire_check(); char body[512];
    (void)compatibility_check("/ryunet/ranked-party?name=LocalHost&members=1,2&mode=1",body,sizeof body);
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
    /* The ranked check now waits inline instead of bouncing the player with a notice. */
    start_search(&result, NULL, doc, sizeof doc, NULL);
    assert(result && sent == before + 1 && !rejection[0]);
    assert(strstr(payload, "\"octr_ranked\"") && !strstr(payload, "\"octr_itemless\""));
    assert(strstr(payload, "\"mode\":0"));
    snprintf(confirmation_doc, sizeof confirmation_doc,
             "{\"octr_ranked_v1\":\"1/91/0/%016" PRIx64 "\"}", request_nonce);
    assert(accept(owner, room, confirmation_doc, (unsigned)strlen(confirmation_doc)));
    assert(runtime_ranked_active() && !runtime_public_itemless_active());
    runtime_public_itemless_select(1);
    prepare_public();
    start_search(&result, NULL, doc, sizeof doc, NULL);
    assert(result && strstr(payload, "\"octr_ranked\"") && strstr(payload, "\"octr_itemless\""));
    snprintf(confirmation_doc, sizeof confirmation_doc,
             "{\"octr_ranked_v1\":\"1/91/1/%016" PRIx64 "\",\"octr_itemless_v1\":\"1/91/1/%016" PRIx64 "\"}",
             request_nonce, request_nonce);
    assert(accept(owner, room, confirmation_doc, (unsigned)strlen(confirmation_doc)));
    assert(runtime_ranked_active() && runtime_public_itemless_active());
int mode_before_ui_reset = -1;
selected = 0;
assert(runtime_public_itemless_lobby_mode() == 1);
assert(runtime_ranked_session(NULL, &mode_before_ui_reset) && mode_before_ui_reset == 1);
selected = 1;
    runtime_public_itemless_reset();
    status = 404;
    expire_check();
    int before_unsupported = sent;
    start_search(&result, NULL, doc, sizeof doc, NULL);
    assert(!result && sent == before_unsupported && strstr(rejection, "Ranked server"));
    rejection[0] = 0;
    status = 200;
    ranked_selected = 0;
    runtime_public_itemless_select(0);
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
    menu_party_returned=1;native_return_focus(root,0);assert(!menu_party_returned);
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
static void test_ranked_season_gate(void) {
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

static int entry_joins,entry_creates;
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
    joined(owner,3,73,"",0,"{}",2);joined(owner,3,73,"",0,"{}",2);
    assert(entry_joins==1);
    /* A guest may take over as host once; repeated create must not reopen map voting. */
    created(owner,3,73,2,"",0,"{}",2);created(owner,3,73,2,"",0,"{}",2);
    assert(entry_creates==2 && entry_joins==1);
    runtime_public_itemless_reset();party_guest=0;matchmaking_search=1;
    pthread_t workers[2];
    for(int i=0;i<2;++i) assert(!pthread_create(&workers[i],NULL,parallel_create,owner));
    for(int i=0;i<2;++i) assert(!pthread_join(workers[i],NULL));
    assert(entry_creates==3);
    party_guest=1;
    runtime_public_itemless_reset();native_join=old_join;native_create=old_create;
}

static int party_returns;
static void ABI return_party_mock(void *party) { assert(party); ++party_returns; }
static void test_native_party_menu_return(void) {
    unsigned char party[0x80]={0},session[0x20]={0},connection[0x120]={0};
    void *party_value=party,*network_value=test_network;
    void **saved_party=party_manager,**saved_network=network_manager;
    void (ABI *saved_return)(void *)=native_party_return;
    void (ABI *saved_stop)(void *)=native_stop_matchmaking;
    int saved_guest=party_guest,old_stops=stopped,old_returns=party_returns;
    party_manager=&party_value;network_manager=&network_value;
    native_party_return=return_party_mock;native_stop_matchmaking=stop_matchmaking;
    *(void **)(party+0x18)=session;*(void **)(session+0x10)=connection;
    *(int *)(connection+0x118)=1;party[0x6c]=1;
    party_guest=0;menu_party_returned=0;
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
    assert(party_returns==old_returns+1 && stopped==old_stops+1);
    assert(*party_manager==party && party[0x6c] && selected && ranked_selected);
    assert(!search_mark && !matchmaking_search && !search_server_active && !matchmaking_room);
    strcpy(reply,"{\"state\":\"searching\",\"elapsed\":99,\"timeout\":180,\"range\":400,\"ranked\":true}");
    search_poll(job);assert(!search_mark && !search_server_active);
    int elapsed,timeout,range,ranked;
    (void)runtime_matchmaking_view(&elapsed,&timeout,&range,&ranked);
    int polls=requests;update_search_clock();
    assert(requests==polls && !search_poll_running);
    runtime_public_itemless_menu_return();assert(party_returns==old_returns+1 && stopped==old_stops+1);
    /* A new lobby can notify again; a follower never broadcasts a host return. */
    menu_party_returned=0;runtime_public_itemless_menu_return();assert(party_returns==old_returns+2);
    *(int *)(connection+0x118)=2;party_guest=1;menu_party_returned=0;
    runtime_public_itemless_menu_return();assert(party_returns==old_returns+2);
    matchmaking_search=1;search_mark=check_clock();search_elapsed_base=180;
    int attempts=timeout_attempts;runtime_public_itemless_update();
    assert(timeout_attempts==attempts && search_timeout_pending);
    runtime_public_itemless_reset();
    party_guest=saved_guest;party_manager=saved_party;network_manager=saved_network;
    native_party_return=saved_return;native_stop_matchmaking=saved_stop;
    stopped=old_stops;selected=ranked_selected=0;strcpy(reply,"ok");
}

static int native_race_party_returns;
static unsigned char ABI race_party_return_mock(void *sender,void *message,void *context) {
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
    assert(received_party_return((void *)1,(void *)2,(void *)3)==1);
    assert(native_race_party_returns==1 && timeout_attempts==old_attempts+5);
    frozen_race=0;timeout_ready=old_ready;timeout_attempts=old_attempts;
    stopped=old_stops;returned=old_returns;
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
    struct SearchPoll *job=calloc(1,sizeof *job);
    job->generation=session_generation;job->serial=search_poll_serial;
    strcpy(job->target,"/ryunet/matchmaking-status?name=LocalHost");search_poll(job);
    assert(runtime_matchmaking_view(&elapsed,&timeout,&range,&ranked));
    assert(elapsed==45 && range==1000 && ranked);
    job=calloc(1,sizeof *job);job->generation=session_generation;job->serial=search_poll_serial;
    strcpy(job->target,"/ryunet/matchmaking-status?name=LocalHost");
    runtime_public_itemless_reset();search_poll(job);
    assert(!runtime_matchmaking_view(&elapsed,&timeout,&range,&ranked));
    runtime_public_itemless_reset();
    matchmaking_search=1;search_mark=check_clock();search_elapsed_base=180;search_poll_after=UINT64_MAX;
    update_search_clock();assert(search_timeout_pending && !rejection[0]);
    runtime_public_itemless_reset();rejection[0]=0;snprintf(reply,sizeof reply,"ok");
    party_guest=saved_guest;
}

int main(void) {
    test_ranked_device_and_search_clock();
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
    native_leave=send_leave;
    leave_lobby(&result,NULL,room);
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
    assert(!confirmation("{\"octr_itemless_v1\":\"2/42/1/0000000000000001\"}",42,1));
    assert(!confirmation("{\"octr_itemless_v1\":\"1/42/1/0\"}",42,1));
    assert(!confirmation("{\"nested\":{\"octr_itemless_v1\":\"1/42/1/0000000000000001\"}}",42,1));
    assert(!confirmation("{\"octr_itemless_v1\":\"1/42/1/0000000000000001\",\"octr_itemless_v1\":\"bad\"}",42,1));
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
    test_ranked_button_event();
    test_native_return_focus();
    test_timeout_preserves_party_and_mode();
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

    alarm(0);
    puts("Public Itemless request, party rejection, confirmation and lifecycle tests passed");
}
