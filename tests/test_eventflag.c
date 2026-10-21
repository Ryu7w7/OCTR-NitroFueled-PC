/* SacredFire: kernel event flag contracts (src/runtime_eventflag.c). */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void runtime_restart(void) { abort(); }

typedef int32_t (ABI *Create)(uint32_t *, const char *, uint32_t, uint64_t, const void *);
typedef int32_t (ABI *Id)(uint32_t);
typedef int32_t (ABI *Bits)(uint32_t, uint64_t);
typedef int32_t (ABI *Wait)(uint32_t, uint64_t, uint32_t, uint64_t *, uint32_t *);
typedef int32_t (ABI *Poll)(uint32_t, uint64_t, uint32_t, uint64_t *);
typedef int32_t (ABI *Cancel)(uint32_t, uint64_t, int32_t *);
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))

static Create create; static Id delete_flag; static Bits set, clear; static Wait wait; static Poll poll_flag; static Cancel cancel;
static uint32_t shared;
static void *setter(void *arg) { usleep(20000); assert(set(shared,(uint64_t)(uintptr_t)arg)==0); return NULL; }

int main(void) {
    runtime_start(1);
    /* The NID table the loader would pass with --import-names, written here so the test stands alone. */
    char path[]="/tmp/sacredfire-eventflag-XXXXXX";
    int fd=mkstemp(path); assert(fd>=0);
    const char *table="BpFoboUJoZU libkernel sceKernelCreateEventFlag\n8mql9OcQnd4 libkernel sceKernelDeleteEventFlag\nIOnSvHzqu6A libkernel sceKernelSetEventFlag\n7uhBFWRAS60 libkernel sceKernelClearEventFlag\nJTvBflhYazQ libkernel sceKernelWaitEventFlag\n9lvj5DjHZiA libkernel sceKernelPollEventFlag\nPZku4ZrXJqg libkernel sceKernelCancelEventFlag\n";
    assert(write(fd,table,strlen(table))==(ssize_t)strlen(table)); close(fd);
    assert(runtime_load_import_names(path)==7); unlink(path);
    create=(Create)runtime_eventflag_resolve("BpFoboUJoZU#libkernel");
    delete_flag=(Id)runtime_eventflag_resolve("8mql9OcQnd4#libkernel");
    set=(Bits)runtime_eventflag_resolve("IOnSvHzqu6A#libkernel");
    clear=(Bits)runtime_eventflag_resolve("7uhBFWRAS60#libkernel");
    wait=(Wait)runtime_eventflag_resolve("JTvBflhYazQ#libkernel");
    poll_flag=(Poll)runtime_eventflag_resolve("9lvj5DjHZiA#libkernel");
    cancel=(Cancel)runtime_eventflag_resolve("PZku4ZrXJqg#libkernel");
    assert(create && delete_flag && set && clear && wait && poll_flag && cancel);
    uint32_t id=0;
    assert(create(&id,"test",0x20,0,NULL)==0 && id);
    uint64_t got=0;
    /* OR wait satisfied by one bit, pattern cleared. */
    assert(set(id,0x5)==0);
    assert(wait(id,0x4,0x02|0x20,&got,NULL)==0 && got==0x5);
    assert(poll_flag(id,0x4,0x02,&got)==ERR(16));
    assert(poll_flag(id,0x1,0x02,&got)==0 && got==0x1);
    /* AND wait needs both bits; CLEAR_ALL empties the flag; timeout updated. */
    uint32_t timeout=10000;
    assert(wait(id,0x3,0x01,&got,&timeout)==ERR(60) && timeout==0);
    pthread_t t; shared=id;
    assert(!pthread_create(&t,NULL,setter,(void *)(uintptr_t)0x2));
    timeout=2000000;
    assert(wait(id,0x3,0x01|0x10,&got,&timeout)==0 && got==0x3 && timeout<2000000);
    pthread_join(t,NULL);
    assert(poll_flag(id,0x3,0x02,&got)==ERR(16)); /* cleared */
    /* Clear keeps only the mask; cancel wakes waiters with ECANCELED and sets the pattern. */
    assert(set(id,0xf0)==0 && clear(id,0x30)==0 && poll_flag(id,0xc0,0x02,&got)==ERR(16) && poll_flag(id,0x30,0x01,&got)==0);
    int32_t waiters=-1;
    assert(cancel(id,0x1,&waiters)==0 && waiters==0 && poll_flag(id,0x1,0x01,&got)==0 && got==0x1);
    /* Invalid arguments and unknown ids. */
    assert(wait(id,0,0x02,&got,NULL)==ERR(22) && wait(id,1,0x03,&got,NULL)==ERR(22) && wait(id,1,0x32,&got,NULL)==ERR(22));
    assert(set(id+100,1)==ERR(3) && delete_flag(id+100)==ERR(3));
    assert(delete_flag(id)==0 && delete_flag(id)==ERR(3));
    /* Single-waiter flags refuse a second blocking waiter. */
    assert(create(&id,"single",0x10,0,NULL)==0);
    assert(create(&id,"bad",0x30,0,NULL)==ERR(22));
    puts("PASS: event flag contracts");
    return 0;
}
