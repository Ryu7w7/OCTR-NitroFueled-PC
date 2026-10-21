/* SacredFire: kernel event flags (libkernel sceKernelCreateEventFlag and friends).
 * A flag is a 64-bit pattern with waiters; a waiter asks for a pattern with AND or OR semantics and
 * may clear all bits or its pattern when satisfied. Single-waiter flags (attr 0x10) refuse a second
 * waiter with EPERM; multi-waiter flags (attr 0x20) wake every satisfied waiter in FIFO order.
 * Timeouts are microseconds and are updated with the remaining time, as on the console. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define WAIT_AND 0x01
#define WAIT_OR 0x02
#define CLEAR_ALL 0x10
#define CLEAR_PAT 0x20
#define ATTR_SINGLE 0x10
#define ATTR_MULTI 0x20
typedef struct EfWaiter {
    uint64_t pattern; uint32_t mode;
    uint64_t result; int32_t status; int done;
    struct EfWaiter *next;
} EfWaiter;
typedef struct EventFlag {
    uint32_t id, attr;
    uint64_t bits;
    EfWaiter *first;
    unsigned waiters;
    struct EventFlag *next;
} EventFlag;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
/* Deadlines are on WAIT_CLOCK, so the condition variable must use that clock too. winpthreads
 * condition variables accept only CLOCK_REALTIME (pthread_condattr_setclock(CLOCK_MONOTONIC)
 * fails with EINVAL), as in runtime_sema.c. */
#ifdef _WIN32
#define WAIT_CLOCK CLOCK_REALTIME
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
#endif
static pthread_cond_t changed;
static pthread_once_t changed_once=PTHREAD_ONCE_INIT;
static void init_changed(void) {
    pthread_condattr_t a; pthread_condattr_init(&a);
#ifndef _WIN32
    pthread_condattr_setclock(&a,WAIT_CLOCK);
#endif
    pthread_cond_init(&changed,&a); pthread_condattr_destroy(&a);
}
static EventFlag *registry;
static uint32_t next_id=1;
static size_t created, deleted, waits, sets, timeouts;

