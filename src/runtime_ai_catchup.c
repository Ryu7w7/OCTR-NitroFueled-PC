#define _POSIX_C_SOURCE 200809L
#include "runtime_ai_catchup.h"
#include <math.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* Separate from native packets: nonce echoes prevent a previous race's bonus
 * being adopted even when a CUP repeats the same track. */
typedef struct {
    uint64_t map, nonce, sequence, received, last_receive;
    char host[24];
    int authority, require_peers, level;
    uint8_t bonus[8], mask, recovery, results_valid, results_finished;
    struct { char name[24]; uint64_t nonce, last_send, last_seen, last_hello; } peers[8];
} CatchupSync;
static CatchupSync sync;
static atomic_flag lock = ATOMIC_FLAG_INIT;
static void take(void) { while (atomic_flag_test_and_set_explicit(&lock,memory_order_acquire)) {} }
static void give(void) { atomic_flag_clear_explicit(&lock,memory_order_release); }
uint64_t runtime_ai_catchup_now(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;
#endif
}
static uint64_t read64(const uint8_t *p) {
    uint64_t v=0; for (int i=0;i<8;++i) v=(v<<8)|p[i]; return v;
}
static void write64(uint8_t *p,uint64_t v) {
    for (int i=7;i>=0;--i) { p[i]=(uint8_t)v; v>>=8; }
}
void runtime_ai_catchup_reset(const char *map,const char *host,int authority) {
    static uint64_t generation;
    take();
    memset(&sync,0,sizeof sync); sync.level=-1;
    if (!generation) generation=runtime_ai_catchup_now()<<16;
    sync.nonce=++generation;
    /* The nonce need only differ between this process's consecutive races. */
    if (map && host && *host && strlen(host)<sizeof sync.host) {
        sync.map=UINT64_C(14695981039346656037);
        for (const char *p=map;*p;++p) sync.map=(sync.map^(uint8_t)*p)*UINT64_C(1099511628211);
        memcpy(sync.host,host,strlen(host)+1); sync.authority=authority;
    }
    give();
}
void runtime_ai_catchup_require_peers(int required) {
    take(); sync.require_peers=required; give();
}
static int ready(uint64_t now) {
    if (!sync.map || !sync.authority) return 0;
    if (!sync.require_peers) return 1;
    int peers=0;
    for (int i=0;i<8;++i) if (sync.peers[i].last_seen && now-sync.peers[i].last_seen<=1500) {
        ++peers;
        if (!sync.peers[i].nonce || !sync.peers[i].last_hello || now-sync.peers[i].last_hello>1500) return 0;
    }
    return peers>=sync.require_peers;
}
int runtime_ai_catchup_discovery(void) {
    take();
    /* Team result metadata also needs transport in an eight-human room. */
    const int active=sync.map!=0;
    give(); return active;
}
int runtime_ai_catchup_ready(void) {
    take(); int result=ready(runtime_ai_catchup_now()); give(); return result;
}
void runtime_ai_catchup_publish(unsigned id,float bonus) {
    if (id>=8 || !isfinite(bonus)) return;
    take();
    if (sync.authority) {
        /* Both ends use the same quantized value, including the authority. */
        sync.bonus[id]=(uint8_t)lroundf(fminf(fmaxf(bonus,0),2.5f)*100);
        sync.mask|=(uint8_t)(1u<<id); ++sync.sequence;
    }
    give();
}
void runtime_ai_catchup_level_publish(int level) {
    take();
    if (sync.authority && level>=0 && level<=5) { sync.level=level; ++sync.sequence; }
    give();
}
int runtime_ai_catchup_level(void) {
    take();
    int level=sync.last_receive && runtime_ai_catchup_now()-sync.last_receive<=1500 ? sync.level : -1;
    give(); return level;
}
float runtime_ai_catchup_bonus(unsigned id) {
    float bonus=0;
    take();
    const uint64_t now=runtime_ai_catchup_now();
    if (id<8 && (sync.mask&(1u<<id)) &&
        (sync.authority ? ready(now) : (sync.last_receive && now-sync.last_receive<=1500))) bonus=sync.bonus[id]*.01f;
    give(); return bonus;
}
void runtime_ai_catchup_recovery_publish(unsigned id,int active) {
    if (id>=8) return;
    take();
    if (sync.authority) {
        uint8_t bit=(uint8_t)(1u<<id);
        uint8_t value=active ? sync.recovery|bit : sync.recovery&~bit;
        if (value!=sync.recovery) { sync.recovery=value; ++sync.sequence; }
    }
    give();
}
int runtime_ai_catchup_recovery(unsigned id) {
    take();
    const uint64_t now=runtime_ai_catchup_now();
    int active=id<8 && (sync.recovery&(1u<<id)) &&
        (sync.authority ? ready(now) : (sync.last_receive && now-sync.last_receive<=1500));
    give(); return active;
}
void runtime_ai_catchup_results_publish(unsigned finished_mask) {
    if (finished_mask>255) return;
    take();
    if (sync.map && sync.authority && !sync.results_valid) {
        sync.results_finished=(uint8_t)finished_mask; sync.results_valid=1; ++sync.sequence;
    }
    give();
}
int runtime_ai_catchup_results(void) {
    take();
    const int result=sync.results_valid ? sync.results_finished : -1;
    give(); return result;
}
static int peer_slot(const char *name) {
    if (!name || !*name || strlen(name)>=24) return -1;
    for (int i=0;i<8;++i) if (!strcmp(sync.peers[i].name,name)) return i;
    for (int i=0;i<8;++i) if (!*sync.peers[i].name) {
        memcpy(sync.peers[i].name,name,strlen(name)+1); return i;
    }
    return -1;
}
size_t runtime_ai_catchup_send(const char *peer,uint8_t *out,uint64_t now) {
    size_t n=0;
    take();
    if (!sync.map || !peer || (!sync.authority && strcmp(peer,sync.host))) goto done;
    int slot=peer_slot(peer);
    if (slot<0) goto done;
    sync.peers[slot].last_seen=now;
    if ((sync.peers[slot].last_send && now-sync.peers[slot].last_send<50)) goto done;
    if (sync.authority && !sync.peers[slot].nonce) goto done;
    memset(out,0,AI_CATCHUP_PACKET); memcpy(out,"RYUPN1",6); out[6]=sync.authority ? 4 : 3; out[7]=1;
    write64(out+8,sync.map);
    write64(out+16,sync.authority ? sync.peers[slot].nonce : sync.nonce);
    write64(out+24,sync.sequence);
    if (sync.authority) { out[43]=sync.results_valid; out[44]=sync.results_finished; out[32]=sync.mask; out[41]=(uint8_t)(sync.level+1); if (ready(now)) { memcpy(out+33,sync.bonus,8); out[42]=sync.recovery; } }
    sync.peers[slot].last_send=now; n=AI_CATCHUP_PACKET;
done:
    give(); return n;
}
int runtime_ai_catchup_receive(const char *peer,const uint8_t *in,size_t len,uint64_t now) {
    if (len<7 || memcmp(in,"RYUPN1",6) || (in[6]!=3 && in[6]!=4)) return 0;
    if (len!=AI_CATCHUP_PACKET || in[7]!=1 || !peer || !*peer) return 1;
    int hello=0, snapshot=0;
    take();
    if (!sync.map || read64(in+8)!=sync.map) goto done;
    if (in[6]==3 && sync.authority) {
        int slot=peer_slot(peer);
        if (slot>=0) { hello=sync.peers[slot].nonce!=read64(in+16); sync.peers[slot].nonce=read64(in+16); sync.peers[slot].last_hello=now; }
    } else if (in[6]==4 && !sync.authority && !strcmp(peer,sync.host) &&
               read64(in+16)==sync.nonce && read64(in+24)>sync.received) {
        for (int i=0;i<8;++i) if (in[33+i]>250) goto done;
        if (in[41]>6 || in[43]>1) goto done;
        snapshot=!sync.received;
        sync.level=(int)in[41]-1;
        sync.received=read64(in+24); sync.last_receive=now;
        sync.mask=in[32]; sync.recovery=in[42]; memcpy(sync.bonus,in+33,8);
        if (in[43] && !sync.results_valid) {
            sync.results_finished=in[44]; sync.results_valid=1;
        }
    }
done:
    give();
    if (hello) puts("Native AI catch-up sync: guest handshake accepted");
    if (snapshot) puts("Native AI catch-up sync: host snapshot accepted");
    return 1;
}

float runtime_ai_catchup_target(int level,float gap,float human_speed,float ai_speed,float previous) {
    static const float seconds[]={18,14,11,8,6,4};
    static const float caps[]={.12f,.22f,.38f,.60f,.90f,1.20f};
    if (level<0 || level>5 || !isfinite(gap) || !isfinite(human_speed) ||
        !isfinite(ai_speed) || !isfinite(previous) || gap<=0 || human_speed<=0 || ai_speed<=0) return 0;
    const float baseline=ai_speed/(1+fmaxf(previous,0));
    const float closing=fminf(gap/seconds[level],baseline*caps[level]);
    const float desired=(human_speed+closing)/baseline-1;
    return fminf(2.5f,fmaxf(0,desired));
}
