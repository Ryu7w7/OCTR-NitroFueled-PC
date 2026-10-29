/* System services for an offline, single-user console:
 *   - one local user (id 1) who is logged in but not signed in to PSN;
 *   - network cable unplugged: NetCtl disconnected, sockets unavailable;
 *   - common dialogs complete immediately (no UI is drawn yet);
 *   - the whole package is installed (PlayGo reports every chunk local).
 * Every entry here is an explicit contract; unknown functions still stop. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include "ctr_trophies.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
#else
#include <arpa/inet.h>
#include <sys/stat.h>
#endif

#define USER_ID 1
#define ORBIS_OK 0
#define USER_INVALID_ARGUMENT ((int32_t)0x80960005)
#define USER_NO_EVENT ((int32_t)0x80960007)
#define SYSTEM_NO_EVENT ((int32_t)0x80A10004)
#define SYSTEM_PARAMETER ((int32_t)0x80A10003)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define NET_EINVAL ((int32_t)0x80410116)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define PLAYGO_BAD_HANDLE ((int32_t)0x80B20009)
#define PLAYGO_BAD_POINTER ((int32_t)0x80B2000A)
#define PLAYGO_BAD_SIZE ((int32_t)0x80B2000B)
#define PLAYGO_BAD_CHUNK ((int32_t)0x80B2000C)
#define AUDIO_IN_NOT_OPENED ((int32_t)0x80260109)
#define TROPHY_INVALID ((int32_t)0x80551604)

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int next_id=1;
static int new_id(void) { pthread_mutex_lock(&lock); int id=next_id++; pthread_mutex_unlock(&lock); return id; }
static void note(const char *what) { printf("Runtime: %s\n",what); }

/* ---- UserService ---- */
/* SacredFire: UserService events: login of user 1 at start, then logins/logouts of users 2..4 as
 * the in-game controller ports are assigned and their gamepads connect (runtime_pad.c). */
static struct { int user, login; } user_events[32];
static int user_event_count=0, user_events_seeded;
static void seed_user_events(void) { if (!user_events_seeded) { user_events_seeded=1; user_events[0].user=USER_ID; user_events[0].login=1; user_event_count=1; } }
void runtime_services_user_change(int user,int login) {
    pthread_mutex_lock(&lock);
    seed_user_events();
    if (user_event_count<(int)(sizeof(user_events)/sizeof(*user_events))) { user_events[user_event_count].user=user; user_events[user_event_count].login=login; ++user_event_count; }
    pthread_mutex_unlock(&lock);
    printf("Runtime: user %d %s (controller port %d)\n",user,login ? "logged in" : "logged out",user);
}
/* The local user's name: BB_USER_NAME, else BB_ONLINE_NAME, else "Player 1". */
static const char *default_user_name(void) {
    const char *online = getenv("BB_ONLINE_NAME");
    if (online && *online) return online;
    return "Player 1";
}
static ABI int32_t user_initialize(const void *params) { (void)params; note("UserService initialized (user 1 logged in)"); return 0; }
static ABI int32_t user_terminate(void) { return 0; }
static ABI int32_t user_initial(int32_t *id) { if (!id) return USER_INVALID_ARGUMENT; *id=USER_ID; return 0; }
static ABI int32_t user_list(int32_t *ids) {
    if (!ids) return USER_INVALID_ARGUMENT;
    int n=0;
    ids[n++]=USER_ID;
    for (int user=2;user<=4;++user) if (runtime_pad_user_present(user)) ids[n++]=user;
    while (n<4) ids[n++]=-1;
    return 0;
}
static ABI int32_t user_name(int32_t id,char *name,uint64_t size) {
    if (id<1 || id>4 || !name) return USER_INVALID_ARGUMENT;
    char player[16];
    const char *value=getenv("BB_USER_NAME") ? getenv("BB_USER_NAME") : default_user_name();
    if (id!=USER_ID) { snprintf(player,sizeof player,"Player %d",id); value=player; }
    if (strlen(value)+1>size) return (int32_t)0x8096000a; /* BUFFER_TOO_SHORT */
    strcpy(name,value); return 0;
}
static ABI int32_t user_event(int32_t *event) {
    if (!event) return USER_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    seed_user_events();
    if (!user_event_count) { pthread_mutex_unlock(&lock); return USER_NO_EVENT; }
    const int user=user_events[0].user, login=user_events[0].login;
    memmove(user_events,user_events+1,(size_t)(user_event_count-1)*sizeof(*user_events)); --user_event_count;
    pthread_mutex_unlock(&lock);
    event[0]=login ? 0 : 1; event[1]=user; /* LOGIN / LOGOUT */
    return 0;
}

/* ---- SystemService ---- */
static int language(void) { const char *v=getenv("BB_LANGUAGE"); return v ? atoi(v) : 1; }
static ABI int32_t system_param(int32_t id,int32_t *value) {
    if (!value) return SYSTEM_PARAMETER;
    switch (id) {
    case 1: *value=language(); break;           /* language (1 = English US, 8 = Russian) */
    case 2: *value=1; break;                    /* date format DD/MM/YYYY */
    case 3: *value=1; break;                    /* 24-hour clock */
#ifdef _WIN32
    case 4: *value=(int32_t)(compat_utc_offset(time(NULL))/60); break;
#else
    case 4: { time_t now=time(NULL); struct tm t; localtime_r(&now,&t); *value=(int32_t)(t.tm_gmtoff/60); break; }
#endif
    case 5: *value=0; break;                    /* summer time */
    case 7: *value=0; break;                    /* parental level off */
    case 1000: *value=1; break;                 /* enter button = cross */
    default: fprintf(stderr,"STOP: unsupported system parameter %d\n",id); exit(21);
    }
    return 0;
}
/* ---- Session invitations (RyuNET, accepted in Discord) ----
 * When the player accepts a Discord invite, the launcher writes ryunet_join.txt next to
 * bbport.ini: "session=<PSN session id>\nhost=<nickname>\ntime=<unix seconds>\n". The game
 * hears about it the way a PS4 tells it about an invitation accepted in the system UI:
 * sceSystemServiceGetStatus counts a pending event and sceSystemServiceReceiveEvent hands it
 * SESSION_INVITATION (0x10000002), whose param is SceNpSessionInvitationEventParam
 * (shadPS4 invitation_dialog.h, static_asserted offsets/sizes there):
 *   +0x00 sessionId OrbisNpSessionId (char[45]+term+pad = 48)
 *   +0x30 invitationId OrbisNpInvitationId (char[60]+term = 61)
 *   +0x70 flag u32 (1 = invitation)
 *   +0x74 padding[4]
 *   +0x78 onlineId OrbisNpOnlineId (char[16]+term+dummy = 20) of the local user who accepted
 *   +0x8C userId s32 of the local user who accepted (USER_ID, single-user system)
 *   +0x90 referralOnlineId (24) of the inviter
 *   +0xA8 referralAccountId u64 of the inviter
 * (176 bytes total: the game copies exactly 0xB0, eboot 0xb89d44.) The userId
 * MUST be the accepting local user: with 0 the game reports "not the active
 * user" and retries the join forever. The game drops an invitation that comes
 * while it is busy (0xa99af0), so it is offered again every few seconds until the game joins
 * that session (POST /v1/sessions/<id>/members, see invite_session_joined) or it expires. */
const char *runtime_np_get_online_name(void);
uint64_t runtime_np_calculate_account_id(const char *name);
#define SYSTEM_EVENT_SESSION_INVITATION 0x10000002
/* taken: the game asked for the invited session (GET /v1/sessions/<id>), so it acted on the
 * invitation; it is not offered again (each offer it acts on shows its own message). */
static struct { char session[64]; char host[32]; time_t expires, next_offer; int active, taken; } s_invite;
static pthread_mutex_t s_invite_lock=PTHREAD_MUTEX_INITIALIZER;

static int invite_text_ok(const char *s, size_t max) {
    size_t n=strlen(s);
    if (!n || n>max) return 0;
    for (size_t i=0;i<n;++i) {
        const char c=s[i];
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')) return 0;
    }
    return 1;
}

/* Picks up ryunet_join.txt (at most once a second) and removes it. */
static void poll_invite_file(void) {
    static time_t last_check;
    const time_t now=time(NULL);
    if (now==last_check) return;
    last_check=now;
    char path[600];
    const char *cfg=getenv("BB_CONFIG");
    const char *slash=cfg ? strrchr(cfg,'/') : NULL;
    const char *bslash=cfg ? strrchr(cfg,'\\') : NULL;
    if (bslash>slash) slash=bslash;
    if (slash) snprintf(path,sizeof path,"%.*s/ryunet_join.txt",(int)(slash-cfg),cfg);
    else snprintf(path,sizeof path,"ryunet_join.txt");
    FILE *f=fopen(path,"r");
    if (!f) return;
    char line[256], session[64]="", host[32]="";
    long long written=0;
    while (fgets(line,sizeof line,f)) {
        line[strcspn(line,"\r\n")]=0;
        if (!strncmp(line,"session=",8)) snprintf(session,sizeof session,"%s",line+8);
        else if (!strncmp(line,"host=",5)) snprintf(host,sizeof host,"%s",line+5);
        else if (!strncmp(line,"time=",5)) written=atoll(line+5);
    }
    fclose(f);
    remove(path);
    /* A stale file (a game that never started) must not join a match hours later. */
    if (!invite_text_ok(session,45) || !invite_text_ok(host,16) || written<(long long)now-300 || written>(long long)now+60) {
        printf("Runtime: ignored an invitation file (session='%s' host='%s' age=%llds)\n",session,host,(long long)now-written);
        return;
    }
    pthread_mutex_lock(&s_invite_lock);
    snprintf(s_invite.session,sizeof s_invite.session,"%s",session);
    snprintf(s_invite.host,sizeof s_invite.host,"%s",host);
    s_invite.expires=now+240;  /* time for a game started by the invite to reach the menus */
    s_invite.next_offer=now;
    s_invite.active=1;
    s_invite.taken=0;
    pthread_mutex_unlock(&s_invite_lock);
    printf("Runtime: invitation from %s accepted in Discord: joining session %s\n",host,session);
}

/* An invitation accepted in the menu's Friends tab (runtime_friends.c): offered to the game
 * like one accepted in Discord. */
void runtime_invite_join(const char *host) {
    if (!invite_text_ok(host,16)) return;
    const time_t now=time(NULL);
    pthread_mutex_lock(&s_invite_lock);
    snprintf(s_invite.session,sizeof s_invite.session,"cusa13795-session-%s",host);
    snprintf(s_invite.host,sizeof s_invite.host,"%s",host);
    s_invite.expires=now+240;
    s_invite.next_offer=now;
    s_invite.active=1;
    s_invite.taken=0;
    pthread_mutex_unlock(&s_invite_lock);
    printf("Runtime: invitation from %s accepted in the Friends tab: joining their session\n",host);
}

static int invite_due(void) {
    poll_invite_file();
    const time_t now=time(NULL);
    pthread_mutex_lock(&s_invite_lock);
    if (s_invite.active && now>=s_invite.expires) {
        s_invite.active=0;
        printf("Runtime: invitation from %s expired: the game never joined it\n",s_invite.host);
    }
    const int due=s_invite.active && !s_invite.taken && now>=s_invite.next_offer;
    pthread_mutex_unlock(&s_invite_lock);
    return due;
}

/* The game asked for this session: it acts on the invitation, which is not offered again. */
static void invite_session_taken(const char *session) {
    pthread_mutex_lock(&s_invite_lock);
    if (s_invite.active && !s_invite.taken && session && !strcmp(session,s_invite.session)) {
        s_invite.taken=1;
        printf("Runtime: the game took the invitation from %s (reading its session)\n",s_invite.host);
    }
    pthread_mutex_unlock(&s_invite_lock);
}

/* The game joined a session: an invitation to it is done. */
static void invite_session_joined(const char *path) {
    pthread_mutex_lock(&s_invite_lock);
    if (s_invite.active && path && strstr(path,s_invite.session)) {
        s_invite.active=0;
        printf("Runtime: joined %s's session from the Discord invitation\n",s_invite.host);
    }
    pthread_mutex_unlock(&s_invite_lock);
}

static ABI int32_t system_status(unsigned char *status) {
    if (!status) return SYSTEM_PARAMETER;
    memset(status,0,12); /* event_num=0, no overlay, foreground, normal CPU mode */
    if (invite_due()) *(int32_t *)status=1;
    return 0;
}
static ABI int32_t system_event(void *event) {
    if (!event) return SYSTEM_PARAMETER;
    const time_t now=time(NULL);
    pthread_mutex_lock(&s_invite_lock);
    if (!s_invite.active || s_invite.taken || now<s_invite.next_offer) { pthread_mutex_unlock(&s_invite_lock); return SYSTEM_NO_EVENT; }
    s_invite.next_offer=now+4;
    unsigned char *e=(unsigned char *)event;
    memset(e,0,4+0xB0);
    *(int32_t *)e=SYSTEM_EVENT_SESSION_INVITATION;
    unsigned char *param=e+4;
    snprintf((char *)param+0x00,45+1,"%s",s_invite.session);
    snprintf((char *)param+0x30,60+1,"inv-%s",s_invite.host);
    *(uint32_t *)(param+0x70)=1; /* SCE_NP_SESSION_INVITATION_EVENT_FLAG_INVITATION */
    const char *me=runtime_np_get_online_name();
    snprintf((char *)param+0x78,16+1,"%s",me ? me : "");
    *(int32_t *)(param+0x8C)=(int32_t)USER_ID; /* accepting local user: 0 reads as "not the active user" */
    snprintf((char *)param+0x90,16+1,"%s",s_invite.host); /* referral = inviter */
    *(uint64_t *)(param+0xA8)=runtime_np_calculate_account_id(s_invite.host);
    printf("Runtime: SESSION_INVITATION offered to the game (session %s, from %s)\n",s_invite.session,s_invite.host);
    pthread_mutex_unlock(&s_invite_lock);
    return 0;
}
static ABI int32_t hide_splash(void) { note("SystemService: splash screen hidden"); return 0; }
static ABI int32_t launch_browser(void) { note("SystemService: web browser request ignored (offline)"); return 0; }

#include "runtime_net.h"
#include "runtime_http.h"
#include "runtime_np_online.h"
#include "runtime_friends.h"

