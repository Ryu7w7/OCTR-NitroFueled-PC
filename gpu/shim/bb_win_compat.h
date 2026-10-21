// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (Windows): small POSIX helpers the renderer uses for diagnostics and thread
// identity. Force-included into the GPU library on Windows only (CMakeLists.txt); kept free
// of <windows.h> so its macros do not reach every translation unit.
#pragma once
#ifdef _WIN32
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
static inline int gettid(void) { return (int)GetCurrentThreadId(); }
/* SacredFire diagnostics read guest memory that may be unmapped with process_vm_readv on
 * Linux; on Windows ReadProcessMemory on the own process does the same (src/compat_win.c). */
ssize_t compat_read_own_memory(void* out, const void* source, size_t size);
struct iovec {
    void* iov_base;
    size_t iov_len;
};
static inline ssize_t process_vm_readv(int pid, const struct iovec* local, unsigned long local_count,
                                       const struct iovec* remote, unsigned long remote_count,
                                       unsigned long flags) {
    (void)pid;
    (void)flags;
    if (local_count != 1 || remote_count != 1) {
        return -1;
    }
    const size_t size = local->iov_len < remote->iov_len ? local->iov_len : remote->iov_len;
    return compat_read_own_memory(local->iov_base, remote->iov_base, size);
}
#ifdef __cplusplus
}
#endif
#endif
