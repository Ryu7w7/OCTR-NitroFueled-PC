/* Linux pthread-backed opaque PS4 mutex handles. No guest threads yet. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#define MUTEX_MAGIC UINT32_C(0x4d555458)
#define COND_MAGIC  UINT32_C(0x434f4e44)

typedef struct { int type; } GuestAttr;
/* The game's libc takes a mutex handle for libkernel's struct pthread_mutex: right after creating
 * a heap's mutex (0x2c4ebed, 0x2c4ee91, 0x2c50aa2 in the linked image) it sets that struct's
 * m_flags itself, or dword [handle+0x20],0x100. ps4_view gives such writes room inside this
 * object. With a 16-byte object the write landed in the next heap block, usually winpthread's own
 * object for the native mutex, whose function table pointer then gained 0x100: the first malloc
 * crashed in pthread_mutex_trylock, or the heap was never created (code 3 at start, about one
 * start in two on Windows). */
typedef struct {
    uint32_t magic;
    uint32_t destroyed;
    unsigned char ps4_view[0x100 - 8];
    pthread_mutex_t native;
} GuestMutex;
_Static_assert(offsetof(GuestMutex, ps4_view) <= 0x20 && offsetof(GuestMutex, native) >= 0x100,
               "libkernel's pthread_mutex fields the libc writes must fall in ps4_view");
static size_t created, locks, unlocks;
static int32_t orbis_error(int e) {
    if (!e) return 0;
    unsigned code;
    switch (e) {
    case EPERM: code=1; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case EDEADLK: code=11; break;
    default: fprintf(stderr, "STOP: unmapped host pthread error %d\n", e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000) | code);
}
static ABI int32_t attr_init(GuestAttr **out) {
    if (!out) return orbis_error(EINVAL);
    GuestAttr *attr = malloc(sizeof(*attr));
    if (!attr) return orbis_error(ENOMEM);
    attr->type = 1; *out = attr; return 0;
}
static ABI int32_t attr_type(GuestAttr **attr, int type) {
    if (!attr || !*attr || type < 1 || type > 4) return orbis_error(EINVAL);
    (*attr)->type = type; return 0;
}
/* Priority protocols need a PS4 scheduler; the value is validated and recorded only. */
static ABI int32_t attr_protocol(GuestAttr **attr, int protocol) {
    if (!attr || !*attr || protocol < 0 || protocol > 2) return orbis_error(EINVAL);
    return 0;
}
static ABI int32_t attr_destroy(GuestAttr **attr) {
    if (!attr || !*attr) return orbis_error(EINVAL);
    free(*attr); *attr = NULL; return 0;
}
static ABI int32_t mutex_init(GuestMutex **out, GuestAttr **attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return orbis_error(EINVAL);
    int type = attr ? (*attr)->type : 1;
    if (type < 1 || type > 4) return orbis_error(EINVAL);
    GuestMutex *mutex = calloc(1, sizeof(*mutex));
    if (!mutex) return orbis_error(ENOMEM);
    mutex->magic = MUTEX_MAGIC;
    pthread_mutexattr_t native_attr;
    int e = pthread_mutexattr_init(&native_attr);
    if (!e) {
        /* PS4 types: 1 errorcheck (the default), 2 recursive, 3 normal, 4 adaptive (a normal
           mutex that spins first: no error checking either). */
        int native_type = type == 2 ? PTHREAD_MUTEX_RECURSIVE :
                          type == 1 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL;
        e = pthread_mutexattr_settype(&native_attr, native_type);
        if (!e) e = pthread_mutex_init(&mutex->native, &native_attr);
        pthread_mutexattr_destroy(&native_attr);
    }
    if (e) { free(mutex); return orbis_error(e); }
    *out = mutex; ++created; return 0;
}
static pthread_mutex_t static_init = PTHREAD_MUTEX_INITIALIZER;

static inline int is_valid_ptr(const void *p) {
    uintptr_t v = (uintptr_t)p;
    return v >= 0x10000 && v <= 0x00007fffffffffffULL;
}

