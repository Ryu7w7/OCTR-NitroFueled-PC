/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PSN_HOST_H
#define PSN_HOST_H
#include "include/psn_backend.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* PS4 guests use x86-64 System V even when the provider uses the Windows ABI.
 * This module deliberately has no dependency on any bbport runtime header. */
#define PSN_GUEST_ABI __attribute__((sysv_abi))
typedef struct {
    const char *name;
    void *function;
} PsnHostExport;
static inline uintptr_t psn_host_lookup(const PsnHostExport *table, size_t count,
                                        const char *symbol) {
    if (!symbol)
        return 0;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(symbol, table[i].name))
            return (uintptr_t)table[i].function;
    return 0;
}
#define PSN_LOOKUP(table, symbol) psn_host_lookup(table, sizeof(table) / sizeof(*(table)), symbol)

/* Internal host adapter. Backend lock serializes native calls, never guest callbacks.
 * Module registry locks may nest the backend lock; the core releases its lock before
 * delivering events to either module. Resolver functions are only used in external mode. */
int psn_host_selected(void);
int psn_host_start(void);
void psn_host_stop(void);
const PsnBackendV1 *psn_host_lock(void **instance);
void psn_host_unlock(void);
int32_t psn_host_pump(void);
uintptr_t psn_host_resolve(const char *name);

uintptr_t psn_host_identity_resolve(const char *name);
int32_t psn_host_identity_event(const PsnEvent *event);
void psn_host_identity_dispatch(void);
void psn_host_identity_reset(void);
uintptr_t psn_host_webapi_resolve(const char *name);
int32_t psn_host_webapi_event(const PsnPushEvent *event);
void psn_host_webapi_dispatch(void);
void psn_host_webapi_reset(void);

/* Host-owned SystemService queue. Calling count/receive pumps copied backend events. */
int32_t psn_host_system_count(void);
int32_t psn_host_system_receive(void *event);
#endif