/* ---- NetCtl / Net / Http / Ssl ---- */
static ABI int32_t net_init(void) { return runtime_net_init(); }
static ABI int32_t net_term(void) { return runtime_net_term(); }
static ABI int32_t *net_errno_loc(void) { return runtime_net_errno_loc(); }
static ABI int32_t net_pool_create(const char *name,int size,int flags) { (void)name; (void)size; (void)flags; return new_id(); }
static ABI int32_t net_pool_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_socket(const char *name,int domain,int type,int protocol) { return runtime_net_socket(name,domain,type,protocol); }
static ABI int32_t net_connect(int s,const void *addr,uint32_t addrlen) { return runtime_net_connect(s,(const OrbisNetSockaddr*)addr,addrlen); }
static ABI int32_t net_bind(int s,const void *addr,uint32_t addrlen) { return runtime_net_bind(s,(const OrbisNetSockaddr*)addr,addrlen); }
static ABI int32_t net_listen(int s,int backlog) { return runtime_net_listen(s,backlog); }
static ABI int32_t net_accept(int s,void *addr,uint32_t *addrlen) { return runtime_net_accept(s,(OrbisNetSockaddr*)addr,addrlen); }
static ABI int32_t net_send(int s,const void *msg,size_t len,int flags) { return runtime_net_send(s,msg,len,flags); }
static ABI int32_t net_sendto(int s,const void *msg,size_t len,int flags,const void *to,uint32_t tolen) { return runtime_net_sendto(s,msg,len,flags,(const OrbisNetSockaddr*)to,tolen); }
static ABI int32_t net_recv(int s,void *buf,size_t len,int flags) { return runtime_net_recv(s,buf,len,flags); }
static ABI int32_t net_recvfrom(int s,void *buf,size_t len,int flags,void *from,uint32_t *fromlen) { return runtime_net_recvfrom(s,buf,len,flags,(OrbisNetSockaddr*)from,fromlen); }
static ABI int32_t net_setsockopt(int s,int level,int optname,const void *optval,uint32_t optlen) { return runtime_net_setsockopt(s,level,optname,optval,optlen); }
static ABI int32_t net_getsockopt(int s,int level,int optname,void *optval,uint32_t *optlen) { return runtime_net_getsockopt(s,level,optname,optval,optlen); }
static ABI int32_t net_getsockname(int s,void *name,uint32_t *namelen) { return runtime_net_getsockname(s,(OrbisNetSockaddr*)name,namelen); }
static ABI int32_t net_shutdown(int s,int how) { return runtime_net_shutdown(s,how); }
static ABI int32_t net_close(int s) { return runtime_net_socket_close(s); }
static ABI int32_t net_abort(int s,int flags) { return runtime_net_socket_abort(s,flags); }

static ABI int32_t net_epoll_create(const char *name,int flags) { return runtime_net_epoll_create(name,flags); }
static ABI int32_t net_epoll_destroy(int id) { return runtime_net_epoll_destroy(id); }
static ABI int32_t net_epoll_control(int eid,int op,int s,void *event) { return runtime_net_epoll_control(eid,op,s,(OrbisNetEpollEvent*)event); }
static ABI int32_t net_epoll_wait(int eid,void *events,int maxevents,int timeout) { return runtime_net_epoll_wait(eid,(OrbisNetEpollEvent*)events,maxevents,timeout); }
static ABI int32_t net_epoll_abort(int eid,int flags) { return runtime_net_epoll_abort(eid,flags); }

static ABI int32_t net_resolver_create(const char *name,int pool,int flags) { return runtime_net_resolver_create(name,pool,flags); }
static ABI int32_t net_resolver_destroy(int id) { return runtime_net_resolver_destroy(id); }
static ABI int32_t net_resolver_start_ntoa(int rid,const char *hostname,void *addr,int timeout,int retry,int flags) { return runtime_net_resolver_start_ntoa(rid,hostname,(struct in_addr*)addr,timeout,retry,flags); }
static ABI int32_t net_resolver_start_aton(int rid,struct in_addr addr,char *hostname,size_t hostlen,int timeout,int retry,int flags) { return runtime_net_resolver_start_aton(rid,addr,hostname,hostlen,timeout,retry,flags); }
static ABI int32_t net_resolver_abort(int rid,int flags) { return runtime_net_resolver_abort(rid,flags); }

static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t net_pton(int af,const char *src,void *dst) {
    if (af!=2) { return NET_EINVAL; }
    return inet_pton(AF_INET,src,dst);
}
static ABI const char *net_ntop(int af,const void *src,char *dst,uint32_t size) {
    if (af!=2) { return NULL; }
    return inet_ntop(AF_INET,src,dst,size);
}

static ABI int32_t netctl_init(void) { return runtime_netctl_init(); }
static ABI int32_t netctl_term(void) { return runtime_netctl_term(); }
static ABI int32_t netctl_state(int32_t *state) { return runtime_netctl_state(state); }
static ABI int32_t netctl_info(int code,void *info) { return runtime_netctl_info(code,info); }
static ABI int32_t netctl_nat(uint32_t *info) { return runtime_netctl_nat((OrbisNetCtlNatInfo*)info); }
static ABI int32_t netctl_register(void *cb,void *arg,int32_t *cid) { return runtime_netctl_register(cb,arg,cid); }
static ABI int32_t netctl_unregister(int cid) { return runtime_netctl_unregister(cid); }
static ABI int32_t netctl_check(void) { return runtime_netctl_check(); }

static ABI int32_t lib_init_id(void) { return new_id(); }
static ABI int32_t ok_void(void) { return 0; }

/* NpWebApi push events: none are sent. Logged (each filter's first data type, the string its
 * parameters start with). NpToolkit2 listens only for presence ("np:service:presence:onlineStatus"
 * and, for "inGamePresence", "np:service:presence:gameTitleInfo"), not for friend list changes:
 * the game reads its friend list once, at sign-in. */
static ABI int32_t np_push_filter(int32_t lib, int32_t handle, const char *service, uint32_t label,
                                  const char *params, size_t count) {
    const int32_t id=new_id();
    printf("Runtime: NpWebApi push filter %d (lib %d, handle %d, service '%s', label %u, %zu type(s))\n",id,lib,handle,
           service ? service : "",label,count);
    if (params && count) printf("  first type '%.64s'\n",params);
    return id;
}
static ABI int32_t np_push_callback(int32_t user, int32_t filter, void *callback, void *arg) {
    const int32_t id=new_id();
    printf("Runtime: NpWebApi push callback %d (user ctx %d, filter %d, function %p, arg %p)\n",id,user,filter,callback,arg);
    return id;
}

/* HTTP */
static ABI int32_t http_init_abi(void) { return runtime_http_init(); }
static ABI int32_t http_term_abi(void) { return runtime_http_term(); }
static ABI int32_t http_template(int32_t http_ctx,const char *name,int32_t version,int32_t auto_redirect) { return runtime_http_create_template(http_ctx,name,version,auto_redirect); }
static ABI int32_t http_del_template(int32_t tmpl) { return runtime_http_delete_template(tmpl); }
static ABI int32_t http_conn(int32_t tmpl,const char *url,int32_t keep_alive) { return runtime_http_create_connection(tmpl,url,keep_alive); }
static ABI int32_t http_del_conn(int32_t conn) { return runtime_http_delete_connection(conn); }
static ABI int32_t http_req(int32_t conn,int32_t method,const char *path,uint64_t len) { return runtime_http_create_request(conn,method,path,len); }
static ABI int32_t http_req2(int32_t conn,const char *method,const char *path,uint64_t len) { return runtime_http_create_request2(conn,method,path,len); }
static ABI int32_t http_del_req(int32_t req) { return runtime_http_delete_request(req); }
static ABI int32_t http_set_req_len(int32_t req,uint64_t len) { return runtime_http_set_request_content_length(req,len); }
static ABI int32_t http_add_header(int32_t req,const char *name,const char *value,int32_t mode) { return runtime_http_add_request_header(req,name,value,mode); }
static ABI int32_t http_send(int32_t req,const void *data,size_t size) { return runtime_http_send_request(req,data,size); }
static ABI int32_t http_abort(int32_t req) { return runtime_http_abort_request(req); }
static ABI int32_t http_status_code(int32_t req,int32_t *code) { return runtime_http_get_status_code(req,code); }
static ABI int32_t http_resp_len(int32_t req,int32_t *result,uint64_t *len) { return runtime_http_get_response_content_length(req,result,len); }
static ABI int32_t http_read_data(int32_t req,void *data,uint64_t size) { return runtime_http_read_data(req,data,size); }
static ABI int32_t http_all_headers(int32_t req,char **headers,uint64_t *length) { return runtime_http_get_all_response_headers(req,headers,length); }
/* PS4-faithful blob parser: (blob, blob_len, field, &value, &value_len). */
static ABI int32_t http_parse_header(const char *blob,uint64_t blob_len,const char *name,const char **value,uint64_t *length) { return runtime_http_parse_response_header(blob,blob_len,name,value,length); }
static ABI int32_t http_epoll(int32_t ctx,void **handle) { return runtime_http_create_epoll(ctx,handle); }
static ABI int32_t http_destroy_epoll(void *handle) { return runtime_http_destroy_epoll(handle); }
static ABI int32_t http_set_epoll(void *handle,int32_t req,void *user_data) { return runtime_http_set_epoll(handle,req,user_data); }
static ABI int32_t http_unset_epoll(void *handle,int32_t req) { return runtime_http_unset_epoll(handle,req); }
static ABI int32_t http_wait(void *handle,void *events,int32_t max,int64_t timeout) { return runtime_http_wait_request(handle,events,max,timeout); }

/* NP */
static ABI int32_t np_state(int32_t user,int32_t *state) { return runtime_np_get_state(user,state); }
static ABI int32_t np_online_id(int32_t user,void *id) { return runtime_np_get_online_id(user,id); }
static ABI int32_t np_np_id(int32_t user,void *id) { return runtime_np_get_np_id(user,id); }
static ABI int32_t np_account_id(int32_t user,uint64_t *id) { return runtime_np_get_account_id(user,id); }
static ABI int32_t np_user_id_by_acc_id(uint64_t acc, int32_t *user_id) { return runtime_np_get_user_id_by_account_id(acc, user_id); }
static ABI int32_t np_check_plus(int32_t req, const void *param, void *result) { return runtime_np_check_plus(req, param, result); }
static ABI int32_t np_check_avail(void) { return runtime_np_check_availability(); }
static ABI int32_t np_parental_control(int32_t req, void *online_or_user, int8_t *age, void *info) { return runtime_np_parental_control(req, online_or_user, age, info); }
static ABI int32_t np_auth_id_token(int32_t req,const void *param,void *token) { return runtime_np_auth_get_id_token(req,param,token); }
static ABI int32_t np_auth_code(int32_t req,const void *param,void *code) { return runtime_np_auth_get_auth_code(req,param,code,NULL); }
static ABI int32_t np_reg_state_cb(void *cb,void *arg) { return runtime_np_register_state_cb(cb,arg); }
static ABI int32_t np_reg_state_cb_toolkit(void *cb,void *arg) { return runtime_np_register_state_cb_toolkit(cb,arg); }
static ABI int32_t np_unreg_state_cb(void *cb,void *arg) { return runtime_np_unregister_state_cb(cb,arg); }
static ABI int32_t np_unreg_state_cb_toolkit(void *cb,void *arg) { return runtime_np_unregister_state_cb_toolkit(cb,arg); }
static ABI int32_t np_check_cb(void) { return runtime_np_check_callback(); }
static ABI int32_t np_presence_status(const void *id, int32_t *status) { (void)id; if (status) *status = runtime_net_is_online() ? 1 : 0; return 0; }

static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t np_register(void *cb,void *arg) { (void)cb; (void)arg; return new_id(); }
static ABI void np_register_void(void *cb,void *arg) { (void)cb; (void)arg; }
static ABI int32_t np_request(const void *param) { (void)param; return new_id(); }
static ABI int32_t np_request_ctx(int32_t ctx,const void *param) { (void)ctx; (void)param; return new_id(); }
static ABI int32_t np_poll(int32_t request,int32_t *result) { return runtime_np_poll(request, result); }
static ABI int32_t np_compare(const void *a,const void *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return memcmp(a,b,16) ? (int32_t)0x80550609 : 0; /* NP_UTIL NOT_MATCH */
}

/* WebApi */
typedef struct {
    int32_t httpStatus;
    char *pErrorObject;
    uint64_t errorObjectSize;
    uint64_t responseDataSize;
} NpWebApiResponseInfo;

typedef struct {
    int64_t id;
    int in_use;
    int32_t method;
    char group[64];
    char path[256];
    char *response;
    size_t response_len;
    size_t read_pos;
} WebApiReq;

#define MAX_WEBAPI_REQS 32
static WebApiReq s_webapi_reqs[MAX_WEBAPI_REQS];
static pthread_mutex_t s_webapi_lock = PTHREAD_MUTEX_INITIALIZER;

static void get_sessions_dir(char *out, size_t out_len) {
    const char *sess_env = getenv("BB_SESSIONS_DIR");
    if (sess_env && *sess_env) {
        snprintf(out, out_len, "%s", sess_env);
    } else {
        snprintf(out, out_len, "sessions");
    }
#ifdef _WIN32
    _mkdir(out);
#else
    mkdir(out, 0777);
#endif
}

/* ---- Session members (RyuNET) ----
 * POST /v1/sessions/<id>/members joins that session as the local user. The
 * game then reads the session back and requires the local user to be listed
 * as a member, otherwise it reports "not the active user" and retries the
 * join forever (the repeated AVISO dialog). So every join is recorded.
 * Members live next to the session files, one `onlineId|accountId` per line
 * in sessions/members-<sid>.txt, so two local instances share them. Remote
 * players need server-side membership; only users seen joining on this
 * machine are ever recorded here. At most 8 members per session. */
static pthread_mutex_t s_members_lock = PTHREAD_MUTEX_INITIALIZER;
/* The WebAPI invitation session can remain after leaving a private room. It
 * identifies the room, while recent game traffic with a peer distinguishes a
 * multiplayer race from Arcade/Adventure. */
static int s_private_room;
extern int runtime_net_private_lobby_active(void); /* runtime_net.c */
int runtime_private_room_active(void) {
    return __atomic_load_n(&s_private_room, __ATOMIC_ACQUIRE) &&
           runtime_net_private_lobby_active();
}
static void private_room_active(int active) {
    __atomic_store_n(&s_private_room, active, __ATOMIC_RELEASE);
}

static int session_members_path(const char *sid, char *out, size_t out_size) {
    if (!sid || !*sid) return 0;
    size_t n = strlen(sid);
    if (n == 0 || n > 90) return 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = sid[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    char dir[512];
    get_sessions_dir(dir, sizeof(dir));
    snprintf(out, out_size, "%s/members-%s.txt", dir, sid);
    return 1;
}

/* Records the local user as a member of sid. Returns total members. */
static int record_session_member(const char *sid) {
    const char *me = runtime_np_get_online_name();
    if (!me || !*me) return 0;
    char path[512];
    if (!session_members_path(sid, path, sizeof(path))) return 0;
    pthread_mutex_lock(&s_members_lock);
    char seen[8][64];
    int count = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[160];
        while (count < 8 && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *bar = strchr(line, '|');
            if (!line[0] || !bar) continue;
            *bar = 0;
            int dup = 0;
            for (int i = 0; i < count; ++i) dup = dup || strcmp(seen[i], line) == 0;
            if (!dup) snprintf(seen[count++], sizeof(seen[0]), "%s", line);
        }
        fclose(f);
    }
    int present = 0;
    for (int i = 0; i < count; ++i) present = present || strcmp(seen[i], me) == 0;
    if (!present && count < 8) {
        f = fopen(path, "a");
        if (f) {
            fprintf(f, "%s|%llu\n", me, (unsigned long long)runtime_np_calculate_account_id(me));
            fclose(f);
            snprintf(seen[count++], sizeof(seen[0]), "%s", me);
        }
    }
    pthread_mutex_unlock(&s_members_lock);
    runtime_net_trace("webapi: %s joined session %s (%d member(s))", me, sid, count);
    return count;
}

