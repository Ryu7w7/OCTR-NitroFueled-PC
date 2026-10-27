// SPDX-License-Identifier: GPL-2.0-or-later
// SacredFire (macOS): small POSIX helpers the renderer uses for diagnostics and thread identity
// that Darwin lacks. Force-included into the GPU library on macOS only (CMakeLists.txt), like
// bb_win_compat.h on Windows.
#pragma once
#ifdef __APPLE__
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#ifdef __cplusplus
extern "C" {
#endif
static inline int gettid(void) {
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return (int)id;
}

// process_vm_readv on the own process: a read that cannot fault (mach_vm_read_overwrite).
static inline ssize_t process_vm_readv(pid_t pid, const struct iovec* local, unsigned long local_count,
                                       const struct iovec* remote, unsigned long remote_count,
                                       unsigned long flags) {
    (void)flags;
    if (pid != getpid() || local_count != 1 || remote_count != 1) {
        errno = ENOSYS;
        return -1;
    }
    const size_t size = local->iov_len < remote->iov_len ? local->iov_len : remote->iov_len;
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)remote->iov_base, size,
                               (mach_vm_address_t)(uintptr_t)local->iov_base, &got) != KERN_SUCCESS) {
        errno = EFAULT;
        return -1;
    }
    return (ssize_t)got;
}

// getrusage(RUSAGE_THREAD): the calling thread's user and system time from Mach.
#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 1
#endif
static inline int compat_getrusage(int who, struct rusage* usage) {
    if (who != RUSAGE_THREAD) {
        return getrusage(who, usage);
    }
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(pthread_mach_thread_np(pthread_self()), THREAD_BASIC_INFO, (thread_info_t)&info, &count) !=
        KERN_SUCCESS) {
        errno = EINVAL;
        return -1;
    }
    memset(usage, 0, sizeof(*usage));
    usage->ru_utime.tv_sec = info.user_time.seconds;
    usage->ru_utime.tv_usec = info.user_time.microseconds;
    usage->ru_stime.tv_sec = info.system_time.seconds;
    usage->ru_stime.tv_usec = info.system_time.microseconds;
    return 0;
}
#define getrusage(who, usage) compat_getrusage(who, usage)
#ifdef __cplusplus
}
#endif
#endif
