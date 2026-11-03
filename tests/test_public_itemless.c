#include "../src/runtime_public_itemless.c"
#include <assert.h>
static int sent, dialogs, status = 200, stopped, returned, early, forwarded, requests, private_room;
static int private_speed, private_cup, private_teams, modified_difficulty;
int runtime_private_difficulty_modified(void) { return modified_difficulty; }
int runtime_online_mode_private_cup(void) { return private_cup; }
int runtime_online_mode_private_teams(void) { return private_teams; }
int runtime_speed_mode_enabled(void) { return private_speed; }
int runtime_online_mode_in_private(void) { return private_room; }
static void *test_network=(void *)1;
static void ABI stop_matchmaking(void *self) { assert(self==test_network); ++stopped; }
static void ABI return_to_lobby(void) { assert(stopped==returned+1); ++returned; }
static char expected_ids[192];
static char payload[4096], message[512], reply[512] = "ok";
const char *runtime_np_get_online_name(void) { return "LocalHost"; }
int runtime_ryunet_request(const char *method, const char *target, char *body, size_t size) {
    ++requests;
    if (!strcmp(method,"POST")) {
        assert(strstr(target,"/ryunet/itemless-client?name=LocalHost&version=1&nonce="));
        snprintf(body,size,"ok"); return 200;
    }
    assert(!strcmp(method,"GET"));
    assert(strstr(target,"/ryunet/itemless-party?name=LocalHost&members=") ||
           strstr(target,"/ryunet/private-party?name=LocalHost&feature="));
    if (early) assert(!strcmp(strstr(target,"&members=")+9, expected_ids));
    snprintf(body,size,"%s",strstr(target,"/ryunet/private-party?") && !strcmp(reply,"ok") ? "ok-private-v1" : reply); return status;
}
static void ABI show(const char *body, void *options) {
    if (!early) assert(stopped==returned && returned==dialogs+1);
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
static void ABI forward_event(void *self, void *event) {
    (void)self; (void)event; ++forwarded;
}
static void *ABI object_type(void *object) { return *(void **)((char *)object + 8); }
static void *ABI graph_type(void *object) { return *(void **)((char *)object + 0x20); }
static void test_early_gate(void) {
    early=1;
    native_placeable_event=forward_event;
    void *(ABI *vtable[])(void *) = {object_type};
    void *event_meta[3]={0,0,"igGuiEventFocusInputPressed"};
    void *graph_meta[3]={0,0,"Scripts.Graph.Octane_Lobby_ModeSelect_GoToLobbyButton"};
    void *event[4]={vtable,event_meta,0,(void *)8};
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
    for(n=1;n<=8;++n) {
        memcpy(party_roster+12,&n,4);
        size_t len=0;
        for(int j=1;j<=n;++j) len+=(size_t)snprintf(expected_ids+len,sizeof expected_ids-len,"%s%d",j==1?"":",",j);
        status=409;snprintf(reply,sizeof reply,"Update required: OldGuest%d",n);
        int f=forwarded,d=dialogs;
        placeable_event(self,event);
        assert(forwarded==f && dialogs==d+1 && strstr(message,"OldGuest"));
        status=200;strcpy(reply,"ok");
        placeable_event(self,event);assert(forwarded==f+1);
    }
    /* A removed remote member must disappear from the next request. */
    n=1;memcpy(party_roster+12,&n,4);strcpy(expected_ids,"1");
    int accepted=forwarded;
    placeable_event(self,event);assert(forwarded==accepted+1);
    /* Missing party state must not silently fall back to the one local player. */
    q=NULL;memcpy(party+0x30,&q,8);
    int count_before=requests;
    placeable_event(self,event);assert(forwarded==accepted+1 && requests==count_before);
    q=party_roster;memcpy(party+0x30,&q,8);
    /* Base metadata and dynamic graph metadata must both take the early gate. */
    graph[0]=vtable;
    status=409;strcpy(reply,"Update required: OldGuest");
    int f0=forwarded,d0=dialogs;
    placeable_event(self,event);assert(forwarded==f0 && dialogs==d0+1);
    graph[0]=graph_vtable;
    status=503;
    int f=forwarded;placeable_event(self,event);assert(forwarded==f);
    n=9;memcpy(party_roster+12,&n,4);int r=requests;
    placeable_event(self,event);assert(forwarded==f && requests==r);
    graph_meta[2]="Scripts.Graph.OtherButton";
    placeable_event(self,event);assert(forwarded==++f && requests==r);
    graph_meta[2]="Scripts.Graph.Octane_Lobby_ModeSelect_GoToLobbyButton";
    n=2;memcpy(party_roster+12,&n,4);memcpy(records[1]+0x18,records[0]+0x18,8);
    placeable_event(self,event);assert(forwarded==f && requests==r);
    event[3]=(void *)9;placeable_event(self,event);assert(forwarded==++f && requests==r);
    event[3]=(void *)8;private_room=1;placeable_event(self,event);assert(forwarded==++f && requests==r);
    private_room=0;runtime_public_itemless_select(0);placeable_event(self,event);assert(forwarded==++f && requests==r);
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
        placeable_event(self,event);assert(forwarded==f && dialogs==d+1);
        status=200;strcpy(reply,"ok");
        placeable_event(self,event);assert(forwarded==f+1);
    }
    status=503;f=forwarded;placeable_event(self,event);assert(forwarded==f);
    private_speed=0;r=requests;
    placeable_event(self,event);assert(forwarded==f+1 && requests==r);
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
            placeable_event(self,event);assert(forwarded==f && dialogs==d+1);
            status=200;strcpy(reply,"ok");
            placeable_event(self,event);assert(forwarded==f+1);
        }
        status=503;f=forwarded;placeable_event(self,event);assert(forwarded==f);
        private_room=0;r=requests;placeable_event(self,event);
        assert(forwarded==f+1 && requests==r); /* No Team/Cup gate on public menus. */
        private_room=1;
    }
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
    runtime_public_itemless_select(1);
    status=409; strcpy(reply,"Cannot start ITEMLESS. These players need the latest Itemless patch: OldPlayer");
    start_search(&result,NULL,doc,sizeof doc,NULL);
    assert(!result && sent==1 && dialogs==0);
    start_search(&result,NULL,doc,sizeof doc,NULL); /* repeated task cannot escape rejection */
    assert(sent==1 && stopped==0);
    runtime_public_itemless_update();
    runtime_public_itemless_update(); /* cancellation and popup happen once */
    assert(stopped==1 && returned==1);
    assert(!result && sent==1 && dialogs==1 && strstr(message,"OldPlayer"));
    status=503;
    start_search(&result,NULL,doc,sizeof doc,NULL);
    runtime_public_itemless_update();
    assert(!result && sent==1 && dialogs==2);
    status=200; strcpy(reply,"ok");
    start_search(&result,NULL,doc,sizeof doc,NULL);
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
    runtime_public_itemless_update();
    test_early_gate();
    test_full_lobby_confirmation();
    test_installed_hooks("1277e00");
    test_installed_hooks("1277df0");
    puts("Public Itemless request, party rejection, confirmation and lifecycle tests passed");
}