/* Statically initialized handles (0/1) are created once, even under contention. */
static int32_t ensure_mutex(GuestMutex **mutex) {
    if (!mutex) return orbis_error(EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)mutex, __ATOMIC_ACQUIRE);
    if (value == 2 || value == (uintptr_t)-1) return orbis_error(EINVAL);
    if (value >= 2) {
        if (!is_valid_ptr((void *)value)) return orbis_error(EINVAL);
        GuestMutex *m = (GuestMutex *)value;
        if (m->magic == MUTEX_MAGIC) {
            if (m->destroyed) return orbis_error(EINVAL);
            return 0;
        }
        return orbis_error(EINVAL);
    }
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*mutex < 2) {
        GuestMutex *created_mutex = NULL;
        e = mutex_init(&created_mutex, NULL, NULL);
        if (!e) __atomic_store_n(mutex, created_mutex, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
static ABI int32_t mutex_lock(GuestMutex **mutex) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    GuestMutex *m = *mutex;
    if (!m || m->magic != MUTEX_MAGIC || m->destroyed) return orbis_error(EINVAL);
    int r = pthread_mutex_trylock(&m->native);
    if (r == EBUSY) { /* contended: timed for the wait profile */
        const uint64_t start = runtime_wait_clock();
        r = pthread_mutex_lock(&m->native);
        runtime_wait_note(1, runtime_wait_clock() - start);
    }
    e = orbis_error(r);
    if (!e) ++locks;
    return e;
}
static ABI int32_t mutex_trylock(GuestMutex **mutex) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    GuestMutex *m = *mutex;
    if (!m || m->magic != MUTEX_MAGIC || m->destroyed) return orbis_error(EINVAL);
    e = orbis_error(pthread_mutex_trylock(&m->native));
    if (!e) ++locks;
    return e;
}
static ABI int32_t mutex_unlock(GuestMutex **mutex) {
    if (!mutex || !is_valid_ptr(*mutex)) return orbis_error(EINVAL);
    GuestMutex *m = *mutex;
    if (m->magic != MUTEX_MAGIC || m->destroyed) return orbis_error(EINVAL);
    int32_t e = orbis_error(pthread_mutex_unlock(&m->native));
    if (!e) ++unlocks;
    return e;
}
static ABI int32_t mutex_destroy(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2 || (uintptr_t)*mutex == (uintptr_t)-1) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return 0;
    if (!is_valid_ptr(*mutex)) return orbis_error(EINVAL);
    GuestMutex *m = *mutex;
    if (m->magic != MUTEX_MAGIC || m->destroyed) return orbis_error(EINVAL);
    int e = pthread_mutex_destroy(&m->native);
    if (e) return orbis_error(e);
    m->destroyed = 1;
    *mutex = (GuestMutex *)(uintptr_t)2;
    return 0;
}
static int deadline_after(struct timespec *end, uint64_t usec) {
    if (clock_gettime(CLOCK_REALTIME, end)) return errno;
    uint64_t ns = (uint64_t)end->tv_nsec + (usec % 1000000) * 1000;
    end->tv_sec += (time_t)(usec / 1000000 + ns / 1000000000);
    end->tv_nsec = (long)(ns % 1000000000);
    return 0;
}
static int32_t timed_error(int e) { return e == ETIMEDOUT ? (int32_t)UINT32_C(0x8002003c) : orbis_error(e); }
static ABI int32_t mutex_timedlock(GuestMutex **mutex, uint32_t usec) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    GuestMutex *m = *mutex;
    if (!m || m->magic != MUTEX_MAGIC || m->destroyed) return orbis_error(EINVAL);
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    e = timed_error(pthread_mutex_timedlock(&m->native, &end));
    if (!e) ++locks;
    return e;
}
/* Condition variables: opaque handles over host pthread_cond (CLOCK_REALTIME,
 * like FreeBSD's default), sharing the host mutex behind GuestMutex. */