/* Fills names/accs with the recorded members of sid (up to max). Returns count. */
static int read_session_members(const char *sid, char names[][64], uint64_t *accs, int max) {
    char path[512];
    int count = 0;
    if (max <= 0 || !session_members_path(sid, path, sizeof(path))) return 0;
    pthread_mutex_lock(&s_members_lock);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[160];
        while (count < max && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *bar = strchr(line, '|');
            if (!line[0] || !bar) continue;
            *bar = 0;
            snprintf(names[count], 64, "%s", line);
            accs[count] = strtoull(bar + 1, NULL, 10);
            if (accs[count] == 0) accs[count] = runtime_np_calculate_account_id(names[count]);
            count++;
        }
        fclose(f);
    }
    pthread_mutex_unlock(&s_members_lock);
    return count;
}

/* Members recorded for sid excluding host_name (the host is counted separately). */
static int session_join_count(const char *sid, const char *host_name) {
    char names[8][64];
    uint64_t accs[8];
    int n = read_session_members(sid, names, accs, 8);
    int joins = 0;
    for (int i = 0; i < n; ++i) {
        if (!host_name || strcmp(names[i], host_name) != 0) joins++;
    }
    return joins;
}

static void write_active_session(const char *session_id) {
    char dir[512];
    get_sessions_dir(dir, sizeof(dir));
    const char *my_name = runtime_np_get_online_name();
    if (!my_name || !*my_name) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.session", dir, my_name);
    FILE *f = fopen(path, "w");
    if (f) {
        uint64_t acc_id = runtime_np_calculate_account_id(my_name);
        fprintf(f, "sessionId=%s\naccountId=%llu\nonlineId=%s\ntime=%llu\n",
                session_id, (unsigned long long)acc_id, my_name, (unsigned long long)time(NULL));
        fclose(f);
        runtime_net_trace("write_active_session: wrote session '%s' to '%s'", session_id, path);
    }
}

static void remove_active_session(void) {
    private_room_active(0);
    char dir[512];
    get_sessions_dir(dir, sizeof(dir));
    const char *my_name = runtime_np_get_online_name();
    if (!my_name || !*my_name) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.session", dir, my_name);
    remove(path);
    /* Joins recorded for our own session end with it (no stale members). */
    {
        char sid[128], mpath[512];
        snprintf(sid, sizeof(sid), "cusa13795-session-%s", my_name);
        if (session_members_path(sid, mpath, sizeof(mpath))) remove(mpath);
    }
    runtime_net_trace("remove_active_session: removed '%s'", path);
}

/* The running game's title id (param.sfo TITLE_ID): friends are shown in this game (the European
 * edition is CUSA14876, the Americas' CUSA13795). */
static char np_title[16] = "CUSA13795";
void runtime_np_set_title(const char *title_id) {
    if (title_id && *title_id) snprintf(np_title, sizeof(np_title), "%s", title_id);
}

