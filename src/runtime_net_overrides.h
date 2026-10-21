#ifndef RUNTIME_NET_OVERRIDES_H
#define RUNTIME_NET_OVERRIDES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize host overrides by reading host_overrides.json (or user/host_overrides.json). */
void net_overrides_init(void);

/* Check if a hostname has an override configured.
 * Returns the resolved IP or target hostname string (or NULL if not overridden).
 * If is_http is not NULL and the override specified an "http://" scheme, *is_http is set to 1.
 * If port_override is not NULL and a custom port was specified, *port_override is set (e.g. 8080).
 */
const char *net_overrides_lookup(const char *host, uint16_t *port_override, int *is_http);

/* Whitelist check: returns 1 if allowed, 0 if blocked.
 * Any *.demonware.net, *.playstation.net, or *.sonyentertainmentnetwork.com
 * domain MUST have an explicit override or it is blocked to prevent accidental
 * connections to production servers.
 */
int net_overrides_is_allowed(const char *host);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_NET_OVERRIDES_H */