typedef struct {
    uint32_t magic;
    uint32_t destroyed;
    pthread_cond_t native;
} GuestCond;
static size_t conds, waits, wakeups;
static ABI int32_t cond_init(GuestCond **out, void **attr, const char *name) {
    (void)attr; (void)name;
    if (!out) return orbis_error(EINVAL);
    GuestCond *c = calloc(1, sizeof(*c));
    if (!c) return orbis_error(ENOMEM);
    c->magic = COND_MAGIC;
    int e = pthread_cond_init(&c->native, NULL);
    if (e) { free(c); return orbis_error(e); }
    *out = c; ++conds; return 0;
}
static int32_t ensure_cond(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)cond, __ATOMIC_ACQUIRE);
    if (value == 2 || value == (uintptr_t)-1) return orbis_error(EINVAL);
    if (value >= 2) {
        if (!is_valid_ptr((void *)value)) return orbis_error(EINVAL);
        GuestCond *c = (GuestCond *)value;
        if (c->magic == COND_MAGIC) {
            if (c->destroyed) return orbis_error(EINVAL);
            return 0;
        }
        return orbis_error(EINVAL);
    }
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*cond < 2) {
        GuestCond *c = NULL;
        e = cond_init(&c, NULL, NULL);
        if (!e) __atomic_store_n(cond, c, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
static ABI int32_t cond_destroy(GuestCond **cond) {
    if (!cond || (uintptr_t)*cond == (uintptr_t)-1) return orbis_error(EINVAL);
    if ((uintptr_t)*cond < 2) return 0;
    if (!is_valid_ptr(*cond)) return orbis_error(EINVAL);
    GuestCond *c = *cond;
    if (c->magic != COND_MAGIC || c->destroyed) return orbis_error(EINVAL);
    int e = pthread_cond_destroy(&c->native);
    if (e) return orbis_error(e);
    c->destroyed = 1;
    *cond = NULL;
    return 0;
}
static ABI int32_t cond_wait(GuestCond **cond, GuestMutex **mutex) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    int32_t em = ensure_mutex(mutex);
    if (em) return em;
    GuestCond *c = *cond;
    GuestMutex *m = *mutex;
    if (!c || c->magic != COND_MAGIC || c->destroyed || !m || m->magic != MUTEX_MAGIC || m->destroyed)
        return orbis_error(EINVAL);
    ++waits;
    const uint64_t start = runtime_wait_clock();
    const int e2 = pthread_cond_wait(&c->native, &m->native);
    runtime_wait_note(0, runtime_wait_clock() - start);
    return orbis_error(e2);
}
static int32_t cond_wait_until(GuestCond **cond, GuestMutex **mutex, const struct timespec *end) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    int32_t em = ensure_mutex(mutex);
    if (em) return em;
    GuestCond *c = *cond;
    GuestMutex *m = *mutex;
    if (!c || c->magic != COND_MAGIC || c->destroyed || !m || m->magic != MUTEX_MAGIC || m->destroyed || !end)
        return orbis_error(EINVAL);
    ++waits;
    const uint64_t start = runtime_wait_clock();
    const int e2 = pthread_cond_timedwait(&c->native, &m->native, end);
    runtime_wait_note(0, runtime_wait_clock() - start);
    return timed_error(e2);
}
static ABI int32_t cond_timedwait(GuestCond **cond, GuestMutex **mutex, uint32_t usec) {
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    return cond_wait_until(cond, mutex, &end);
}
static ABI int32_t cond_signal(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    GuestCond *c = *cond;
    if (!c || c->magic != COND_MAGIC || c->destroyed) return orbis_error(EINVAL);
    ++wakeups; return orbis_error(pthread_cond_signal(&c->native));
}
static ABI int32_t cond_broadcast(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    GuestCond *c = *cond;
    if (!c || c->magic != COND_MAGIC || c->destroyed) return orbis_error(EINVAL);
    ++wakeups; return orbis_error(pthread_cond_broadcast(&c->native));
}
/* PS4 struct timespec is {int64 sec, int64 nsec}, identical to Linux x86-64. */
typedef struct { int64_t sec, nsec; } GuestTimespec;
static ABI int32_t cond_abs_timedwait(GuestCond **cond, GuestMutex **mutex, const GuestTimespec *abs) {
    if (!abs || abs->nsec < 0 || abs->nsec >= 1000000000) return orbis_error(EINVAL);
    struct timespec end = {.tv_sec = (time_t)abs->sec, .tv_nsec = (long)abs->nsec};
    return cond_wait_until(cond, mutex, &end);
}
/* POSIX pthread APIs return positive guest errno values, unlike scePthread. */
static int32_t posix_result(int32_t result) { return result ? (int32_t)((uint32_t)result & 0xffff) : 0; }
static ABI int32_t posix_attr_init(GuestAttr **out) { return posix_result(attr_init(out)); }
static ABI int32_t posix_attr_type(GuestAttr **attr,int type) { return posix_result(attr_type(attr,type)); }
static ABI int32_t posix_attr_destroy(GuestAttr **attr) { return posix_result(attr_destroy(attr)); }
static ABI int32_t posix_mutex_init(GuestMutex **out,GuestAttr **attr) { return posix_result(mutex_init(out,attr,NULL)); }
static ABI int32_t posix_mutex_destroy(GuestMutex **m) { return posix_result(mutex_destroy(m)); }
static ABI int32_t posix_mutex_lock(GuestMutex **m) { return posix_result(mutex_lock(m)); }
static ABI int32_t posix_mutex_trylock(GuestMutex **m) { return posix_result(mutex_trylock(m)); }
static ABI int32_t posix_mutex_unlock(GuestMutex **m) { return posix_result(mutex_unlock(m)); }
static ABI int32_t posix_cond_init(GuestCond **c, void **a) { return posix_result(cond_init(c, a, NULL)); }
static ABI int32_t posix_cond_destroy(GuestCond **c) { return posix_result(cond_destroy(c)); }
static ABI int32_t posix_cond_wait(GuestCond **c, GuestMutex **m) { return posix_result(cond_wait(c, m)); }
static ABI int32_t posix_cond_timedwait(GuestCond **c, GuestMutex **m, const GuestTimespec *t) { return posix_result(cond_abs_timedwait(c, m, t)); }
static ABI int32_t posix_cond_signal(GuestCond **c) { return posix_result(cond_signal(c)); }
static ABI int32_t posix_cond_broadcast(GuestCond **c) { return posix_result(cond_broadcast(c)); }
uintptr_t runtime_mutex_resolve(const char *name) {
    if (runtime_nid_equals(name,"0TyVk4MSLt0")) return (uintptr_t)posix_cond_init;
    if (runtime_nid_equals(name,"RXXqi4CtF8w")) return (uintptr_t)posix_cond_destroy;
    if (runtime_nid_equals(name,"Op8TBGY5KHg")) return (uintptr_t)posix_cond_wait;
    if (runtime_nid_equals(name,"27bAgiJmOh0")) return (uintptr_t)posix_cond_timedwait;
    if (runtime_nid_equals(name,"2MOy+rUfuhQ")) return (uintptr_t)posix_cond_signal;
    if (runtime_nid_equals(name,"mkx2fVhNMsg")) return (uintptr_t)posix_cond_broadcast;
    if (runtime_nid_equals(name,"2Tb92quprl0#libkernel")) return (uintptr_t)cond_init;
    if (runtime_nid_equals(name,"g+PZd2hiacg#libkernel")) return (uintptr_t)cond_destroy;
    if (runtime_nid_equals(name,"WKAXJ4XBPQ4#libkernel")) return (uintptr_t)cond_wait;
    if (runtime_nid_equals(name,"BmMjYxmew1w#libkernel")) return (uintptr_t)cond_timedwait;
    if (runtime_nid_equals(name,"kDh-NfxgMtE#libkernel")) return (uintptr_t)cond_signal;
    if (runtime_nid_equals(name,"JGgj7Uvrl+A#libkernel")) return (uintptr_t)cond_broadcast;
    if (runtime_nid_equals(name,"IafI2PxcPnQ#libkernel")) return (uintptr_t)mutex_timedlock;
    if (runtime_nid_equals(name,"dQHWEsJtoE4")) return (uintptr_t)posix_attr_init;
    if (runtime_nid_equals(name,"mDmgMOGVUqg")) return (uintptr_t)posix_attr_type;
    if (runtime_nid_equals(name,"HF7lK46xzjY")) return (uintptr_t)posix_attr_destroy;
    if (runtime_nid_equals(name,"ttHNfU+qDBU")) return (uintptr_t)posix_mutex_init;
    if (runtime_nid_equals(name,"ltCfaGr2JGE")) return (uintptr_t)posix_mutex_destroy;
    if (runtime_nid_equals(name,"7H0iTOciTLo")) return (uintptr_t)posix_mutex_lock;
    if (runtime_nid_equals(name,"K-jXhbt2gn4")) return (uintptr_t)posix_mutex_trylock;
    if (runtime_nid_equals(name,"2Z+PpY6CaJg")) return (uintptr_t)posix_mutex_unlock;
    if (runtime_nid_equals(name,"F8bUHwAG284#libkernel")) return (uintptr_t)attr_init;
    if (runtime_nid_equals(name,"iMp8QpE+XO4#libkernel")) return (uintptr_t)attr_type;
    if (runtime_nid_equals(name,"smWEktiyyG0#libkernel")) return (uintptr_t)attr_destroy;
    if (runtime_nid_equals(name,"1FGvU0i9saQ#libkernel")) return (uintptr_t)attr_protocol;
    if (runtime_nid_equals(name,"cmo1RIYva9o#libkernel")) return (uintptr_t)mutex_init;
    if (runtime_nid_equals(name,"9UK1vLZQft4#libkernel")) return (uintptr_t)mutex_lock;
    if (runtime_nid_equals(name,"upoVrzMHFeE#libkernel")) return (uintptr_t)mutex_trylock;
    if (runtime_nid_equals(name,"tn3VlD0hG60#libkernel")) return (uintptr_t)mutex_unlock;
    if (runtime_nid_equals(name,"2Of0f+3mhhE#libkernel")) return (uintptr_t)mutex_destroy;
    return 0;
}
void runtime_mutex_report(void) {
    printf("Runtime: mutexes created=%zu, locks=%zu, unlocks=%zu\n", created, locks, unlocks);
    printf("Runtime: condition variables created=%zu, waits=%zu, wakeups=%zu\n", conds, waits, wakeups);
}