static char *generate_friends_json(void) {
    const char *csv = runtime_np_get_friends_csv();
    char buf[16384];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"friendList\":[");

    int count = 0;
    if (csv && *csv) {
        char copy[1024];
        strncpy(copy, csv, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *token = strtok(copy, ",");
        while (token && pos + 1024 < sizeof(buf)) {
            while (*token == ' ' || *token == '\t') token++;
            char *end = token + strlen(token);
            while (end > token && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
            *end = '\0';
            if (*token) {
                uint64_t acc_id = runtime_np_calculate_account_id(token);
                if (count > 0) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                }
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{"
                    "\"avatarUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\","
                    "\"profilePictureUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\","
                    "\"region\":\"US\","
                    "\"isOfficiallyVerified\":false,"
                    "\"onlineId\":\"%s\","
                    "\"accountId\":\"%llu\","
                    "\"user\":{\"accountId\":\"%llu\",\"onlineId\":\"%s\"},"
                    "\"personalDetail\":{"
                        "\"displayName\":\"%s\","
                        "\"firstName\":\"%s\","
                        "\"middleName\":\"\","
                        "\"lastName\":\"\","
                        "\"profilePictureUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\""
                    "},"
                    "\"presence\":{"
                        "\"onlineStatus\":\"online\","
                        "\"platform\":\"PS4\","
                        "\"incontextInfoList\":[{"
                            "\"platform\":\"PS4\","
                            "\"onlineStatus\":\"online\","
                            "\"gameStatus\":\"In Game\","
                            "\"gameData\":\"\","
                            "\"gameTitleInfo\":{"
                                "\"npTitleId\":\"%s_00\","
                                "\"titleName\":\"Crash Team Racing Nitro-Fueled\""
                            "}"
                        "}]"
                    "}"
                    "}",
                    token, (unsigned long long)acc_id, (unsigned long long)acc_id, token, token, token, np_title);
                count++;
            }
            token = strtok(NULL, ",");
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"totalResults\":%d,\"size\":%d,\"offset\":0,\"limit\":500,\"start\":0}", count, count);
    runtime_net_trace("generate_friends_json: returning %d friend(s)", count);
    return strdup(buf);
}

static char *generate_profiles_json(const char *path) {
    (void)path;
    const char *csv = runtime_np_get_friends_csv();
    const char *my_name = runtime_np_get_online_name();

    char buf[16384];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"profiles\":[");

    int count = 0;

    /* Include local player */
    if (my_name && *my_name) {
        uint64_t my_acc = runtime_np_calculate_account_id(my_name);
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{"
            "\"accountId\":\"%llu\","
            "\"onlineId\":\"%s\","
            "\"region\":\"US\","
            "\"isOfficiallyVerified\":false,"
            "\"user\":{\"accountId\":\"%llu\",\"onlineId\":\"%s\"},"
            "\"relation\":\"me\","
            "\"aboutMe\":\"Crash Team Racing Nitro-Fueled\","
            "\"languagesUsed\":[\"en\"],"
            "\"avatarUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\","
            "\"profilePictureUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\","
            "\"personalDetail\":{"
                "\"displayName\":\"%s\","
                "\"firstName\":\"%s\","
                "\"middleName\":\"\","
                "\"lastName\":\"\","
                "\"profilePictureUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\""
            "}"
            "}",
            (unsigned long long)my_acc, my_name, (unsigned long long)my_acc, my_name, my_name, my_name);
        count++;
    }

    /* Include friends */
    if (csv && *csv) {
        char copy[1024];
        strncpy(copy, csv, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *token = strtok(copy, ",");
        while (token && pos + 1024 < sizeof(buf)) {
            while (*token == ' ' || *token == '\t') token++;
            char *end = token + strlen(token);
            while (end > token && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
            *end = '\0';
            if (*token && (!my_name || strcmp(token, my_name) != 0)) {
                uint64_t acc_id = runtime_np_calculate_account_id(token);
                if (count > 0) {
                    pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                }
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "{"
                    "\"accountId\":\"%llu\","
                    "\"onlineId\":\"%s\","
                    "\"region\":\"US\","
                    "\"isOfficiallyVerified\":false,"
                    "\"user\":{\"accountId\":\"%llu\",\"onlineId\":\"%s\"},"
                    "\"relation\":\"friend\","
                    "\"aboutMe\":\"Crash Team Racing Nitro-Fueled\","
                    "\"languagesUsed\":[\"en\"],"
                    "\"avatarUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\","
                    "\"profilePictureUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\","
                    "\"personalDetail\":{"
                        "\"displayName\":\"%s\","
                        "\"firstName\":\"%s\","
                        "\"middleName\":\"\","
                        "\"lastName\":\"\","
                        "\"profilePictureUrl\":\"https://image.api.playstation.com/trophy/np/default_avatar.png\""
                    "}"
                    "}",
                    (unsigned long long)acc_id, token, (unsigned long long)acc_id, token, token, token);
                count++;
            }
            token = strtok(NULL, ",");
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"size\":%d,\"totalResults\":%d}", count, count);
    runtime_net_trace("generate_profiles_json: returning %d profile(s)", count);
    return strdup(buf);
}

static char *generate_friend_online_id_lists_json(void) {
    const char *csv = runtime_np_get_friends_csv();
    const char *my_name = runtime_np_get_online_name();
    uint64_t my_acc = runtime_np_calculate_account_id(my_name);

    char buf[8192];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"friendOnlineIdLists\":[{\"user\":{\"accountId\":\"%llu\",\"onlineId\":\"%s\"},\"friendOnlineIdList\":[",
        (unsigned long long)my_acc, (my_name && *my_name) ? my_name : "Player");

    int count = 0;
    if (csv && *csv) {
        char copy[1024];
        strncpy(copy, csv, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *token = strtok(copy, ",");
        while (token && pos + 128 < sizeof(buf)) {
            while (*token == ' ' || *token == '\t') token++;
            char *end = token + strlen(token);
            while (end > token && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
            *end = '\0';
            if (*token) {
                if (count > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                pos += snprintf(buf + pos, sizeof(buf) - pos, "\"%s\"", token);
                count++;
            }
            token = strtok(NULL, ",");
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}]}");
    return strdup(buf);
}

static char *invitation_json(const char *host);

static char *generate_invitations_json(void) {
    char dir[512];
    get_sessions_dir(dir, sizeof(dir));
    const char *csv = runtime_np_get_friends_csv();
    const char *my_name = runtime_np_get_online_name();

    char buf[8192];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"invitations\":[");

    int count = 0;
    if (csv && *csv) {
        char copy[1024];
        strncpy(copy, csv, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *token = strtok(copy, ",");
        while (token && pos + 1024 < sizeof(buf)) {
            while (*token == ' ' || *token == '\t') token++;
            char *end = token + strlen(token);
            while (end > token && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
            *end = '\0';
            if (*token && (!my_name || strcmp(token, my_name) != 0)) {
                char path[512];
                snprintf(path, sizeof(path), "%s/%s.session", dir, token);
                FILE *f = fopen(path, "r");
                if (f) {
                    char sid[128] = {0};
                    uint64_t acc_id = 0;
                    char line[256];
                    while (fgets(line, sizeof(line), f)) {
                        if (strncmp(line, "sessionId=", 10) == 0) {
                            char *nl = line + 10;
                            while (*nl && *nl != '\r' && *nl != '\n') nl++;
                            *nl = '\0';
                            strncpy(sid, line + 10, sizeof(sid) - 1);
                        } else if (strncmp(line, "accountId=", 10) == 0) {
                            acc_id = strtoull(line + 10, NULL, 10);
                        }
                    }
                    fclose(f);

                    if (sid[0] == '\0') {
                        snprintf(sid, sizeof(sid), "cusa13795-session-%s", token);
                    }
                    if (acc_id == 0) {
                        acc_id = runtime_np_calculate_account_id(token);
                    }

                    if (count > 0) {
                        pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                    }
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "{"
                        "\"invitationId\":\"inv-%s\","
                        "\"sessionId\":\"%s\","
                        "\"receivedDate\":\"2026-10-14T00:00:00.000Z\","
                        "\"updateDate\":\"2026-10-14T00:00:00.000Z\","
                        "\"expired\":false,"
                        "\"usedFlag\":false,"
                        "\"fromUser\":{"
                            "\"onlineId\":\"%s\","
                            "\"accountId\":\"%llu\""
                        "},"
                        "\"session\":{"
                            "\"sessionId\":\"%s\","
                            "\"sessionName\":\"CTR Private Match\","
                            "\"sessionLockFlag\":false"
                        "}"
                        "}",
                        token, sid, token, (unsigned long long)acc_id, sid);
                    count++;
                }
            }
            token = strtok(NULL, ",");
        }
    }

    /* The invitation accepted in Discord: its host's session file lives on another PC. */
    pthread_mutex_lock(&s_invite_lock);
    char pending_host[32] = "";
    if (s_invite.active) snprintf(pending_host, sizeof(pending_host), "%s", s_invite.host);
    pthread_mutex_unlock(&s_invite_lock);
    char pending_marker[48];
    snprintf(pending_marker, sizeof(pending_marker), "\"inv-%s\"", pending_host);
    if (pending_host[0] && !strstr(buf, pending_marker) && pos + 1024 < sizeof(buf)) {
        char *one = invitation_json(pending_host);
        if (one) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%s", count > 0 ? "," : "", one);
            free(one);
            count++;
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"size\":%d,\"totalResults\":%d}", count, count);
    runtime_net_trace("generate_invitations_json: found %d invitation(s)", count);
    return strdup(buf);
}


static char *generate_friend_sessions_json(void) {
    char dir[512];
    get_sessions_dir(dir, sizeof(dir));
    const char *csv = runtime_np_get_friends_csv();
    const char *my_name = runtime_np_get_online_name();

    char buf[8192];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"sessions\":[");

    int count = 0;
    if (csv && *csv) {
        char copy[1024];
        strncpy(copy, csv, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *token = strtok(copy, ",");
        while (token && pos + 1024 < sizeof(buf)) {
            while (*token == ' ' || *token == '\t') token++;
            char *end = token + strlen(token);
            while (end > token && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
            *end = '\0';
            if (*token && (!my_name || strcmp(token, my_name) != 0)) {
                char path[512];
                snprintf(path, sizeof(path), "%s/%s.session", dir, token);
                FILE *f = fopen(path, "r");
                if (f) {
                    char sid[128] = {0};
                    uint64_t acc_id = 0;
                    char line[256];
                    while (fgets(line, sizeof(line), f)) {
                        if (strncmp(line, "sessionId=", 10) == 0) {
                            char *nl = line + 10;
                            while (*nl && *nl != '\r' && *nl != '\n') nl++;
                            *nl = '\0';
                            strncpy(sid, line + 10, sizeof(sid) - 1);
                        } else if (strncmp(line, "accountId=", 10) == 0) {
                            acc_id = strtoull(line + 10, NULL, 10);
                        }
                    }
                    fclose(f);

                    if (sid[0] == '\0') {
                        snprintf(sid, sizeof(sid), "cusa13795-session-%s", token);
                    }
                    if (acc_id == 0) {
                        acc_id = runtime_np_calculate_account_id(token);
                    }

                    if (count > 0) {
                        pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                    }
                    int total = 1 + session_join_count(sid, token);
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "{"
                        "\"sessionId\":\"%s\","
                        "\"sessionName\":\"CTR Private Match\","
                        "\"sessionCreateTimestamp\":\"2026-10-14T00:00:00.000Z\","
                        "\"availablePlatforms\":[\"PS4\"],"
                        "\"memberCount\":%d,"
                        "\"sessionLockFlag\":false,"
                        "\"friends\":[{"
                            "\"onlineId\":\"%s\","
                            "\"accountId\":\"%llu\","
                            "\"platform\":\"PS4\""
                        "}]"
                        "}",
                        sid, total, token, (unsigned long long)acc_id);
                    count++;
                }
            }
            token = strtok(NULL, ",");
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "],\"start\":0,\"size\":%d,\"totalResults\":%d}", count, count);
    runtime_net_trace("generate_friend_sessions_json: found %d active session(s)", count);
    return strdup(buf);
}

static char *generate_multiuser_sessions_json(void) {
    char dir[512];
    get_sessions_dir(dir, sizeof(dir));
    const char *csv = runtime_np_get_friends_csv();
    const char *my_name = runtime_np_get_online_name();

    char buf[8192];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "{\"users\":[");

    int count = 0;
    if (csv && *csv) {
        char copy[1024];
        strncpy(copy, csv, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';
        char *token = strtok(copy, ",");
        while (token && pos + 1024 < sizeof(buf)) {
            while (*token == ' ' || *token == '\t') token++;
            char *end = token + strlen(token);
            while (end > token && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
            *end = '\0';
            if (*token && (!my_name || strcmp(token, my_name) != 0)) {
                char path[512];
                snprintf(path, sizeof(path), "%s/%s.session", dir, token);
                FILE *f = fopen(path, "r");
                if (f) {
                    char sid[128] = {0};
                    uint64_t acc_id = 0;
                    char line[256];
                    while (fgets(line, sizeof(line), f)) {
                        if (strncmp(line, "sessionId=", 10) == 0) {
                            char *nl = line + 10;
                            while (*nl && *nl != '\r' && *nl != '\n') nl++;
                            *nl = '\0';
                            strncpy(sid, line + 10, sizeof(sid) - 1);
                        } else if (strncmp(line, "accountId=", 10) == 0) {
                            acc_id = strtoull(line + 10, NULL, 10);
                        }
                    }
                    fclose(f);

                    if (sid[0] == '\0') {
                        snprintf(sid, sizeof(sid), "cusa13795-session-%s", token);
                    }
                    if (acc_id == 0) {
                        acc_id = runtime_np_calculate_account_id(token);
                    }

                    if (count > 0) {
                        pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                    }
                    pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "{"
                        "\"accountId\":\"%llu\","
                        "\"sessions\":[{"
                            "\"sessionId\":\"%s\","
                            "\"sessionName\":\"CTR Private Match\","
                            "\"availablePlatforms\":[\"PS4\"],"
                            "\"memberCount\":1,"
                            "\"sessionLockFlag\":false,"
                            "\"priority\":0,"
                            "\"index\":0"
                        "}]"
                        "}",
                        (unsigned long long)acc_id, sid);
                    count++;
                }
            }
            token = strtok(NULL, ",");
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    return strdup(buf);
}

static char *generate_session_info_json(const char *session_id) {
    char host_name[64] = "Player";
    uint64_t acc_id = 0;

    if (session_id && strncmp(session_id, "cusa13795-session-", 18) == 0) {
        strncpy(host_name, session_id + 18, sizeof(host_name) - 1);
        host_name[sizeof(host_name) - 1] = '\0';
    } else {
        const char *my_name = runtime_np_get_online_name();
        if (my_name && *my_name) {
            strncpy(host_name, my_name, sizeof(host_name) - 1);
            host_name[sizeof(host_name) - 1] = '\0';
        }
    }

    char dir[512];
    get_sessions_dir(dir, sizeof(dir));
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.session", dir, host_name);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "accountId=", 10) == 0) {
                acc_id = strtoull(line + 10, NULL, 10);
            }
        }
        fclose(f);
    }
    if (acc_id == 0) {
        acc_id = runtime_np_calculate_account_id(host_name);
    }

    /* members: the host plus everyone recorded by POST .../members. The game
     * requires the local user here, else "not the active user". */
    char mnames[8][64];
    uint64_t maccs[8];
    int nmem = read_session_members(session_id ? session_id : "", mnames, maccs, 8);
    char buf[2048];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{"
        "\"sessionId\":\"%s\","
        "\"sessionName\":\"CTR Private Match\","
        "\"sessionPrivacy\":\"public\","
        "\"sessionType\":\"owner-bind\","
        "\"sessionMaxUser\":8,"
        "\"sessionStatus\":\"in_lobby\","
        "\"sessionLockFlag\":false,"
        "\"availablePlatforms\":[\"PS4\"],"
        /* sessionCreator: an invitation is joined through the session's creator (eboot
         * 0xac4576 reads its accountId from the toolkit's Session; 0 there is "failed to
         * retrieve friend information", 0xac4584), whose Demonware lobby is then looked up. */
        "\"sessionCreator\":{"
            "\"accountId\":\"%llu\","
            "\"onlineId\":\"%s\","
            "\"platform\":\"PS4\""
        "},"
        "\"members\":[{"
            "\"accountId\":\"%llu\","
            "\"onlineId\":\"%s\","
            "\"platform\":\"PS4\""
        "}",
        session_id ? session_id : "cusa13795-session",
        (unsigned long long)acc_id, host_name, (unsigned long long)acc_id, host_name);
    for (int i = 0; i < nmem && pos + 160 < sizeof(buf); ++i) {
        if (strcmp(mnames[i], host_name) == 0) continue;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            ",{"
            "\"accountId\":\"%llu\","
            "\"onlineId\":\"%s\","
            "\"platform\":\"PS4\""
            "}",
            (unsigned long long)maccs[i], mnames[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    return strdup(buf);
}

static ABI int32_t np_webapi_create_ctx(int32_t lib_ctx, void *online_id) { (void)lib_ctx; (void)online_id; return new_id(); }
static ABI int32_t np_webapi_create_ctx_a(int32_t lib_ctx, int32_t user_id) { (void)lib_ctx; (void)user_id; return new_id(); }
static ABI int32_t np_webapi_create_req(int32_t user_ctx, const char *group, const char *path,
                                        int32_t method, const void *param, int64_t *req_id) {
    (void)user_ctx; (void)param;
    int id = new_id();
    pthread_mutex_lock(&s_webapi_lock);
    for (int i = 0; i < MAX_WEBAPI_REQS; i++) {
        if (!s_webapi_reqs[i].in_use) {
            s_webapi_reqs[i].in_use = 1;
            s_webapi_reqs[i].id = id;
            s_webapi_reqs[i].method = method;
            strncpy(s_webapi_reqs[i].group, group ? group : "", sizeof(s_webapi_reqs[i].group) - 1);
            s_webapi_reqs[i].group[sizeof(s_webapi_reqs[i].group) - 1] = '\0';
            strncpy(s_webapi_reqs[i].path, path ? path : "", sizeof(s_webapi_reqs[i].path) - 1);
            s_webapi_reqs[i].path[sizeof(s_webapi_reqs[i].path) - 1] = '\0';
            s_webapi_reqs[i].response = NULL;
            s_webapi_reqs[i].response_len = 0;
            s_webapi_reqs[i].read_pos = 0;
            break;
        }
    }
    pthread_mutex_unlock(&s_webapi_lock);
    if (req_id) *req_id = (int64_t)id;
    runtime_net_trace("sceNpWebApiCreateRequest: group='%s', path='%s', method=%d, req_id=%d",
                      group ? group : "", path ? path : "", method, id);
    return 0;
}

/* One invitation from host to their session "cusa13795-session-<host>", the shape the list
 * (generate_invitations_json) uses. Sessions are named after their host, so another PC's
 * invitation needs no shared file: the Discord invite named the host. */
static char *invitation_json(const char *host) {
    char sid[96], buf[1024];
    snprintf(sid, sizeof(sid), "cusa13795-session-%s", host);
    snprintf(buf, sizeof(buf),
        "{\"invitationId\":\"inv-%s\",\"sessionId\":\"%s\","
        "\"receivedDate\":\"2026-10-14T00:00:00.000Z\",\"updateDate\":\"2026-10-14T00:00:00.000Z\","
        "\"expired\":false,\"usedFlag\":false,"
        "\"fromUser\":{\"onlineId\":\"%s\",\"accountId\":\"%llu\"},"
        "\"session\":{\"sessionId\":\"%s\",\"sessionName\":\"CTR Private Match\",\"sessionLockFlag\":false}}",
        host, sid, host, (unsigned long long)runtime_np_calculate_account_id(host), sid);
    return strdup(buf);
}

/* The <id> in ".../<marker><id>[/?...]", or "" (at most out_size-1 characters). */
static void path_segment_after(const char *path, const char *marker, char *out, size_t out_size) {
    out[0] = '\0';
    const char *p = path ? strstr(path, marker) : NULL;
    if (!p) return;
    p += strlen(marker);
    size_t n = 0;
    while (p[n] && p[n] != '?' && p[n] != '/' && n < out_size - 1) n++;
    memcpy(out, p, n);
    out[n] = '\0';
}

static char *get_webapi_response_for_req(const WebApiReq *req) {
    runtime_net_trace("get_webapi_response_for_req: path='%s', method=%d", req->path, req->method);

    /* GET /v1/users/me/invitations/inv-<host>: one invitation, as after a SESSION_INVITATION event. */
    if (req->method == 0 && strstr(req->path, "/invitations/inv-")) {
        char host[64];
        path_segment_after(req->path, "/invitations/inv-", host, sizeof(host));
        if (host[0]) return invitation_json(host);
    }
    /* PUT on an invitation (e.g. marking it used): nothing to keep. */
    if (req->method == 2 && strstr(req->path, "/invitations/")) {
        return strdup("{}");
    }
    /* POST /v1/sessions/<id>/members -> join that session. Checked before "create", which also
     * POSTs under /v1/sessions and used to answer a join with the joiner's own session. */
    if (req->method == 1 && strstr(req->path, "/v1/sessions/") && strstr(req->path, "/members")) {
        char sid[96];
        path_segment_after(req->path, "/v1/sessions/", sid, sizeof(sid));
        if (sid[0]) {
            record_session_member(sid);
            private_room_active(1);
        }
        invite_session_joined(req->path);
        char resp[160];
        snprintf(resp, sizeof(resp), "{\"sessionId\":\"%s\"}", sid[0] ? sid : "cusa13795-session");
        runtime_net_trace("webapi: joined session -> %s", resp);
        return strdup(resp);
    }

    if (strstr(req->path, "friendList")) {
        return generate_friends_json();
    }
    if (strstr(req->path, "friendOnlineIdLists")) {
        return generate_friend_online_id_lists_json();
    }
    if (strstr(req->path, "profile") || strstr(req->path, "profiles")) {
        return generate_profiles_json(req->path);
    }
    if (strstr(req->path, "friends/sessions")) {
        return generate_friend_sessions_json();
    }
    if (strstr(req->path, "@users/sessions")) {
        return generate_multiuser_sessions_json();
    }
    if (strstr(req->path, "entitlements")) {
        return strdup("{\"entitlements\":[],\"totalResults\":0,\"start\":0,\"size\":0}");
    }
    if (strstr(req->path, "invitations") && req->method == 1) {
        return strdup("{\"invitationId\":\"inv-sent\"}");
    }
    if (strstr(req->path, "invitations")) {
        return generate_invitations_json();
    }
    /* POST /v1/sessions -> create session (method 1 is POST in OrbisNpWebApiHttpMethod) */
    if (req->method == 1 && strstr(req->path, "/v1/sessions")) {
        const char *my_name = runtime_np_get_online_name();
        char sid[128];
        snprintf(sid, sizeof(sid), "cusa13795-session-%s", (my_name && *my_name) ? my_name : "host");
        write_active_session(sid);
        private_room_active(1);
        char resp[256];
        snprintf(resp, sizeof(resp), "{\"sessionId\":\"%s\"}", sid);
        runtime_net_trace("webapi: created session -> %s", resp);
        return strdup(resp);
    }
    /* DELETE /v1/sessions -> delete session (method 3 is DELETE) */
    if (req->method == 3 && strstr(req->path, "/v1/sessions")) {
        remove_active_session();
        return strdup("{}");
    }
    /* POST /v1/sessions/<id>/members -> join session */
    if (strstr(req->path, "/v1/sessions/") && strstr(req->path, "/members")) {
        char sid[96];
        path_segment_after(req->path, "/v1/sessions/", sid, sizeof(sid));
        if (sid[0]) {
            record_session_member(sid);
            private_room_active(1);
        }
        invite_session_joined(req->path);
        return strdup("{\"sessionId\":\"cusa13795-session\"}");
    }
    /* GET /v1/sessions/<id> */
    if (strstr(req->path, "/v1/sessions/")) {
        const char *p = strstr(req->path, "/v1/sessions/") + 13;
        char sid[128] = {0};
        size_t si = 0;
        while (p[si] && p[si] != '?' && p[si] != '/' && si < sizeof(sid) - 1) {
            sid[si] = p[si];
            si++;
        }
        sid[si] = '\0';
        invite_session_taken(sid);
        return generate_session_info_json(sid);
    }
    /* generic /v1/sessions query */
    if (strstr(req->path, "/v1/sessions")) {
        return generate_friend_sessions_json();
    }
    if (strstr(req->path, "friends")) {
        return generate_friends_json();
    }
    return strdup("{}");
}

static ABI int32_t np_webapi_send_req(int64_t req_id, const void *data, uint64_t size) {
    (void)data; (void)size;
    pthread_mutex_lock(&s_webapi_lock);
    for (int i = 0; i < MAX_WEBAPI_REQS; i++) {
        if (s_webapi_reqs[i].in_use && s_webapi_reqs[i].id == req_id) {
            if (!s_webapi_reqs[i].response) {
                s_webapi_reqs[i].response = get_webapi_response_for_req(&s_webapi_reqs[i]);
                s_webapi_reqs[i].response_len = strlen(s_webapi_reqs[i].response);
                s_webapi_reqs[i].read_pos = 0;
            }
            runtime_net_trace("sceNpWebApiSendRequest: req_id=%lld -> prepared %zu bytes response",
                              (long long)req_id, s_webapi_reqs[i].response_len);
            break;
        }
    }
    pthread_mutex_unlock(&s_webapi_lock);
    return 0;
}

static ABI int32_t np_webapi_send_req2(int64_t req_id, const void *data, uint64_t size, NpWebApiResponseInfo *resp) {
    (void)data; (void)size;
    pthread_mutex_lock(&s_webapi_lock);
    for (int i = 0; i < MAX_WEBAPI_REQS; i++) {
        if (s_webapi_reqs[i].in_use && s_webapi_reqs[i].id == req_id) {
            if (!s_webapi_reqs[i].response) {
                s_webapi_reqs[i].response = get_webapi_response_for_req(&s_webapi_reqs[i]);
                s_webapi_reqs[i].response_len = strlen(s_webapi_reqs[i].response);
                s_webapi_reqs[i].read_pos = 0;
            }
            if (resp) {
                resp->httpStatus = 200;
                resp->responseDataSize = s_webapi_reqs[i].response_len;
            }
            runtime_net_trace("sceNpWebApiSendRequest2: req_id=%lld -> status 200, size=%zu",
                              (long long)req_id, s_webapi_reqs[i].response_len);
            pthread_mutex_unlock(&s_webapi_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&s_webapi_lock);
    if (resp) { resp->httpStatus = 200; resp->responseDataSize = 0; }
    return 0;
}

static ABI int32_t np_webapi_add_header(int64_t req_id, const char *name, const char *val) {
    (void)req_id; (void)name; (void)val;
    return 0;
}

static ABI int32_t np_webapi_read_data(int64_t req_id, void *data, uint64_t size) {
    pthread_mutex_lock(&s_webapi_lock);
    for (int i = 0; i < MAX_WEBAPI_REQS; i++) {
        if (s_webapi_reqs[i].in_use && s_webapi_reqs[i].id == req_id) {
            if (s_webapi_reqs[i].response && data && size > 0) {
                size_t avail = s_webapi_reqs[i].response_len - s_webapi_reqs[i].read_pos;
                size_t to_copy = size < avail ? (size_t)size : avail;
                if (to_copy > 0) {
                    memcpy(data, s_webapi_reqs[i].response + s_webapi_reqs[i].read_pos, to_copy);
                    s_webapi_reqs[i].read_pos += to_copy;
                }
                runtime_net_trace("sceNpWebApiReadData: req_id=%lld, read %zu bytes (pos=%zu/%zu)",
                                  (long long)req_id, to_copy, s_webapi_reqs[i].read_pos, s_webapi_reqs[i].response_len);
                pthread_mutex_unlock(&s_webapi_lock);
                return (int32_t)to_copy;
            }
            pthread_mutex_unlock(&s_webapi_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&s_webapi_lock);
    return 0;
}

static ABI int32_t np_webapi_delete_req(int64_t req_id) {
    pthread_mutex_lock(&s_webapi_lock);
    for (int i = 0; i < MAX_WEBAPI_REQS; i++) {
        if (s_webapi_reqs[i].in_use && s_webapi_reqs[i].id == req_id) {
            if (s_webapi_reqs[i].response) {
                free(s_webapi_reqs[i].response);
                s_webapi_reqs[i].response = NULL;
            }
            s_webapi_reqs[i].in_use = 0;
            break;
        }
    }
    pthread_mutex_unlock(&s_webapi_lock);
    return 0;
}

static ABI int32_t np_webapi_get_status(int64_t req_id, int32_t *code) {
    (void)req_id;
    if (code) *code = 200;
    return 0;
}

static ABI int32_t np_webapi_get_header(int64_t req_id, const char *name, char *val, uint64_t size) {
    (void)req_id; (void)name;
    if (val && size > 0) val[0] = '\0';
    return 0;
}

static ABI int32_t np_webapi_get_header_len(int64_t req_id, const char *name, uint64_t *len) {
    (void)req_id; (void)name;
    if (len) *len = 0;
    return 0;
}

static ABI int32_t np_webapi_pool_stats(int32_t lib_ctx, void *stats) {
    (void)lib_ctx;
    if (stats) memset(stats, 0, 32);
    return 0;
}

static ABI int32_t np_webapi_parse_npid(const char *json, void *npid) {
    if (!npid) return NP_INVALID_ARGUMENT;
    memset(npid, 0, 32);
    if (!json) return 0;
    const char *p = strstr(json, "\"onlineId\":");
    if (!p) p = strstr(json, "\"onlineId\" :");
    if (!p) p = strstr(json, "\"npId\":");
    if (!p) p = strstr(json, "\"npId\" :");
    if (p) {
        const char *quote1 = strchr(p, ':');
        if (quote1) {
            quote1 = strchr(quote1, '"');
            if (quote1) {
                quote1++;
                const char *quote2 = strchr(quote1, '"');
                if (quote2) {
                    size_t len = (size_t)(quote2 - quote1);
                    if (len > 15) len = 15;
                    memcpy(npid, quote1, len);
                    ((char *)npid)[len] = '\0';
                }
            }
        }
    }
    runtime_net_trace("sceNpWebApiUtilityParseNpId -> '%s'", (char *)npid);
    return 0;
}

static ABI int32_t http_uri_escape(char *out, uint64_t *require, uint64_t prepare, const char *in) {
    if (!in) return -1;
    static const char hex_chars[] = "0123456789ABCDEF";
    uint64_t needed = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            needed++;
        } else {
            needed += 3;
        }
    }
    needed++;
    if (require) *require = needed;
    if (!out) return 0;
    if (prepare < needed) return -1;
    char *dst = out;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            *dst++ = *p;
        } else {
            *dst++ = '%';
            *dst++ = hex_chars[(*p >> 4) & 0x0F];
            *dst++ = hex_chars[*p & 0x0F];
        }
    }
    *dst = '\0';
    return 0;
}

/* (ctxId, stats*): sceSslGetMemoryPoolStats, sceHttpGetMemoryPoolStats, sceNetGetMemoryPoolStats,
 * sceNpInGameMessageGetMemoryPoolStatistics. Stats = 3 size_t + int32 (32 bytes). */
static ABI int32_t ctx_pool_stats(int32_t ctx, void *stats) {
    (void)ctx;
    if (stats) memset(stats, 0, 32);
    return 0;
}

/* ---- Voice chat: ports exist, carry no audio ---- */
static ABI int32_t voice_port(void *param,uint32_t *port) { (void)param; if (!port) return (int32_t)0x8029000b; *port=(uint32_t)new_id(); return 0; }
static ABI int32_t voice_read(uint32_t port,void *data,uint32_t *size) { (void)port; (void)data; if (size) *size=0; return 0; }
static ABI int32_t voice_write(uint32_t port,const void *data,uint32_t *size) { (void)port; (void)data; (void)size; return 0; }
static ABI int32_t voice_info(uint32_t port,uint32_t *info) {
    (void)port;
    if (!info) return (int32_t)0x8029000b;
    memset(info,0,40); /* type, state=unconnected, no bytes available */
    return 0;
}

/* ---- Common dialogs: nothing is displayed; an opened dialog finishes. ---- */
typedef struct { const char *name; int initialized, status; } Dialog;
static Dialog dialogs[]={{"CommonDialog",0,0},{"MsgDialog",0,0},{"SaveDataDialog",0,0},
                         {"NpProfileDialog",0,0},{"NpCommerceDialog",0,0},{"ImeDialog",0,0},
                         {"WebBrowserDialog",0,0},{"InvitationDialog",0,0},
                         {"SigninDialog",0,0},{"GameCustomDataDialog",0,0}};
static int common_initialized;
static ABI int32_t common_init(void) { common_initialized=1; return 0; }
static int32_t dialog_init(int i) {
    if (dialogs[i].initialized) return (int32_t)0x80B80004;
    dialogs[i].initialized=1; dialogs[i].status=1; return 0;
}
static int32_t dialog_open(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s opened; completed immediately (no dialog UI yet)\n",dialogs[i].name);
    dialogs[i].status=3; return 0;
}
static int32_t dialog_status(int i) { return dialogs[i].status; }
static int32_t dialog_term(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    dialogs[i].initialized=0; dialogs[i].status=0; return 0;
}
#define DIALOG(tag,i) \
    static ABI int32_t tag##_init(void) { return dialog_init(i); } \
    static ABI int32_t tag##_open(const void *p) { (void)p; return dialog_open(i); } \
    static ABI int32_t tag##_status(void) { return dialog_status(i); } \
    static ABI int32_t tag##_term(void) { return dialog_term(i); }
DIALOG(msg,1) DIALOG(save,2) DIALOG(profile,3) DIALOG(commerce,4) DIALOG(webbrowser,6) DIALOG(signin,8) DIALOG(customdata,9)
/* The game's Invite button opens the PS4 invitation dialog to pick friends (its parameter names
 * only the game's own session, "cusa13795-session-<nickname>", in user-select mode). With RyuNET
 * friends, every friend online is invited at once and a notice says so (runtime_friends.c); the
 * dialog then closes as before. */
static ABI int32_t invitation_init(void) { return dialog_init(7); }
static ABI int32_t invitation_open(const void *p) {
    (void)p;
    if (!dialogs[7].initialized) return DIALOG_NOT_INITIALIZED;
    if (runtime_friends_invite_online()) printf("Runtime: InvitationDialog: friends online invited\n");
    return dialog_open(7);
}
static ABI int32_t invitation_status(void) { return dialog_status(7); }
static ABI int32_t invitation_term(void) { return dialog_term(7); }
/* SacredFire: the web browser and invitation dialogs close at once with a zeroed result (user
 * closed / no invitation sent); Close on the browser is accepted. */
static ABI int32_t webbrowser_close(void) { if (!dialogs[6].initialized) return DIALOG_NOT_INITIALIZED; dialogs[6].status=3; return 0; }
static ABI int32_t webbrowser_result(void *result) { if (result) memset(result,0,16); return 0; }
static ABI int32_t invitation_result(void *result) { if (result) memset(result,0,16); return 0; }
static ABI int32_t profile_result(void *result) { if (result) memset(result,0,4); return 0; }
static ABI int32_t signin_result(void *result) { if (result) memset(result,0,16); return 0; }
static ABI int32_t customdata_result(void *result) { if (result) memset(result,0,16); return 0; }
/* SacredFire: the message dialog closed as if the user pressed its first button: result OK (0),
 * button 1 (OK/Yes). OrbisMsgDialogResult: mode, result, buttonId, reserved[32]. */
typedef struct { int32_t mode, result, button; char reserved[32]; } MsgDialogResult;
static ABI int32_t msg_result(MsgDialogResult *r) {
    if (!dialogs[1].initialized) return DIALOG_NOT_INITIALIZED;
    if (!r) return (int32_t)0x80B80001;
    r->result=0; r->button=1; memset(r->reserved,0,sizeof(r->reserved)); return 0;
}
/* NetCtl init/term bracket the disconnected state; a resolver abort has nothing to abort offline. */
/* UserService event callbacks (ForNpToolkit): registered and notify login of local user 1. */
typedef struct SceUserServiceEvent {
    int32_t eventType; /* 0 = SCE_USER_SERVICE_EVENT_TYPE_LOGIN */
    int32_t userId;    /* 1 */
} SceUserServiceEvent;
typedef void (ABI *SceUserServiceEventCallback)(const SceUserServiceEvent *event, void *userData);

static SceUserServiceEventCallback s_user_event_cb = NULL;
static void *s_user_event_arg = NULL;

static ABI int32_t user_event_register(void *cb, void *arg) {
    s_user_event_cb = (SceUserServiceEventCallback)cb;
    s_user_event_arg = arg;
    runtime_net_trace("sceUserServiceRegisterEventCallback: registered cb=%p, arg=%p", cb, arg);
    if (s_user_event_cb) {
        SceUserServiceEvent ev;
        ev.eventType = 0; /* SCE_USER_SERVICE_EVENT_TYPE_LOGIN */
        ev.userId = 1;
        runtime_net_trace("sceUserServiceRegisterEventCallback -> dispatching initial LOGIN for user 1");
        s_user_event_cb(&ev, s_user_event_arg);
    }
    return 0;
}
static ABI int32_t user_event_unregister(void) {
    s_user_event_cb = NULL;
    s_user_event_arg = NULL;
    return 0;
}

/* ImeDialog: text typed on the keyboard into the game window (title bar shows it).
 * OrbisImeDialogParam: user, type, languages(8), enter label, method, filter,
 * option, max length, char16 buffer, position, alignment, placeholder, title. */
typedef struct {
    int32_t user; uint32_t type; uint64_t languages; uint32_t enter_label, input_method;
    void *filter; uint32_t option, max_length; uint16_t *buffer;
    float x, y; uint32_t halign, valign; const uint16_t *placeholder, *title; int8_t reserved[16];
} ImeParam;
static struct { int running, finished, end_status; uint16_t *buffer; uint32_t max_length; } ime;
static size_t utf16_to_utf8(const uint16_t *in, size_t limit, char *out, size_t size) {
    size_t n=0;
    for (size_t i=0; in && i<limit && in[i] && n+4<size; ++i) {
        uint32_t c=in[i];
        if (c>=0xD800 && c<0xDC00 && i+1<limit && in[i+1]>=0xDC00 && in[i+1]<0xE000) { c=0x10000+((c-0xD800)<<10)+(in[i+1]-0xDC00); ++i; }
        if (c<0x80) out[n++]=(char)c;
        else if (c<0x800) { out[n++]=(char)(0xC0|c>>6); out[n++]=(char)(0x80|(c&63)); }
        else if (c<0x10000) { out[n++]=(char)(0xE0|c>>12); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
        else { out[n++]=(char)(0xF0|c>>18); out[n++]=(char)(0x80|((c>>12)&63)); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
    }
    out[n]=0; return n;
}
static void utf8_to_utf16(const char *in, uint16_t *out, uint32_t max) {
    uint32_t n=0;
    for (const unsigned char *p=(const unsigned char *)in; *p && n<max;) {
        uint32_t c; int extra;
        if (*p<0x80) { c=*p; extra=0; } else if ((*p&0xE0)==0xC0) { c=*p&31; extra=1; }
        else if ((*p&0xF0)==0xE0) { c=*p&15; extra=2; } else { c=*p&7; extra=3; }
        ++p;
        for (int k=0;k<extra && (*p&0xC0)==0x80;++k) c=(c<<6)|(*p++&63);
        if (c>=0x10000) { if (n+2>max) break; c-=0x10000; out[n++]=(uint16_t)(0xD800+(c>>10)); out[n++]=(uint16_t)(0xDC00+(c&1023)); }
        else out[n++]=(uint16_t)c;
    }
    out[n<max ? n : max]=0;
}
static void ime_complete(int end_status, const char *text) {
    if (!end_status && ime.buffer && ime.max_length) utf8_to_utf16(text,ime.buffer,ime.max_length);
    ime.end_status=end_status; ime.finished=1; ime.running=0;
    printf("Runtime: ImeDialog %s%s%s\n",end_status ? "cancelled" : "text: ",end_status ? "" : text,"");
}
static ABI int32_t ime_init(const ImeParam *param, const void *extended) {
    (void)extended;
    if (!param || !param->buffer || !param->max_length) return (int32_t)0x80BC0004; /* IME INVALID_ADDRESS */
    if (ime.running) return (int32_t)0x80BC0003;                                  /* BUSY */
    memset(&ime,0,sizeof(ime));
    ime.buffer=param->buffer; ime.max_length=param->max_length; ime.running=1;
    char initial[512], prompt[256];
    utf16_to_utf8(param->buffer,param->max_length,initial,sizeof(initial));
    utf16_to_utf8(param->title,128,prompt,sizeof(prompt));
    const char *preset=getenv("BB_IME_TEXT");
    if (preset) { ime_complete(0,preset); return 0; }
    if (!bbgpu_text_input_begin(initial,prompt[0] ? prompt : "Text")) {
        const char *name=getenv("BB_USER_NAME");
        ime_complete(0,name ? name : initial[0] ? initial : default_user_name());
    } else printf("Runtime: ImeDialog opened: type in the game window, Enter to confirm, Esc to cancel\n");
    return 0;
}
static ABI int32_t ime_status(void) {
    if (ime.running) {
        char text[512];
        int state=bbgpu_text_input_poll(text,sizeof(text));
        if (state) ime_complete(state==1 ? 0 : 1,text);
    }
    return ime.running ? 1 : ime.finished ? 2 : 0; /* Running / Finished / None */
}
static ABI int32_t ime_result(uint32_t *result) {
    if (!ime.finished) return (int32_t)0x80BC0101; /* DIALOG NOT_FINISHED */
    if (result) *result=(uint32_t)ime.end_status;
    return 0;
}
static ABI int32_t ime_term(void) { memset(&ime,0,sizeof(ime)); return 0; }

/* ---- Trophies (v0.4): unlocks are kept in <user>/trophies/<TITLE_ID>/unlocked.txt, one
 * "<id> <unix time>" line each, and shown as a notice over the game. The launcher's Achievements
 * tab reads the same file; it also writes trophies.tsv (id, grade, hidden, name, detail; from
 * sce_sys/trophy/trophy00.trp when the player gave it their PS4's trophy key), which gives the
 * notice its name and the platinum its id. The platinum unlocks itself once every other trophy
 * is, as on a PS4. */
#define TROPHY_MAX 128
static pthread_mutex_t trophy_lock=PTHREAD_MUTEX_INITIALIZER;
static char trophy_title[16]="";
static int trophy_loaded, trophy_total=-1, trophy_platinum=-1;
static uint64_t trophy_unlocked_at[TROPHY_MAX];      /* 0: locked */
static char trophy_grade[TROPHY_MAX];                 /* 'P' 'G' 'S' 'B', 0 unknown */
static char *trophy_names[TROPHY_MAX], *trophy_details[TROPHY_MAX];
/* Weak: tests link this module without the GPU library. */
__attribute__((weak)) void bbgpu_notify(const char *title_utf8, const char *name_utf8, const char *detail_utf8, int kind,
                                        const void *png, uint64_t png_size);

void runtime_trophy_configure(const char *title) {
    if (title && *title) snprintf(trophy_title,sizeof(trophy_title),"%s",title);
}
static void trophy_path(char *out, size_t size, const char *file) {
    snprintf(out,size,"%s/trophies/%s/%s",runtime_file_user_dir(),trophy_title[0] ? trophy_title : "UNKNOWN",file);
}
/* The game's trophy00.trp (big-endian header, 64-byte entries at 0x60: 32-byte name, offset,
 * size): the number of trophies (TROPnnn.PNG entries) or, with `id` >= 0, that trophy's picture
 * (a malloc'ed PNG in *png). */
static int trophy_trp(int id, unsigned char **png, uint64_t *png_size) {
    int fd=(int)runtime_file_open("/app0/sce_sys/trophy/trophy00.trp",0,0);
    if (fd<0) return -1;
    unsigned char header[0x60];
    int count=-1;
    char want[16]="";
    if (id>=0) snprintf(want,sizeof(want),"TROP%03d.PNG",id);
    if (runtime_file_read(fd,header,sizeof(header))==sizeof(header) && header[0]==0xDC && header[1]==0xA2) {
        const uint32_t entries=(uint32_t)header[16]<<24|header[17]<<16|header[18]<<8|header[19];
        count=0;
        for (uint32_t i=0;i<entries && i<512;++i) {
            unsigned char e[64];
            if (runtime_file_pread(fd,e,sizeof(e),0x60+(int64_t)i*64)!=sizeof(e)) break;
            if (!memcmp(e,"TROP",4) && e[4]>='0' && e[4]<='9' && e[7]=='.' && !memcmp(e+8,"PNG",3)) ++count;
            if (want[0] && !memcmp(e,want,12)) {
                uint64_t at=0, size=0;
                for (int b=0;b<8;++b) { at=at<<8|e[32+b]; size=size<<8|e[40+b]; }
                if (size && size<(4u<<20) && (*png=malloc(size)) && runtime_file_pread(fd,*png,size,(int64_t)at)==(int64_t)size) *png_size=size;
                else { free(*png); *png=NULL; }
                break;
            }
        }
    }
    runtime_file_close(fd);
    return count;
}
static int trophy_count_from_trp(void) { return trophy_trp(-1,NULL,NULL); }
static void trophy_load(void) {
    if (trophy_loaded) return;
    trophy_loaded=1;
    char path[800], line[2048];
    trophy_path(path,sizeof(path),"unlocked.txt");
    FILE *f=fopen(path,"r");
    while (f && fgets(line,sizeof(line),f)) {
        int id; unsigned long long at;
        if (sscanf(line,"%d %llu",&id,&at)==2 && id>=0 && id<TROPHY_MAX && !trophy_unlocked_at[id]) trophy_unlocked_at[id]=at ? at : 1;
    }
    if (f) fclose(f);
    trophy_path(path,sizeof(path),"trophies.tsv");
    f=fopen(path,"r");
    int listed=0;
    while (f && fgets(line,sizeof(line),f)) {
        char *fields[5]={0}; int n=0;
        line[strcspn(line,"\r\n")]=0;
        for (char *p=line; n<5; ++n) { fields[n]=p; char *tab=strchr(p,'\t'); if (!tab) { ++n; break; } *tab=0; p=tab+1; }
        if (n<2 || fields[0][0]<'0' || fields[0][0]>'9') continue;
        const int id=atoi(fields[0]);
        if (id<0 || id>=TROPHY_MAX) continue;
        ++listed;
        trophy_grade[id]=fields[1][0];
        if (fields[1][0]=='P') trophy_platinum=id;
        if (n>=4 && fields[3] && *fields[3]) { free(trophy_names[id]); trophy_names[id]=strdup(fields[3]); }
        if (n>=5 && fields[4] && *fields[4]) { free(trophy_details[id]); trophy_details[id]=strdup(fields[4]); }
    }
    if (f) fclose(f);
    trophy_total=listed ? listed : trophy_count_from_trp();
    /* No trophies.tsv (the launcher writes it): CTR's own list (ctr_trophies.h). */
    if (!listed && trophy_total==CTR_TROPHY_COUNT && (!strcmp(trophy_title,"CUSA13795") || !strcmp(trophy_title,"CUSA14876"))) {
        const char *lang=getenv("BB_LANGUAGE");
        const int es=lang && (!strcmp(lang,"3") || !strcmp(lang,"20"));
        for (int i=0;i<CTR_TROPHY_COUNT;++i) {
            const CtrTrophy *t=&ctr_trophies[i];
            trophy_grade[t->id]=t->grade;
            if (t->grade=='P') trophy_platinum=t->id;
            trophy_names[t->id]=strdup(t->name);
            trophy_details[t->id]=strdup(es ? t->es : t->en);
        }
        listed=CTR_TROPHY_COUNT;
    }
    if (trophy_platinum<0 && !listed && trophy_total>1) trophy_platinum=0; /* the usual id when the grades are unknown */
    int before=0;
    for (int i=0;i<TROPHY_MAX;++i) before+=trophy_unlocked_at[i]!=0;
    printf("Runtime: trophies: %d in the game, %d unlocked before%s\n",trophy_total,before,
           listed ? "" : " (names: the launcher's Achievements tab)");
}
static int trophy_spanish(void) {
    const char *lang=getenv("BB_LANGUAGE");
    return lang && (!strcmp(lang,"3") || !strcmp(lang,"20"));
}
static void trophy_record(int id, int notify) {
    trophy_unlocked_at[id]=(uint64_t)time(NULL);
    char path[800];
    trophy_path(path,sizeof(path),"");
    char dirs[800]; snprintf(dirs,sizeof(dirs),"%s",path);
    for (char *p=dirs+1;*p;++p) if (*p=='/' || *p=='\\') { const char c=*p; *p=0; mkdir(dirs,0755); *p=c; }
    trophy_path(path,sizeof(path),"unlocked.txt");
    FILE *f=fopen(path,"a");
    if (f) { fprintf(f,"%d %llu\n",id,(unsigned long long)trophy_unlocked_at[id]); fclose(f); }
    printf("Runtime: trophy %d unlocked%s%s\n",id,trophy_names[id] ? ": " : "",trophy_names[id] ? trophy_names[id] : "");
    if (notify && bbgpu_notify) {
        char text[512];
        if (trophy_names[id]) snprintf(text,sizeof(text),"%s",trophy_names[id]);
        else snprintf(text,sizeof(text),trophy_spanish() ? "Logro #%d" : "Trophy #%d",id);
        const int kind=trophy_grade[id]=='P' ? 4 : trophy_grade[id]=='G' ? 3 : trophy_grade[id]=='S' ? 2 : trophy_grade[id]=='B' ? 1 : 0;
        unsigned char *png=NULL; uint64_t png_size=0;
        trophy_trp(id,&png,&png_size);
        bbgpu_notify(trophy_spanish() ? "Logro desbloqueado" : "Trophy unlocked",text,trophy_details[id] ? trophy_details[id] : "",
                     kind,png,png_size);
        free(png);
    }
}
static ABI int32_t trophy_context(int32_t *ctx,int32_t user,uint32_t label,uint64_t options) {
    (void)user; (void)label; (void)options;
    if (!ctx) return TROPHY_INVALID;
    *ctx=new_id(); return 0;
}
static ABI int32_t trophy_handle(int32_t *handle) { if (!handle) return TROPHY_INVALID; *handle=new_id(); return 0; }
static ABI int32_t trophy_register(int32_t ctx,int32_t handle,uint64_t options) {
    (void)ctx; (void)handle; (void)options;
    pthread_mutex_lock(&trophy_lock); trophy_load(); pthread_mutex_unlock(&trophy_lock);
    return 0;
}
#define TROPHY_ALREADY_UNLOCKED ((int32_t)0x8055160F)
static ABI int32_t trophy_unlock(int32_t ctx,int32_t handle,int32_t id,int32_t *platinum) {
    (void)ctx; (void)handle;
    if (platinum) *platinum=-1;
    if (id<0 || id>=TROPHY_MAX) return TROPHY_INVALID;
    pthread_mutex_lock(&trophy_lock);
    trophy_load();
    if (trophy_unlocked_at[id]) { pthread_mutex_unlock(&trophy_lock); return TROPHY_ALREADY_UNLOCKED; }
    trophy_record(id,1);
    /* Every other trophy unlocked: the platinum too. */
    if (trophy_platinum>=0 && trophy_platinum<TROPHY_MAX && id!=trophy_platinum && !trophy_unlocked_at[trophy_platinum] && trophy_total>1) {
        int missing=0;
        for (int i=0;i<trophy_total && i<TROPHY_MAX;++i) if (i!=trophy_platinum && !trophy_unlocked_at[i]) ++missing;
        if (!missing) { trophy_record(trophy_platinum,1); if (platinum) *platinum=trophy_platinum; }
    }
    pthread_mutex_unlock(&trophy_lock);
    return 0;
}
/* SceNpTrophyFlagArray: 128 bits, bit id%32 of word id/32; count: the game's trophies. */
static ABI int32_t trophy_unlock_state(int32_t ctx,int32_t handle,uint32_t *flags,uint32_t *count) {
    (void)ctx; (void)handle;
    if (!flags || !count) return TROPHY_INVALID;
    pthread_mutex_lock(&trophy_lock);
    trophy_load();
    memset(flags,0,16);
    for (int i=0;i<TROPHY_MAX;++i) if (trophy_unlocked_at[i]) flags[i/32]|=1u<<(i%32);
    *count=(uint32_t)(trophy_total>0 ? trophy_total : 0);
    pthread_mutex_unlock(&trophy_lock);
    return 0;
}
static ABI int32_t trophy_game_info(int32_t ctx,int32_t handle,void *details,void *data) {
    (void)ctx; (void)handle;
    if (details) { uint64_t size; memcpy(&size,details,8); memset((char *)details+8,0,size>8 && size<4096 ? size-8 : 0); }
    if (data) { uint64_t size; memcpy(&size,data,8); memset((char *)data+8,0,size>8 && size<4096 ? size-8 : 0); }
    return 0;
}
static ABI int32_t trophy_info(int32_t ctx,int32_t handle,int32_t id,void *details,void *data) {
    (void)id; return trophy_game_info(ctx,handle,details,data);
}

/* ---- PlayGo: fully installed package ---- */
static int playgo_handle, playgo_chunks=-1;
static ABI int32_t playgo_init(const void *params) {
    (void)params;
    int fd=(int)runtime_file_open("/app0/sce_sys/playgo-chunk.dat",0,0);
    unsigned char header[12];
    playgo_chunks=1;
    if (fd>=0) {
        if (runtime_file_read(fd,header,sizeof(header))==sizeof(header)) playgo_chunks=header[10]|header[11]<<8;
        runtime_file_close(fd);
    }
    printf("Runtime: PlayGo initialized; %d chunks, all installed locally\n",playgo_chunks);
    return 0;
}
static ABI int32_t playgo_open(int32_t *handle,const void *param) {
    (void)param;
    if (!handle) return PLAYGO_BAD_POINTER;
    playgo_handle=1; *handle=1; return 0;
}
static ABI int32_t playgo_chunk_ids(int32_t handle,uint16_t *ids,uint32_t count,uint32_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!out) return PLAYGO_BAD_POINTER;
    if (ids && !count) return PLAYGO_BAD_SIZE;
    if (!ids) { *out=(uint32_t)playgo_chunks; return 0; }
    uint32_t n=count<(uint32_t)playgo_chunks ? count : (uint32_t)playgo_chunks;
    for (uint32_t i=0;i<n;++i) ids[i]=(uint16_t)i;
    *out=n; return 0;
}
static ABI int32_t playgo_locus(int32_t handle,const uint16_t *ids,uint32_t count,int8_t *loci) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !loci) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) {
        if (ids[i]>=playgo_chunks) return PLAYGO_BAD_CHUNK;
        loci[i]=3; /* LocalFast */
    }
    return 0;
}
/* The install speed the game last set (FULL 2 by default); the package being complete, the speed
 * changes nothing but is reported back as set. */
static int32_t playgo_install_speed=2;
static ABI int32_t playgo_speed(int32_t handle,int32_t speed) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (speed<0 || speed>2) return (int32_t)0x80B20002; /* INVALID_ARGUMENT */
    playgo_install_speed=speed; return 0;
}
static ABI int32_t playgo_get_speed(int32_t handle,int32_t *speed) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!speed) return PLAYGO_BAD_POINTER;
    *speed=playgo_install_speed; return 0;
}
/* Progress of a fully installed package: every chunk's progress equals its total (sizes are
 * not read from playgo-chunk.dat; the ratio is what games act on), ETA 0 seconds. */