static EventFlag *find(uint32_t id) {
    for (EventFlag *f=registry;f;f=f->next) if (f->id==id) return f;
    return NULL;
}
static int satisfied(uint64_t bits,uint64_t pattern,uint32_t mode) {
    return (mode&WAIT_AND) ? (bits&pattern)==pattern : (bits&pattern)!=0;
}
/* Under the lock: wake every waiter the current bits satisfy; the clear modes apply in wake order. */
static void settle(EventFlag *f) {
    EfWaiter **link=&f->first;
    while (*link) {
        EfWaiter *w=*link;
        if (!w->done && satisfied(f->bits,w->pattern,w->mode)) {
            w->result=f->bits; w->status=0; w->done=1;
            if (w->mode&CLEAR_ALL) f->bits=0;
            else if (w->mode&CLEAR_PAT) f->bits&=~w->pattern;
            *link=w->next; --f->waiters;
        } else link=&w->next;
    }
}
static ABI int32_t ef_create(uint32_t *out,const char *name,uint32_t attr,uint64_t initial,const void *options) {
    if (!out || !name) return ERR(22);
    uint32_t kind=attr&(ATTR_SINGLE|ATTR_MULTI);
    if (kind==(ATTR_SINGLE|ATTR_MULTI) || (attr&~(uint32_t)(ATTR_SINGLE|ATTR_MULTI|0x3))) return ERR(22);
    if (options) { fputs("STOP: event flag options are not implemented\n",stderr); exit(21); }
    EventFlag *f=calloc(1,sizeof(*f));
    if (!f) return ERR(12);
    pthread_mutex_lock(&lock);
    f->id=next_id++; f->attr=kind ? kind : ATTR_SINGLE; f->bits=initial;
    f->next=registry; registry=f; *out=f->id; ++created;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t ef_delete(uint32_t id) {
    pthread_once(&changed_once,init_changed);
    pthread_mutex_lock(&lock);
    EventFlag **link=&registry;
    while (*link && (*link)->id!=id) link=&(*link)->next;
    EventFlag *f=*link;
    if (!f) { pthread_mutex_unlock(&lock); return ERR(3); }
    /* Waiters of a deleted flag wake with EACCES, as the console reports. */
    for (EfWaiter *w=f->first;w;w=w->next) { w->status=ERR(13); w->done=1; }
    f->first=NULL; f->waiters=0;
    *link=f->next; ++deleted;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    free(f);
    return 0;
}
static ABI int32_t ef_set(uint32_t id,uint64_t bits) {
    pthread_once(&changed_once,init_changed);
    pthread_mutex_lock(&lock);
    EventFlag *f=find(id);
    if (!f) { pthread_mutex_unlock(&lock); return ERR(3); }
    f->bits|=bits; ++sets;
    settle(f);
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t ef_clear(uint32_t id,uint64_t bits) {
    pthread_mutex_lock(&lock);
    EventFlag *f=find(id);
    if (!f) { pthread_mutex_unlock(&lock); return ERR(3); }
    f->bits&=bits; /* the argument is the mask of bits to keep */
    pthread_mutex_unlock(&lock);
    return 0;
}
static int32_t wait_pattern(uint32_t id,uint64_t pattern,uint32_t mode,uint64_t *result,uint32_t *timeout,int block) {
    pthread_once(&changed_once,init_changed);
    uint32_t wait_mode=mode&(WAIT_AND|WAIT_OR), clear=mode&(CLEAR_ALL|CLEAR_PAT);
    if (!pattern || (wait_mode!=WAIT_AND && wait_mode!=WAIT_OR) || clear==(CLEAR_ALL|CLEAR_PAT) || (mode&~(uint32_t)0x33)) return ERR(22);
    pthread_mutex_lock(&lock);
    EventFlag *f=find(id);
    if (!f) { pthread_mutex_unlock(&lock); return ERR(3); }
    if (satisfied(f->bits,pattern,mode)) {
        if (result) *result=f->bits;
        if (mode&CLEAR_ALL) f->bits=0; else if (mode&CLEAR_PAT) f->bits&=~pattern;
        pthread_mutex_unlock(&lock);
        return 0;
    }
    if (!block) { pthread_mutex_unlock(&lock); return ERR(16); } /* EBUSY: poll not satisfied */
    if (f->attr==ATTR_SINGLE && f->waiters) { pthread_mutex_unlock(&lock); return ERR(1); } /* EPERM */
    EfWaiter w={.pattern=pattern,.mode=mode};
    EfWaiter **link=&f->first;
    while (*link) link=&(*link)->next;
    *link=&w; ++f->waiters; ++waits;
    struct timespec deadline; uint64_t start_ns=0;
    if (timeout) {
        clock_gettime(WAIT_CLOCK,&deadline);
        start_ns=(uint64_t)deadline.tv_sec*1000000000+(uint64_t)deadline.tv_nsec;
        uint64_t total=start_ns+(uint64_t)*timeout*1000;
        deadline.tv_sec=(time_t)(total/1000000000); deadline.tv_nsec=(long)(total%1000000000);
    }
    int32_t status=0;
    while (!w.done) {
        int e= timeout ? pthread_cond_timedwait(&changed,&lock,&deadline) : pthread_cond_wait(&changed,&lock);
        if (e==ETIMEDOUT && !w.done) {
            /* Remove ourselves; the flag may have been deleted meanwhile (then w.done is set). */
            EventFlag *g=find(id);
            if (g) { for (EfWaiter **l=&g->first;*l;l=&(*l)->next) if (*l==&w) { *l=w.next; --g->waiters; break; } }
            status=ERR(60); ++timeouts; break;
        }
    }
    if (w.done) status=w.status;
    if (!status && result) *result=w.result;
    if (timeout) {
        struct timespec now; clock_gettime(WAIT_CLOCK,&now);
        uint64_t spent=((uint64_t)now.tv_sec*1000000000+(uint64_t)now.tv_nsec-start_ns)/1000;
        *timeout= spent>=*timeout ? 0 : (uint32_t)(*timeout-spent);
    }
    pthread_mutex_unlock(&lock);
    return status;
}
static ABI int32_t ef_wait(uint32_t id,uint64_t pattern,uint32_t mode,uint64_t *result,uint32_t *timeout) { return wait_pattern(id,pattern,mode,result,timeout,1); }
static ABI int32_t ef_poll(uint32_t id,uint64_t pattern,uint32_t mode,uint64_t *result) { return wait_pattern(id,pattern,mode,result,NULL,0); }
static ABI int32_t ef_cancel(uint32_t id,uint64_t pattern,int32_t *waiters) {
    pthread_once(&changed_once,init_changed);
    pthread_mutex_lock(&lock);
    EventFlag *f=find(id);
    if (!f) { pthread_mutex_unlock(&lock); return ERR(3); }
    if (waiters) *waiters=(int32_t)f->waiters;
    for (EfWaiter *w=f->first;w;w=w->next) { w->status=ERR(85); w->done=1; } /* ECANCELED */
    f->first=NULL; f->waiters=0; f->bits=pattern;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return 0;
}
static const RuntimeExport exports[]={
    {"sceKernelCreateEventFlag",ef_create}, {"sceKernelDeleteEventFlag",ef_delete},
    {"sceKernelSetEventFlag",ef_set}, {"sceKernelClearEventFlag",ef_clear},
    {"sceKernelWaitEventFlag",ef_wait}, {"sceKernelPollEventFlag",ef_poll},
    {"sceKernelCancelEventFlag",ef_cancel},
};
uintptr_t runtime_eventflag_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_eventflag_report(void) {
    printf("Runtime: event flags created=%zu, deleted=%zu, waits=%zu, sets=%zu, timeouts=%zu\n",created,deleted,waits,sets,timeouts);
}
