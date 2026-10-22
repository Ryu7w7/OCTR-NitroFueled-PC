/* macOS (Apple silicon, x86-64 under Rosetta 2) counterparts of the Linux calls the runtime uses.
 * Included through runtime.h after the C library headers, like compat_win.h; the macros only
 * replace calls in this project's own sources. What Rosetta allows was measured with
 * tools/macos-smoke (docs/sacredfire/19-macos-apple-silicon.md). */
#ifndef BB_COMPAT_MAC_H
#define BB_COMPAT_MAC_H
#ifdef __APPLE__
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <sys/resource.h>
#include <sys/stat.h>

/* Linux-only flags: no equivalent needed (macOS commits anonymous memory lazily). */
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#ifndef CLOCK_REALTIME_COARSE
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME CLOCK_MONOTONIC
#endif
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif

/* Symbols defined in top-level asm carry the Mach-O underscore. */
#define BB_ASM_SYMBOL(name) "_" #name

#ifdef __cplusplus
extern "C" {
#endif

/* Host thread id (pthread_threadid_np; 64-bit, truncated where the caller keeps 32 bits). */
static inline int compat_gettid(void) {
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return (int)id;
}
#define gettid() compat_gettid()

/* macOS names only the calling thread. */
static inline int compat_setname_self(const char *name) { return pthread_setname_np(name); }

/* The processor the calling thread runs on: not exposed by macOS; 0 keeps PS4 core math valid. */
static inline int sched_getcpu(void) { return 0; }

/* pthread_getattr_np replacement: base (lowest address) and size of a thread's stack. */
static inline void compat_thread_stack(pthread_t thread, void **base, size_t *size) {
    char *top = (char *)pthread_get_stackaddr_np(thread);   /* highest address on macOS */
    *size = pthread_get_stacksize_np(thread);
    *base = top - *size;
}

/* getrandom(2): getentropy fills at most 256 bytes per call. */
ssize_t compat_getrandom(void *buffer, size_t size, unsigned flags);
#define getrandom(buffer, size, flags) compat_getrandom(buffer, size, flags)

/* memfd_create: an unlinked POSIX shared memory object (sized later with ftruncate). */
int compat_memfd_create(const char *name, unsigned flags);
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 1u
#endif
#define memfd_create(name, flags) compat_memfd_create(name, flags)

/* mmap at exactly `address`, failing (MAP_FAILED, EEXIST) when anything is mapped there, like
 * Linux MAP_FIXED_NOREPLACE. MAP_FIXED alone would replace Rosetta's own reservations. */
void *compat_mmap_noreplace(void *address, size_t size, int prot, int flags, int fd, off_t offset);

/* Reads the own process's memory without faulting (process_vm_readv on Linux). */
ssize_t compat_read_own_memory(void *out, const void *source, size_t size);

/* Closes every descriptor from `first` up (close_range on Linux). */
void compat_close_from(int first);

/* Linux's struct stat names (st_mtim, ...) for Darwin's st_*timespec members. */
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec

/* getrusage(RUSAGE_THREAD): the calling thread's times from Mach (compat_mac.c). */
#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 1
#endif
int compat_getrusage(int who, struct rusage *usage);
#define getrusage(who, usage) compat_getrusage(who, usage)

/* POSIX calls macOS lacks. Deadlines are absolute CLOCK_REALTIME, as POSIX defines them. */
int compat_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline);
int compat_rwlock_timedrdlock(pthread_rwlock_t *lock, const struct timespec *deadline);
int compat_rwlock_timedwrlock(pthread_rwlock_t *lock, const struct timespec *deadline);
#define pthread_mutex_timedlock(m, t) compat_mutex_timedlock(m, t)
#define pthread_rwlock_timedrdlock(l, t) compat_rwlock_timedrdlock(l, t)
#define pthread_rwlock_timedwrlock(l, t) compat_rwlock_timedwrlock(l, t)
/* Condition variables time out on CLOCK_REALTIME only; callers pick that clock on macOS. */
static inline int compat_condattr_setclock(pthread_condattr_t *attr, clockid_t clock) {
    (void)attr;
    return clock == CLOCK_REALTIME ? 0 : EINVAL;
}
#define pthread_condattr_setclock(a, c) compat_condattr_setclock(a, c)
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif
int compat_clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remain);
#define clock_nanosleep(c, f, r, m) compat_clock_nanosleep(c, f, r, m)

/* x86-64 registers in a signal context. Rosetta reports the translated x86 state. */
typedef enum {
    BB_RAX, BB_RCX, BB_RDX, BB_RBX, BB_RSP, BB_RBP, BB_RSI, BB_RDI,
    BB_R8, BB_R9, BB_R10, BB_R11, BB_R12, BB_R13, BB_R14, BB_R15, BB_RIP, BB_EFL
} BbReg;
static inline uint64_t *compat_uc_reg(void *context, BbReg reg) {
    _STRUCT_X86_THREAD_STATE64 *s = &((ucontext_t *)context)->uc_mcontext->__ss;
    switch (reg) {
    case BB_RAX: return (uint64_t *)&s->__rax; case BB_RCX: return (uint64_t *)&s->__rcx;
    case BB_RDX: return (uint64_t *)&s->__rdx; case BB_RBX: return (uint64_t *)&s->__rbx;
    case BB_RSP: return (uint64_t *)&s->__rsp; case BB_RBP: return (uint64_t *)&s->__rbp;
    case BB_RSI: return (uint64_t *)&s->__rsi; case BB_RDI: return (uint64_t *)&s->__rdi;
    case BB_R8: return (uint64_t *)&s->__r8;   case BB_R9: return (uint64_t *)&s->__r9;
    case BB_R10: return (uint64_t *)&s->__r10; case BB_R11: return (uint64_t *)&s->__r11;
    case BB_R12: return (uint64_t *)&s->__r12; case BB_R13: return (uint64_t *)&s->__r13;
    case BB_R14: return (uint64_t *)&s->__r14; case BB_R15: return (uint64_t *)&s->__r15;
    case BB_RIP: return (uint64_t *)&s->__rip; default: return (uint64_t *)&s->__rflags;
    }
}

#ifdef __cplusplus
}
#endif
#else
#define BB_ASM_SYMBOL(name) #name
#endif /* __APPLE__ */
#endif