typedef struct { uint64_t progress, total; } PlayGoProgress;
static ABI int32_t playgo_progress(int32_t handle,const uint16_t *ids,uint32_t count,PlayGoProgress *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !out) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) if (ids[i]>=playgo_chunks) return PLAYGO_BAD_CHUNK;
    out->progress=out->total=(uint64_t)count*65536; return 0;
}
static ABI int32_t playgo_eta(int32_t handle,const uint16_t *ids,uint32_t count,int64_t *eta) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !eta) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    *eta=0; return 0;
}
/* The to-do list (chunks still to install) is empty and stays so; prefetching is accepted. */
typedef struct { uint16_t chunk; uint8_t locus, reserved; } PlayGoToDo;
static ABI int32_t playgo_get_todo(int32_t handle,PlayGoToDo *list,uint32_t count,uint32_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!list || !out) return PLAYGO_BAD_POINTER;
    (void)count; *out=0; return 0;
}
static ABI int32_t playgo_set_todo(int32_t handle,const PlayGoToDo *list,uint32_t count) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!list) return PLAYGO_BAD_POINTER;
    (void)count; return 0;
}
static ABI int32_t playgo_prefetch(int32_t handle,const uint16_t *ids,uint32_t count,int8_t locus) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    (void)locus; return 0;
}
static ABI int32_t playgo_close(int32_t handle) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    playgo_handle=0; return 0;
}
static ABI int32_t playgo_term(void) { playgo_handle=0; return 0; }

