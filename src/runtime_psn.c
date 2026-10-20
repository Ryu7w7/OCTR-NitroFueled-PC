/* SPDX-License-Identifier: GPL-2.0-or-later */
/* OCTR's only symbol-binding glue. The PS4 contracts live in the reusable module. */
#include "runtime_psn.h"
#include "../platform/psn/psn_host.h"
#include "runtime.h"

int runtime_psn_selected(void) { return psn_host_selected(); }
int runtime_psn_start(void) {
    if (!psn_host_selected())
        return 0;
    /* OCTR UserService owns these controller-slot IDs. The reusable PSN module
     * does not assume that another host uses the same local user namespace. */
    const int32_t users[] = {1, 2, 3, 4};
    int32_t result = psn_host_set_local_users(users, 4);
    return result ? result : psn_host_start();
}
int32_t runtime_psn_system_count(void) { return psn_host_system_count(); }
int32_t runtime_psn_system_receive(void *event) { return psn_host_system_receive(event); }
uintptr_t runtime_psn_resolve(const char *name) {
    if (!psn_host_selected())
        return 0;
    /* Keep libc imports under the existing loader's ownership. */
    const char *library = strchr(name, '#');
    if (library && (!strncmp(library, "#libc", 5) || !strncmp(library, "#libSceLibcInternal", 19)))
        return 0;
    return psn_host_resolve(runtime_symbol(name));
}
