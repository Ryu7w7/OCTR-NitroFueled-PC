/* macOS counterparts of Linux calls (compat_mac.h). Built on macOS only (build.sh). */
#ifdef __APPLE__
#include "compat_mac.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

ssize_t compat_getrandom(void *buffer, size_t size, unsigned flags) {
    (void)flags;
    size_t done = 0;
    while (done < size) {
        size_t n = size - done < 256 ? size - done : 256;
        if (getentropy((char *)buffer + done, n)) return -1;
        done += n;
    }
    return (ssize_t)size;
}

int compat_memfd_create(const char *name, unsigned flags) {
    static unsigned serial;
    char path[64];
    for (int attempt = 0; attempt < 64; ++attempt) {
        snprintf(path, sizeof path, "/bb-%d-%u-%.16s", (int)getpid(), __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED), name);
        int fd = shm_open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0 && errno == EEXIST) continue;
        if (fd < 0) return -1;
        shm_unlink(path);
        if (flags & MFD_CLOEXEC) fcntl(fd, F_SETFD, FD_CLOEXEC);
        return fd;
    }
    errno = EEXIST;
    return -1;
}

void *compat_mmap_noreplace(void *address, size_t size, int prot, int flags, int fd, off_t offset) {
    mach_vm_address_t at = (mach_vm_address_t)(uintptr_t)address;
    /* VM_FLAGS_FIXED without VM_FLAGS_OVERWRITE fails on any existing mapping. */
    if (mach_vm_allocate(mach_task_self(), &at, size, VM_FLAGS_FIXED) != KERN_SUCCESS) {
        errno = EEXIST;
        return MAP_FAILED;
    }
    if (fd < 0 && (flags & MAP_ANON)) {
        if (prot != (PROT_READ | PROT_WRITE) && mprotect(address, size, prot)) {
            int saved = errno;
            mach_vm_deallocate(mach_task_self(), at, size);
            errno = saved;
            return MAP_FAILED;
        }
        return address;
    }
    /* A file or shared object: replace our own claim, nothing else. */
    void *p = mmap(address, size, prot, (flags & ~MAP_ANON) | MAP_FIXED, fd, offset);
    if (p == MAP_FAILED) {
        int saved = errno;
        mach_vm_deallocate(mach_task_self(), at, size);
        errno = saved;
    }
    return p;
}

ssize_t compat_read_own_memory(void *out, const void *source, size_t size) {
    mach_vm_size_t got = 0;
    kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)source, size,
                                              (mach_vm_address_t)(uintptr_t)out, &got);
    if (kr != KERN_SUCCESS) { errno = EFAULT; return -1; }
    return (ssize_t)got;
}

/* Timed locks by polling: macOS has trylock only. 50 us steps keep the wake-up latency well
 * under a frame while the wait itself costs almost nothing. */
static int deadline_passed(const struct timespec *deadline) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec > deadline->tv_sec || (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}
static void short_pause(void) { struct timespec t = {0, 50000}; nanosleep(&t, NULL); }
int compat_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline) {
    if (deadline->tv_nsec < 0 || deadline->tv_nsec >= 1000000000) return EINVAL;
    for (;;) {
        int e = pthread_mutex_trylock(mutex);
        if (e != EBUSY) return e;
        if (deadline_passed(deadline)) return ETIMEDOUT;
        short_pause();
    }
}
int compat_rwlock_timedrdlock(pthread_rwlock_t *lock, const struct timespec *deadline) {
    if (deadline->tv_nsec < 0 || deadline->tv_nsec >= 1000000000) return EINVAL;
    for (;;) {
        int e = pthread_rwlock_tryrdlock(lock);
        if (e != EBUSY) return e;
        if (deadline_passed(deadline)) return ETIMEDOUT;
        short_pause();
    }
}
int compat_rwlock_timedwrlock(pthread_rwlock_t *lock, const struct timespec *deadline) {
    if (deadline->tv_nsec < 0 || deadline->tv_nsec >= 1000000000) return EINVAL;
    for (;;) {
        int e = pthread_rwlock_trywrlock(lock);
        if (e != EBUSY) return e;
        if (deadline_passed(deadline)) return ETIMEDOUT;
        short_pause();
    }
}

int compat_clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remain) {
    struct timespec wait = *request;
    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        if (clock_gettime(clock, &now)) return errno;
        int64_t ns = (int64_t)(request->tv_sec - now.tv_sec) * 1000000000 + (request->tv_nsec - now.tv_nsec);
        if (ns <= 0) return 0;
        wait.tv_sec = (time_t)(ns / 1000000000);
        wait.tv_nsec = (long)(ns % 1000000000);
    }
    return nanosleep(&wait, remain) ? errno : 0;
}

#undef getrusage
int compat_getrusage(int who, struct rusage *usage) {
    if (who != RUSAGE_THREAD) return getrusage(who, usage);
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(pthread_mach_thread_np(pthread_self()), THREAD_BASIC_INFO, (thread_info_t)&info, &count) != KERN_SUCCESS) {
        errno = EINVAL;
        return -1;
    }
    memset(usage, 0, sizeof *usage);
    usage->ru_utime.tv_sec = info.user_time.seconds;
    usage->ru_utime.tv_usec = info.user_time.microseconds;
    usage->ru_stime.tv_sec = info.system_time.seconds;
    usage->ru_stime.tv_usec = info.system_time.microseconds;
    return 0;
}

void compat_close_from(int first) {
    int last = getdtablesize();
    for (int fd = first; fd < last; ++fd) close(fd);
}
#endif