/* ---- SacredFire: console features that are absent or idle offline ----
 * SharePlay: initialized, nothing connected, prohibition accepted. */
static int shareplay_prohibition;
static ABI int32_t shareplay_init(void *heap,uint64_t size) { (void)heap; (void)size; return 0; }
static ABI int32_t shareplay_term(void) { return 0; }
static ABI int32_t shareplay_prohibit(int32_t mode) { shareplay_prohibition=mode; return 0; }
/* OrbisSharePlayConnectionInfo: status (0 = dormant), user ids and reserved bytes: 48 bytes zeroed. */
static ABI int32_t shareplay_connection(void *info) { if (!info) return (int32_t)0x80B10003; memset(info,0,48); return 0; }
/* Video recording (the Share button's clips): memory query answers a small size so the game's
 * allocation succeeds; opening a recording fails, which games treat as "recording off".
 * Error codes: 0x80B4 is libSceVideoRecording's facility; the exact member is assumed. */
#define VIDEO_RECORDING_UNAVAILABLE ((int32_t)0x80B40001)
static ABI int32_t videorec_memsize(const void *param) { (void)param; return 0x10000; }
static ABI int32_t videorec_open(const char *path,const void *param,void *buffer,uint64_t size) {
    (void)param; (void)buffer; (void)size;
    printf("Runtime: video recording to %s refused (unavailable)\n",path ? path : "(null)");
    return VIDEO_RECORDING_UNAVAILABLE;
}
static ABI int32_t videorec_fail(void) { return VIDEO_RECORDING_UNAVAILABLE; }
static ABI int32_t videorec_status(void) { return 0; } /* SCE_VIDEO_RECORDING_STATUS_NONE */
/* Content export (screenshots/videos to the console gallery): initialized, every export fails.
 * 0x80AD is assumed for the facility; games check for a negative result. */
