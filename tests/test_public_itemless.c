#define SACREDFIRE_RELEASE "v9.9.9"
#include "../src/runtime_public_itemless.c"
#include <assert.h>
#include <unistd.h>
static int sent, dialogs, status = 200, stopped, returned, early, forwarded, requests, private_room;
static int private_speed, private_cup, private_teams, modified_difficulty;
int runtime_private_difficulty_modified(void) { return modified_difficulty; }
int runtime_online_mode_private_cup(void) { return private_cup; }
int runtime_online_mode_private_teams(void) { return private_teams; }
int runtime_speed_mode_enabled(void) { return private_speed; }
int runtime_online_mode_in_private(void) { return private_room; }
static int ranked_labels;
void runtime_online_mode_set_text(void *placeable, const char *text) {
    assert(placeable && !strcmp(text, "RANKED"));
    ++ranked_labels;
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
    if (!strcmp(method,"POST")) {
        assert(strstr(target,"/ryunet/itemless-client?name=LocalHost&version=1&nonce="));
        assert(strstr(target,"&release=" SACREDFIRE_RELEASE));
        assert(strstr(target,"&ranked=1"));
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
    memcpy(im+0xa97a00,"\x55\x48\x89\xe5\x53\x50",6);
    memcpy(im+0xa9cbe0,"\x55\x48\x89\xe5\x53\x50",6);
    memcpy(im+0x24de10,prefix,16);im[0x24de20]=0x48;
    memcpy(im+0xa548e0,prefix,16);im[0xa548f0]=0x38;
    setenv("BB_ONLINE_MODE_HOOK",region,1);
    assert(runtime_public_itemless_start(im,size));
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
static void ABI forward_event(void *self, void *event) {
    (void)self; (void)event; ++forwarded;
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
    void *event[4] = {vtable, event_meta, 0, (void *)8};
    void *behavior_meta[3] = {0, 0, "igGuiVscBehavior"};
    void *(ABI *graph_vtable[])(void *) = {graph_type};
    void *graph[5] = {graph_vtable, behavior_meta, 0, 0, graph_meta};
    /* The Ranked button shares the matchmaking VSC behavior object (one graph instance in both
     * behavior lists), so its Confirm runs the graph: the hook flags the ranked mode. */
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
    int before_forwarded = forwarded;
    placeable_event(button, event);
    assert(ranked_selected && ranked_labels == 1);
    assert(forwarded == before_forwarded + 1); /* only its own dispatch: the shared graph ran natively */
    /* LoseFocus fires during scene teardown, when the shared comp may already be gone. */
    ranked_selected = 0;
    event_meta[2] = (char *)"igGuiEventPlaceableLoseFocus";
    placeable_event(button, event);
    assert(ranked_labels == 1 && !ranked_selected);
    event_meta[2] = (char *)"igGuiEventFocusInputPressed";
    p = NULL; memcpy(button + 0x28, &p, 8);
    placeable_event(button, event);
    assert(!ranked_selected);
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

int main(void) {
    installed=1;
    unsetenv("BB_PUBLIC_ITEMLESS"); assert(!runtime_public_itemless_available());
    setenv("BB_PUBLIC_ITEMLESS","1",1); unsetenv("BB_PRIVATE_ARCHIVE_PATCHES");
    assert(!runtime_public_itemless_available());
    setenv("BB_PRIVATE_ARCHIVE_PATCHES","prepared",1); assert(runtime_public_itemless_available());
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
