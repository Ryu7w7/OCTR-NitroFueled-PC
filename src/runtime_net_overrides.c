#include "runtime_net_overrides.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifndef _WIN32
#include <strings.h>
#define _stricmp strcasecmp
#endif

#define MAX_OVERRIDES 64
#define MAX_HOST_LEN 128
#define MAX_TARGET_LEN 256

typedef struct {
    char source[MAX_HOST_LEN];
    char target_ip[MAX_HOST_LEN];
    uint16_t port;
    int is_http;
} HostOverride;

static HostOverride overrides[MAX_OVERRIDES];
static int override_count = 0;
static int overrides_loaded = 0;

static void trim(char *s) {
    char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

static void parse_entry(const char *key, const char *val) {
    if (override_count >= MAX_OVERRIDES) return;
    HostOverride *o = &overrides[override_count];
    strncpy(o->source, key, sizeof(o->source) - 1);
    o->source[sizeof(o->source) - 1] = '\0';
    trim(o->source);

    char target[MAX_TARGET_LEN];
    strncpy(target, val, sizeof(target) - 1);
    target[sizeof(target) - 1] = '\0';
    trim(target);

    o->port = 0;
    o->is_http = 0;

    const char *p = target;
    if (strncmp(p, "http://", 7) == 0) {
        o->is_http = 1;
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
    }

    char host_part[MAX_HOST_LEN];
    strncpy(host_part, p, sizeof(host_part) - 1);
    host_part[sizeof(host_part) - 1] = '\0';

    char *colon = strchr(host_part, ':');
    if (colon) {
        *colon = '\0';
        o->port = (uint16_t)atoi(colon + 1);
    }
    char *slash = strchr(host_part, '/');
    if (slash) *slash = '\0';

    strncpy(o->target_ip, host_part, sizeof(o->target_ip) - 1);
    o->target_ip[sizeof(o->target_ip) - 1] = '\0';
    trim(o->target_ip);

    override_count++;
}

static void parse_json_file(FILE *f) {
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) return;

    char *buf = (char *)malloc(sz + 1);
    if (!buf) return;
    size_t read_bytes = fread(buf, 1, sz, f);
    buf[read_bytes] = '\0';

    char *p = buf;
    while (*p) {
        char *q1 = strchr(p, '"');
        if (!q1) break;
        char *q2 = strchr(q1 + 1, '"');
        if (!q2) break;
        *q2 = '\0';
        char *key = q1 + 1;

        char *colon = strchr(q2 + 1, ':');
        if (!colon) break;

        char *v1 = strchr(colon + 1, '"');
        if (!v1) break;
        char *v2 = strchr(v1 + 1, '"');
        if (!v2) break;
        *v2 = '\0';
        char *val = v1 + 1;

        parse_entry(key, val);
        p = v2 + 1;
    }

    free(buf);
}

void net_overrides_init(void) {
    if (overrides_loaded) return;
    overrides_loaded = 1;
    override_count = 0;

    const char *paths[] = {
        "host_overrides.json",
        "user/host_overrides.json",
        NULL
    };

    const char *data_dir = getenv("BB_DATA_DIR");
    char custom_path[512];
    if (data_dir) {
        snprintf(custom_path, sizeof(custom_path), "%s/host_overrides.json", data_dir);
    }

    FILE *f = NULL;
    if (data_dir) {
        f = fopen(custom_path, "r");
    }
    for (int i = 0; !f && paths[i]; i++) {
        f = fopen(paths[i], "r");
    }

    if (f) {
        parse_json_file(f);
        fclose(f);
        printf("NetOverrides: loaded %d host overrides\n", override_count);
    } else {
        printf("NetOverrides: no host_overrides.json found\n");
    }
}

const char *net_overrides_lookup(const char *host, uint16_t *port_override, int *is_http) {
    if (!host) return NULL;
    net_overrides_init();

    for (int i = 0; i < override_count; i++) {
        if (_stricmp(host, overrides[i].source) == 0) {
            if (port_override && overrides[i].port) {
                *port_override = overrides[i].port;
            }
            if (is_http) {
                *is_http = overrides[i].is_http;
            }
            return overrides[i].target_ip;
        }
    }
    return NULL;
}

static int str_ends_with(const char *str, const char *suffix) {
    if (!str || !suffix) return 0;
    size_t str_len = strlen(str);
    size_t suf_len = strlen(suffix);
    if (str_len < suf_len) return 0;
    return (_stricmp(str + str_len - suf_len, suffix) == 0);
}

int net_overrides_is_allowed(const char *host) {
    if (!host) return 0;
    net_overrides_init();

    if (str_ends_with(host, "demonware.net") ||
        str_ends_with(host, "playstation.net") ||
        str_ends_with(host, "sonyentertainmentnetwork.com")) {
        /* Must have an explicit override */
        return net_overrides_lookup(host, NULL, NULL) != NULL;
    }

    /* Localhost is always allowed */
    if (strcmp(host, "127.0.0.1") == 0 || _stricmp(host, "localhost") == 0) {
        return 1;
    }

    return 1;
}