#define CONTENT_EXPORT_UNAVAILABLE ((int32_t)0x80AD0001)
static ABI int32_t contentexport_init(const void *param) { (void)param; return 0; }
static ABI int32_t contentexport_term(void) { return 0; }
static ABI int32_t contentexport_fail(void) { return CONTENT_EXPORT_UNAVAILABLE; }
/* Game live streaming: initialized, streaming never enabled. */
static ABI int32_t livestream_init(uint64_t heap_size) { (void)heap_size; return 0; }
static ABI int32_t livestream_term(void) { return 0; }
static ABI int32_t livestream_enable(int32_t enable) { (void)enable; return 0; }
/* Screenshots: the system's screenshot permission flag; nothing observes it here. */
static ABI int32_t screenshot_toggle(void) { return 0; }
/* NpManager/NpAuth "A" (account id) variants: the user is signed out. */

/* Trophy context/handle destruction: ids come from new_id and are never reused. */
static ABI int32_t trophy_destroy(int32_t id) { return id>0 ? 0 : TROPHY_INVALID; }
/* HTTP extras: nothing is in flight, so aborting, chunked mode and the SSL callback are accepted;
 * response header queries fail as every request does offline. */
static ABI int32_t http_ok(void) { return 0; }
/* Commerce dialog result: closed without a purchase (result 0, zeroed). PS Store icon: no overlay. */
static ABI int32_t commerce_result(void *result) { if (result) memset(result,0,8); return 0; }
static ABI int32_t commerce_icon(void) { return 0; }

/* ---- DiscMap: the game is fully installed, no disc bitmap exists ---- */
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
static ABI int32_t discmap_on_hdd(const char *path,int64_t offset,int64_t size,int32_t *result) {
    (void)path; (void)offset; (void)size; (void)result; return DISC_MAP_NO_BITMAP;
}
static ABI int32_t discmap_8a82(const char *path,int64_t offset,int64_t size,int32_t *flags,int32_t *r1,int32_t *r2) {
    (void)path; (void)offset; (void)size; (void)flags; (void)r1; (void)r2; return DISC_MAP_NO_BITMAP;
}

/* ---- Devices that are absent: mouse, microphone, voice chat ---- */
/* A mouse port opens but never reports a connected device. */
static ABI int32_t mouse_open(int32_t user,int32_t type,int32_t index,const void *param) { (void)user; (void)type; (void)index; (void)param; return new_id(); }
static ABI int32_t mouse_read(int32_t handle,unsigned char *data,int32_t count) {
    (void)handle;
    if (!data || count<1) return (int32_t)0x80DF0001;
    memset(data,0,40); return 1; /* timestamp 0, connected=false */
}
static ABI int32_t mouse_close(int32_t handle) { (void)handle; return 0; }
static ABI int32_t audio_in_open(void) { return AUDIO_IN_NOT_OPENED; }


static const RuntimeExport exports[]={
    {"sceUserServiceInitialize",user_initialize}, {"sceUserServiceTerminate",user_terminate},
    {"sceUserServiceGetInitialUser",user_initial}, {"sceUserServiceGetLoginUserIdList",user_list},
    {"sceUserServiceGetUserName",user_name}, {"sceUserServiceGetEvent",user_event},
    {"sceSystemServiceParamGetInt",system_param}, {"sceSystemServiceGetStatus",system_status},
    {"sceSystemServiceReceiveEvent",system_event}, {"sceSystemServiceHideSplashScreen",hide_splash},
    {"sceSystemServiceLaunchWebBrowser",launch_browser},
    {"sceNetInit",net_init}, {"sceNetTerm",net_term}, {"sceNetErrnoLoc",net_errno_loc},
    {"sceNetPoolCreate",net_pool_create}, {"sceNetPoolDestroy",net_pool_destroy},
    {"sceNetEpollCreate",net_epoll_create}, {"sceNetEpollDestroy",net_epoll_destroy},
    {"sceNetResolverCreate",net_resolver_create}, {"sceNetResolverDestroy",net_resolver_destroy},
    {"sceNetHtons",net_htons}, {"sceNetNtohs",net_ntohs}, {"sceNetHtonl",net_htonl}, {"sceNetNtohl",net_ntohl},
    {"sceNetInetPton",net_pton}, {"sceNetInetNtop",net_ntop},
    {"sceNetSocket",net_socket}, {"sceNetConnect",net_connect}, {"sceNetBind",net_bind},
    {"sceNetListen",net_listen}, {"sceNetAccept",net_accept}, {"sceNetSend",net_send},
    {"sceNetSendto",net_sendto}, {"sceNetRecv",net_recv}, {"sceNetRecvfrom",net_recvfrom},
    {"sceNetSetsockopt",net_setsockopt}, {"sceNetGetsockopt",net_getsockopt},
    {"sceNetGetsockname",net_getsockname}, {"sceNetShutdown",net_shutdown},
    {"sceNetSocketClose",net_close}, {"sceNetSocketAbort",net_abort},
    {"sceNetEpollControl",net_epoll_control}, {"sceNetEpollWait",net_epoll_wait}, {"sceNetEpollAbort",net_epoll_abort},
    {"sceNetResolverStartNtoa",net_resolver_start_ntoa}, {"sceNetResolverStartAton",net_resolver_start_aton},
    {"sceNetCtlGetState",netctl_state}, {"sceNetCtlGetInfo",netctl_info},
    {"sceNetCtlRegisterCallback",netctl_register}, {"sceNetCtlCheckCallback",netctl_check},
    {"sceNetCtlUnregisterCallback",netctl_unregister}, {"sceNetCtlGetNatInfo",netctl_nat},
    {"sceSslInit",lib_init_id}, {"sceSslTerm",ok_void},
    {"sceHttpInit",http_init_abi}, {"sceHttpTerm",http_term_abi},
    {"sceHttpCreateTemplate",http_template}, {"sceHttpDeleteTemplate",http_del_template},
    {"sceHttpCreateConnectionWithURL",http_conn}, {"sceHttpCreateRequestWithURL",http_req},
    {"sceHttpSendRequest",http_send}, {"sceHttpCreateEpoll",http_epoll},
    {"sceHttpSetNonblock",ok_void}, {"sceHttpSetConnectTimeOut",ok_void},
    {"sceHttpsEnableOption",ok_void}, {"sceHttpsDisableOption",ok_void},
    {"sceHttpAddRequestHeader",http_add_header}, {"sceHttpSetRequestContentLength",http_set_req_len},
    {"sceHttpDeleteConnection",http_del_conn}, {"sceHttpDeleteRequest",http_del_req},
    {"sceHttpAbortWaitRequest",http_abort}, {"sceHttpDestroyEpoll",http_destroy_epoll},
    {"sceHttpSetEpoll",http_set_epoll}, {"sceHttpUnsetEpoll",http_unset_epoll}, {"sceHttpWaitRequest",http_wait},
    {"sceHttpGetStatusCode",http_status_code}, {"sceHttpGetResponseContentLength",http_resp_len},
    {"sceHttpReadData",http_read_data},
    {"sceNpGetState",np_state}, {"sceNpGetOnlineId",np_online_id}, {"sceNpGetNpId",np_np_id},
    {"sceNpRegisterStateCallback",np_reg_state_cb}, {"sceNpUnregisterStateCallback",np_unreg_state_cb},
    {"sceNpRegisterGamePresenceCallback",np_register_void}, {"sceNpRegisterPlusEventCallback",np_register},
    {"sceNpUnregisterPlusEventCallback",ok_void}, {"sceNpCheckCallback",np_check_cb},
    {"sceNpSetNpTitleId",ok_void}, {"sceNpNotifyPlusFeature",ok_void}, {"sceNpSetContentRestriction",ok_void},
    {"sceNpCreateAsyncRequest",np_request}, {"sceNpDeleteRequest",ok_void}, {"sceNpAbortRequest",ok_void},
    {"sceNpPollAsync",np_poll}, {"sceNpCheckNpAvailability",np_check_avail},
    {"sceNpGetParentalControlInfo",np_parental_control}, {"sceNpCheckPlus",np_check_plus},
    {"sceNpGetGamePresenceStatus",np_presence_status},
    {"sceNpCmpNpId",np_compare}, {"sceNpCmpOnlineId",np_compare},
    {"sceNpAuthCreateRequest",np_request}, {"sceNpAuthCreateAsyncRequest",np_request},
    {"sceNpAuthDeleteRequest",ok_void}, {"sceNpAuthAbortRequest",ok_void},
    {"sceNpAuthPollAsync",np_poll}, {"sceNpAuthGetAuthorizationCode",np_auth_code},
    {"sceNpLookupCreateTitleCtx",np_request}, {"sceNpLookupDeleteTitleCtx",ok_void},
    {"sceNpLookupCreateAsyncRequest",np_request_ctx}, {"sceNpLookupDeleteRequest",ok_void},
    {"sceNpLookupAbortRequest",ok_void}, {"sceNpLookupPollAsync",np_poll}, {"sceNpLookupNpId",np_signed_out},
    {"sceNpScoreCreateNpTitleCtx",np_request_ctx}, {"sceNpScoreDeleteNpTitleCtx",ok_void},
    {"sceNpScoreCreateRequest",np_request}, {"sceNpScoreDeleteRequest",ok_void}, {"sceNpScoreAbortRequest",ok_void},
    {"sceNpWebApiInitialize",lib_init_id}, {"sceNpWebApiTerminate",ok_void},
    {"sceNpWebApiCreateContext",np_webapi_create_ctx}, {"sceNpWebApiCreateContextA",np_webapi_create_ctx_a},
    {"sceNpWebApiCreateHandle",lib_init_id}, {"sceNpWebApiDeleteHandle",ok_void},
    {"sceNpWebApiCreatePushEventFilter",lib_init_id}, {"sceNpWebApiDeletePushEventFilter",ok_void},
    {"sceNpWebApiCreateExtdPushEventFilter",np_push_filter}, {"sceNpWebApiDeleteExtdPushEventFilter",ok_void},
    {"sceNpWebApiRegisterPushEventCallback",lib_init_id}, {"sceNpWebApiUnregisterPushEventCallback",ok_void},
    {"sceNpWebApiRegisterExtdPushEventCallback",lib_init_id}, {"sceNpWebApiRegisterExtdPushEventCallbackA",np_push_callback},
    {"sceNpWebApiUnregisterExtdPushEventCallback",ok_void},
    {"sceNpWebApiCreateRequest",np_webapi_create_req}, {"sceNpWebApiSendRequest",np_webapi_send_req},
    {"sceNpWebApiSendRequest2",np_webapi_send_req2}, {"sceNpWebApiAddHttpRequestHeader",np_webapi_add_header},
    {"sceNpWebApiDeleteRequest",np_webapi_delete_req}, {"sceNpWebApiAbortRequest",ok_void},
    {"sceNpWebApiDeleteContext",ok_void}, {"sceNpWebApiReadData",np_webapi_read_data},
    {"sceNpWebApiGetHttpStatusCode",np_webapi_get_status},
    {"sceNpWebApiGetHttpResponseHeaderValue",np_webapi_get_header},
    {"sceNpWebApiGetHttpResponseHeaderValueLength",np_webapi_get_header_len},
    {"sceNpWebApiGetMemoryPoolStats",np_webapi_pool_stats},
    {"sceNpWebApiUtilityParseNpId",np_webapi_parse_npid},
    {"sceNpMatching2ContextStart",ok_void}, {"sceNpMatching2ContextStop",ok_void},
    {"sceNpMatching2DestroyContext",ok_void},
    {"sceNpMatching2RegisterContextCallback",ok_void}, {"sceNpMatching2RegisterLobbyEventCallback",ok_void},
    {"sceNpMatching2RegisterRoomEventCallback",ok_void}, {"sceNpMatching2RegisterSignalingCallback",ok_void},
    {"sceNpMatching2RegisterRoomMessageCallback",ok_void},
    {"sceNpMatching2SetDefaultRequestOptParam",ok_void}, {"sceNpMatching2SetSignalingOptParam",ok_void},
    {"sceNpMatching2GetSignalingOptParamLocal",np_signed_out}, {"sceNpMatching2SetUserInfo",ok_void},
    {"sceNpMatching2GetUserInfoListA",np_signed_out},
    {"sceNpMatching2CreateJoinRoom",np_signed_out}, {"sceNpMatching2CreateJoinRoomA",np_signed_out},
    {"sceNpMatching2JoinRoom",np_signed_out}, {"sceNpMatching2JoinRoomA",np_signed_out},
    {"sceNpMatching2LeaveRoom",np_signed_out}, {"sceNpMatching2SearchRoom",np_signed_out},
    {"sceNpMatching2GetServerId",np_signed_out}, {"sceNpMatching2GetWorldInfoList",np_signed_out},
    {"sceNpMatching2GetLobbyInfoList",np_signed_out}, {"sceNpMatching2JoinLobby",np_signed_out},
    {"sceNpMatching2LeaveLobby",np_signed_out}, {"sceNpMatching2GrantRoomOwner",np_signed_out},
    {"sceNpMatching2KickoutRoomMember",np_signed_out},
    {"sceNpMatching2SetRoomDataExternal",np_signed_out}, {"sceNpMatching2GetRoomDataExternalList",np_signed_out},
    {"sceNpMatching2SetRoomDataInternal",np_signed_out}, {"sceNpMatching2GetRoomDataInternal",np_signed_out},
    {"sceNpMatching2SetRoomMemberDataInternal",np_signed_out}, {"sceNpMatching2GetRoomMemberDataInternal",np_signed_out},
    {"sceNpMatching2GetRoomMemberIdListLocal",np_signed_out}, {"sceNpMatching2GetRoomPasswordLocal",np_signed_out},
    {"sceNpMatching2SendRoomChatMessage",np_signed_out}, {"sceNpMatching2SendRoomMessage",np_signed_out},
    {"sceNpMatching2SignalingGetConnectionStatus",np_signed_out}, {"sceNpMatching2SignalingGetPingInfo",np_signed_out},
    {"sceNpMatching2SignalingGetConnectionInfoA",np_signed_out},
    {"sceNpMatching2GetMemoryInfo",ok_void}, {"sceNpMatching2GetSslMemoryInfo",ok_void},
    {"sceNpSignalingDeleteContext",ok_void}, {"sceNpSignalingActivateConnection",np_signed_out},
    {"sceNpSignalingDeactivateConnection",ok_void}, {"sceNpSignalingGetConnectionStatus",np_signed_out},
    {"sceNpScoreCreateNpTitleCtxA",lib_init_id},
    {"sceNpScoreGetFriendsRankingA",np_signed_out}, {"sceNpScoreGetFriendsRankingForCrossSave",np_signed_out},
    {"sceNpScoreGetGameDataByAccountId",np_signed_out}, {"sceNpScoreGetRankingByAccountId",np_signed_out},
    {"sceNpScoreGetRankingByAccountIdForCrossSave",np_signed_out}, {"sceNpScoreGetRankingByAccountIdPcId",np_signed_out},
    {"sceNpScoreGetRankingByAccountIdPcIdForCrossSave",np_signed_out},
    {"sceNpScoreGetRankingByRangeA",np_signed_out}, {"sceNpScoreGetRankingByRangeForCrossSave",np_signed_out},
    {"sceNpScoreCensorComment",np_signed_out}, {"sceNpScoreSanitizeComment",np_signed_out},
    {"sceNpScoreGetBoardInfo",np_signed_out}, {"sceNpScoreGetGameData",np_signed_out},
    {"sceNpScoreGetRankingByNpIdPcId",np_signed_out}, {"sceNpScoreGetRankingByRange",np_signed_out},
    {"sceNpScoreRecordGameData",np_signed_out}, {"sceNpScoreRecordScore",np_signed_out},
    {"sceNpScoreSetPlayerCharacterId",ok_void},
    {"sceVoiceCreatePort",voice_port}, {"sceVoiceDeletePort",ok_void},
    {"sceVoiceConnectIPortToOPort",ok_void}, {"sceVoiceDisconnectIPortFromOPort",ok_void},
    {"sceVoiceStart",ok_void}, {"sceVoiceStop",ok_void}, {"sceVoiceGetPortInfo",voice_info},
    {"sceVoiceReadFromOPort",voice_read}, {"sceVoiceWriteToIPort",voice_write},
    {"sceAudioInInput",audio_in_open}, {"sceAudioInClose",audio_in_open},
    {"sceNpMatching2Initialize",ok_void}, {"sceNpMatching2Terminate",ok_void},
    {"sceNpMatching2CreateContext",lib_init_id}, {"sceNpMatching2CreateContextA",lib_init_id},
    {"sceNpSignalingInitialize",ok_void}, {"sceNpSignalingTerminate",ok_void},
    {"sceNpSignalingCreateContext",lib_init_id}, {"sceNpSignalingCreateContextA",lib_init_id},
    {"sceNpCreateRequest",np_request},
    {"sceNpInGameMessageInitialize",ok_void}, {"sceNpInGameMessageTerminate",ok_void},
    {"sceNpInGameMessageCreateHandle",lib_init_id}, {"sceNpInGameMessageDeleteHandle",ok_void},
    {"sceNpInGameMessagePrepareA",ok_void}, {"sceNpInGameMessageSendDataA",ok_void},
    {"sceNpBandwidthTestInitStart",lib_init_id}, {"sceNpBandwidthTestGetStatus",ok_void},
    {"sceNpBandwidthTestAbort",ok_void}, {"sceNpBandwidthTestShutdown",ok_void},
    {"sceNpWordFilterCreateTitleCtxA",lib_init_id}, {"sceNpWordFilterCreateRequest",lib_init_id},
    {"sceNpWordFilterDeleteTitleCtx",ok_void}, {"sceNpWordFilterDeleteRequest",ok_void},
    {"sceNpWordFilterAbortRequest",ok_void}, {"sceNpWordFilterSanitizeComment",ok_void},
    {"sceNpTssCreateNpTitleCtxA",lib_init_id}, {"sceNpTssGetData",ok_void},
    {"sceNpTusCreateNpTitleCtxA",lib_init_id}, {"sceNpTusDeleteNpTitleCtx",ok_void},
    {"sceNpTusCreateRequest",lib_init_id}, {"sceNpTusDeleteRequest",ok_void},
    {"sceNpTusAbortRequest",ok_void},
    {"sceNpTusGetDataA",ok_void}, {"sceNpTusGetDataAVUser",ok_void},
    {"sceNpTusGetDataForCrossSave",ok_void}, {"sceNpTusGetDataForCrossSaveVUser",ok_void},
    {"sceNpTusSetDataA",ok_void}, {"sceNpTusSetDataAVUser",ok_void},
    {"sceNpTusAddAndGetVariableA",ok_void}, {"sceNpTusAddAndGetVariableAVUser",ok_void},
    {"sceNpTusAddAndGetVariableForCrossSave",ok_void}, {"sceNpTusAddAndGetVariableForCrossSaveVUser",ok_void},
    {"sceNpTusSetMultiSlotVariableA",ok_void}, {"sceNpTusSetMultiSlotVariableVUser",ok_void},
    {"sceNpTusTryAndSetVariableA",ok_void}, {"sceNpTusTryAndSetVariableAVUser",ok_void},
    {"sceNpTusTryAndSetVariableForCrossSave",ok_void}, {"sceNpTusTryAndSetVariableForCrossSaveVUser",ok_void},
    {"sceNpTusGetFriendsDataStatusA",ok_void}, {"sceNpTusGetFriendsDataStatusForCrossSave",ok_void},
    {"sceNpTusGetFriendsVariableA",ok_void}, {"sceNpTusGetFriendsVariableForCrossSave",ok_void},
    {"sceNpTusGetMultiSlotVariableA",ok_void}, {"sceNpTusGetMultiSlotVariableAVUser",ok_void},
    {"sceNpTusGetMultiSlotVariableForCrossSave",ok_void}, {"sceNpTusGetMultiSlotVariableForCrossSaveVUser",ok_void},
    {"sceNpTusGetMultiUserDataStatusA",ok_void}, {"sceNpTusGetMultiUserDataStatusAVUser",ok_void},
    {"sceNpTusGetMultiUserDataStatusForCrossSave",ok_void}, {"sceNpTusGetMultiUserDataStatusForCrossSaveVUser",ok_void},
    {"sceNpTusGetMultiUserVariableA",ok_void}, {"sceNpTusGetMultiUserVariableAVUser",ok_void},
    {"sceNpTusGetMultiUserVariableForCrossSave",ok_void}, {"sceNpTusGetMultiUserVariableForCrossSaveVUser",ok_void},
    {"sceNpTusDeleteMultiSlotDataA",ok_void}, {"sceNpTusDeleteMultiSlotDataVUser",ok_void},
    {"sceNpSnsFacebookCreateRequest",lib_init_id}, {"sceNpSnsFacebookDeleteRequest",ok_void},
    {"sceNpSnsFacebookGetAccessToken",np_signed_out},
    {"sceNpSnsFacebookDialogInitialize",ok_void}, {"sceNpSnsFacebookDialogOpen",ok_void},
    {"sceNpSnsFacebookDialogUpdateStatus",ok_void}, {"sceNpSnsFacebookDialogTerminate",ok_void},
    {"sceNpSnsFacebookDialogGetResult",ok_void},
    {"sceSigninDialogInitialize",signin_init}, {"sceSigninDialogOpen",signin_open},
    {"sceSigninDialogUpdateStatus",signin_status}, {"sceSigninDialogTerminate",signin_term},
    {"sceSigninDialogGetResult",signin_result},
    {"sceGameCustomDataDialogInitialize",customdata_init}, {"sceGameCustomDataDialogOpenA",customdata_open},
    {"sceGameCustomDataDialogUpdateStatus",customdata_status}, {"sceGameCustomDataDialogTerminate",customdata_term},
    {"sceGameCustomDataDialogGetResultA",customdata_result},
    {"sceNpTrophyAbortHandle",ok_void}, {"sceNpTrophyCaptureScreenshot",ok_void},
    {"sceNpTrophyGetGameIcon",ok_void}, {"sceNpTrophyGetGroupIcon",ok_void},
    {"sceNpTrophyGetGroupInfo",ok_void}, {"sceNpTrophyGetTrophyIcon",ok_void},
    {"sceNpTrophyGetTrophyUnlockState",ok_void}, {"sceNpTrophyShowTrophyList",ok_void},
    {"sceSslGetMemoryPoolStats",ctx_pool_stats}, {"sceHttpUriEscape",http_uri_escape},
    {"sceCommonDialogInitialize",common_init},
    {"sceMsgDialogInitialize",msg_init}, {"sceMsgDialogOpen",msg_open},
    {"sceMsgDialogUpdateStatus",msg_status}, {"sceMsgDialogTerminate",msg_term},
    {"sceMsgDialogGetResult",msg_result},
    {"sceNetCtlInit",netctl_init}, {"sceNetCtlTerm",netctl_term}, {"sceNetResolverAbort",net_resolver_abort},
    {"sceSaveDataDialogInitialize",save_init}, {"sceSaveDataDialogOpen",save_open},
    {"sceSaveDataDialogUpdateStatus",save_status}, {"sceSaveDataDialogTerminate",save_term},
    {"sceNpProfileDialogInitialize",profile_init}, {"sceNpProfileDialogOpen",profile_open},
    {"sceNpProfileDialogOpenA",profile_open},
    {"sceNpProfileDialogUpdateStatus",profile_status}, {"sceNpProfileDialogTerminate",profile_term},
    {"sceNpProfileDialogGetResult",profile_result},
    {"sceNpCommerceDialogInitialize",commerce_init}, {"sceNpCommerceDialogOpen",commerce_open},
    {"sceNpCommerceDialogUpdateStatus",commerce_status}, {"sceNpCommerceDialogTerminate",commerce_term},
    {"sceImeDialogInit",ime_init}, {"sceImeDialogGetStatus",ime_status}, {"sceImeDialogGetResult",ime_result},
    {"sceImeDialogTerm",ime_term}, {"sceImeDialogAbort",ime_term},
    {"sceNpTrophyCreateContext",trophy_context}, {"sceNpTrophyCreateHandle",trophy_handle},
    {"sceNpTrophyRegisterContext",trophy_register}, {"sceNpTrophyUnlockTrophy",trophy_unlock},
    {"sceNpTrophyGetGameInfo",trophy_game_info}, {"sceNpTrophyGetTrophyInfo",trophy_info},
    {"sceNpTrophyGetTrophyUnlockState",trophy_unlock_state},
    {"scePlayGoInitialize",playgo_init}, {"scePlayGoOpen",playgo_open}, {"scePlayGoGetChunkId",playgo_chunk_ids},
    {"scePlayGoGetLocus",playgo_locus}, {"scePlayGoSetInstallSpeed",playgo_speed},
    {"scePlayGoGetInstallSpeed",playgo_get_speed}, {"scePlayGoGetProgress",playgo_progress}, {"scePlayGoGetEta",playgo_eta},
    {"scePlayGoGetToDoList",playgo_get_todo}, {"scePlayGoSetToDoList",playgo_set_todo}, {"scePlayGoPrefetch",playgo_prefetch},
    {"scePlayGoClose",playgo_close}, {"scePlayGoTerminate",playgo_term},
    {"sceSharePlayInitialize",shareplay_init}, {"sceSharePlayTerminate",shareplay_term},
    {"sceSharePlaySetProhibition",shareplay_prohibit}, {"sceSharePlayGetCurrentConnectionInfoA",shareplay_connection},
    {"sceVideoRecordingQueryMemSize2",videorec_memsize}, {"sceVideoRecordingOpen2",videorec_open},
    {"sceVideoRecordingStart",videorec_fail}, {"sceVideoRecordingStop",videorec_fail}, {"sceVideoRecordingClose",videorec_fail},
    {"sceVideoRecordingGetStatus",videorec_status},
    {"sceContentExportInit",contentexport_init}, {"sceContentExportTerm",contentexport_term},
    {"sceContentExportStart",contentexport_fail}, {"sceContentExportFromFile",contentexport_fail}, {"sceContentExportFinish",contentexport_fail},
    {"sceGameLiveStreamingInitialize",livestream_init}, {"sceGameLiveStreamingTerminate",livestream_term},
    {"sceGameLiveStreamingEnableLiveStreaming",livestream_enable},
    {"sceScreenShotEnable",screenshot_toggle}, {"sceScreenShotDisable",screenshot_toggle},
    {"sceNpCheckNpAvailabilityA",np_check_avail}, {"sceNpGetAccountIdA",np_account_id},
    {"sceNpGetParentalControlInfoA",np_parental_control}, {"sceNpGetUserIdByAccountId",np_user_id_by_acc_id},
    {"sceNpAuthGetAuthorizationCodeA",np_auth_code}, {"sceNpAuthGetIdTokenA",np_auth_id_token},
    {"sceNpTrophyDestroyContext",trophy_destroy}, {"sceNpTrophyDestroyHandle",trophy_destroy},
    {"sceHttpAbortRequest",http_abort}, {"sceHttpCreateRequestWithURL2",http_req2},
    {"sceHttpGetAllResponseHeaders",http_all_headers}, {"sceHttpParseResponseHeader",http_parse_header},
    {"sceHttpSetChunkedTransferEnabled",http_ok}, {"sceHttpsSetSslCallback",http_ok},
    {"sceHttpGetMemoryPoolStats",ctx_pool_stats}, {"sceNetGetMemoryPoolStats",ctx_pool_stats},
    {"sceNpInGameMessageGetMemoryPoolStatistics",ctx_pool_stats},
    {"sceNpCommerceDialogGetResult",commerce_result}, {"sceNpCommerceShowPsStoreIcon",commerce_icon}, {"sceNpCommerceHidePsStoreIcon",commerce_icon},
    {"sceWebBrowserDialogInitialize",webbrowser_init}, {"sceWebBrowserDialogOpen",webbrowser_open},
    {"sceWebBrowserDialogUpdateStatus",webbrowser_status}, {"sceWebBrowserDialogTerminate",webbrowser_term},
    {"sceWebBrowserDialogClose",webbrowser_close}, {"sceWebBrowserDialogGetResult",webbrowser_result},
    {"sceInvitationDialogInitialize",invitation_init}, {"sceInvitationDialogOpenA",invitation_open},
    {"sceInvitationDialogUpdateStatus",invitation_status}, {"sceInvitationDialogTerminate",invitation_term},
    {"sceInvitationDialogGetResultA",invitation_result},
    /* libSceNpToolkit2.prx's private entry points into NetCtl, NpManager and UserService: the same
     * contracts as the public ones (disconnected, signed out, one user with no events). */
    {"sceNetCtlRegisterCallbackForNpToolkit",netctl_register}, {"sceNetCtlCheckCallbackForNpToolkit",netctl_check},
    {"sceNetCtlUnregisterCallbackForNpToolkit",netctl_unregister},
    {"sceNpRegisterStateCallbackForToolkit",np_reg_state_cb_toolkit}, {"sceNpUnregisterStateCallbackForToolkit",np_unreg_state_cb_toolkit},
    {"sceNpCheckCallbackForLib",np_check_cb},
    {"sceUserServiceRegisterEventCallback",user_event_register}, {"sceUserServiceUnregisterEventCallback",user_event_unregister},
    {"sceMouseInit",ok_void}, {"sceMouseOpen",mouse_open}, {"sceMouseRead",mouse_read}, {"sceMouseClose",mouse_close},
    {"sceAudioInOpen",audio_in_open},
    {"sceDiscMapIsRequestOnHDD",discmap_on_hdd}, {"sceDiscMap_8A828CAEE7EDD5E9",discmap_8a82},
    {"sceVoiceInit",ok_void}, {"sceVoiceEnd",ok_void},
};
uintptr_t runtime_services_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
