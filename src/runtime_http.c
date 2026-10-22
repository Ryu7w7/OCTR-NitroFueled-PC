/* runtime_net.h first: on Linux it turns on _GNU_SOURCE (getaddrinfo, used by
 * runtime_ryunet_request), which must come before any system header. */
#include "runtime_net.h"
#include "runtime_http.h"
#include "runtime_net_overrides.h"
#include "bb_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#define ORBIS_OK 0
#define HTTP_NETWORK ((int32_t)0x80431063)
#define HTTP_INVALID_VALUE ((int32_t)0x80431077)
#define HTTP_INVALID_ID ((int32_t)0x80431100)
/* sceHttpParseResponseHeader errors (ref: shadPS4 http_error.h). */
#define HTTP_PARSE_NOT_FOUND ((int32_t)0x80432025)
#define HTTP_PARSE_INVALID_RESPONSE ((int32_t)0x80432060)
#define HTTP_PARSE_INVALID_VALUE ((int32_t)0x804321fe)

/* PS4 SceHttpMethod (ref: shadPS4 http.cpp map_method). */
#define HTTP_METHOD_GET 0
#define HTTP_METHOD_POST 1
#define HTTP_METHOD_HEAD 2
#define HTTP_METHOD_OPTIONS 3
#define HTTP_METHOD_PUT 4
#define HTTP_METHOD_DELETE 5
#define HTTP_METHOD_TRACE 6
#define HTTP_METHOD_CONNECT 7
#define HTTP_METHOD_CUSTOM 8

#define MAX_HTTP_TEMPLATES 16
#define MAX_HTTP_CONNECTIONS 32
#define MAX_HTTP_REQUESTS 64
#define MAX_HEADERS 32
#define MAX_HEADER_LEN 1024

typedef struct {
    int in_use;
    char name[64];
} HttpTemplate;

typedef struct {
    int in_use;
    int template_id;
    char raw_url[256];
    char host[128];
    char target_ip[64];
    uint16_t port;
    int is_plain_http;
    int redirected; /* to the RyuNET server (runtime_net_overrides) */
} HttpConnection;

typedef struct {
    char name[64];
    char value[512];
} HttpHeader;

typedef struct {
    int in_use;
    int conn_id;
    int method;
    char method_str[32];
    char path[128];
    uint64_t req_content_len;

    HttpHeader req_headers[MAX_HEADERS];
    int req_header_count;

    int status_code;
    char *resp_body;
    size_t resp_body_len;
    size_t resp_read_offset;

    HttpHeader resp_headers[MAX_HEADERS];
    int resp_header_count;
    char *raw_resp_headers;
    size_t raw_resp_headers_len;
} HttpRequest;

static HttpTemplate g_templates[MAX_HTTP_TEMPLATES];
static HttpConnection g_conns[MAX_HTTP_CONNECTIONS];
static HttpRequest g_requests[MAX_HTTP_REQUESTS];

int32_t runtime_http_init(void) {
    runtime_net_init();
    static int s_http_ctx_id = 0;
    int id = ++s_http_ctx_id;
    runtime_net_trace("sceHttpInit -> %d", id);
    return id;
}

int32_t runtime_http_term(void) {
    runtime_net_trace("sceHttpTerm");
    return ORBIS_OK;
}

int32_t runtime_http_create_template(int32_t http_ctx, const char *name, int32_t version, int32_t auto_redirect) {
    (void)http_ctx; (void)version; (void)auto_redirect;
    for (int i = 0; i < MAX_HTTP_TEMPLATES; i++) {
        if (!g_templates[i].in_use) {
            g_templates[i].in_use = 1;
            if (name) strncpy(g_templates[i].name, name, sizeof(g_templates[i].name) - 1);
            runtime_net_trace("sceHttpCreateTemplate(ctx=%d, '%s') -> %d", http_ctx, name ? name : "", i + 1);
            return i + 1;
        }
    }
    return HTTP_INVALID_VALUE;
}

int32_t runtime_http_delete_template(int32_t tmpl) {
    int idx = tmpl - 1;
    if (idx >= 0 && idx < MAX_HTTP_TEMPLATES) {
        g_templates[idx].in_use = 0;
    }
    return ORBIS_OK;
}

static void parse_url(const char *url, char *host, size_t host_sz, uint16_t *port, int *is_plain_http) {
    *port = 80;
    *is_plain_http = 1;
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        *is_plain_http = 0;
        *port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        *is_plain_http = 1;
        *port = 80;
        p += 7;
    }

    char host_part[128];
    strncpy(host_part, p, sizeof(host_part) - 1);
    host_part[sizeof(host_part) - 1] = '\0';

    char *slash = strchr(host_part, '/');
    if (slash) *slash = '\0';

    char *colon = strchr(host_part, ':');
    if (colon) {
        *colon = '\0';
        *port = (uint16_t)atoi(colon + 1);
    }

    strncpy(host, host_part, host_sz - 1);
    host[host_sz - 1] = '\0';
}

int32_t runtime_http_create_connection(int32_t tmpl, const char *url, int32_t keep_alive) {
    (void)keep_alive;
    if (!url) return HTTP_INVALID_VALUE;

    for (int i = 0; i < MAX_HTTP_CONNECTIONS; i++) {
        if (!g_conns[i].in_use) {
            HttpConnection *c = &g_conns[i];
            c->in_use = 1;
            c->template_id = tmpl;
            strncpy(c->raw_url, url, sizeof(c->raw_url) - 1);

            parse_url(url, c->host, sizeof(c->host), &c->port, &c->is_plain_http);

            uint16_t port_override = 0;
            int is_http = 0;
            const char *target = net_overrides_lookup(c->host, &port_override, &is_http);

            if (target) {
                strncpy(c->target_ip, target, sizeof(c->target_ip) - 1);
                if (port_override) c->port = port_override;
                if (is_http) c->is_plain_http = 1;
                c->redirected = 1;
                runtime_net_trace("sceHttpCreateConnectionWithURL('%s') -> override %s:%u (HTTP)",
                                  url, c->target_ip, c->port);
            } else {
                c->redirected = 0;
                strncpy(c->target_ip, c->host, sizeof(c->target_ip) - 1);
                runtime_net_trace("sceHttpCreateConnectionWithURL('%s') -> %s:%u",
                                  url, c->target_ip, c->port);
            }

            return i + 1;
        }
    }
    return HTTP_INVALID_VALUE;
}

int32_t runtime_http_delete_connection(int32_t conn) {
    int idx = conn - 1;
    if (idx >= 0 && idx < MAX_HTTP_CONNECTIONS) {
        g_conns[idx].in_use = 0;
    }
    return ORBIS_OK;
}

static const char *method_enum_name(int method) {
    switch (method) {
    case HTTP_METHOD_GET: return "GET";
    case HTTP_METHOD_POST: return "POST";
    case HTTP_METHOD_HEAD: return "HEAD";
    case HTTP_METHOD_OPTIONS: return "OPTIONS";
    case HTTP_METHOD_PUT: return "PUT";
    case HTTP_METHOD_DELETE: return "DELETE";
    case HTTP_METHOD_TRACE: return "TRACE";
    case HTTP_METHOD_CONNECT: return "CONNECT";
    default: return "CUSTOM";
    }
}

static int method_from_name(const char *name) {
    if (!name) return HTTP_METHOD_CUSTOM;
    if (!_stricmp(name, "GET")) return HTTP_METHOD_GET;
    if (!_stricmp(name, "POST")) return HTTP_METHOD_POST;
    if (!_stricmp(name, "HEAD")) return HTTP_METHOD_HEAD;
    if (!_stricmp(name, "OPTIONS")) return HTTP_METHOD_OPTIONS;
    if (!_stricmp(name, "PUT")) return HTTP_METHOD_PUT;
    if (!_stricmp(name, "DELETE")) return HTTP_METHOD_DELETE;
    if (!_stricmp(name, "TRACE")) return HTTP_METHOD_TRACE;
    if (!_stricmp(name, "CONNECT")) return HTTP_METHOD_CONNECT;
    return HTTP_METHOD_CUSTOM;
}

int32_t runtime_http_create_request(int32_t conn, int32_t method, const char *path, uint64_t content_length) {
    int conn_idx = conn - 1;
    if (conn_idx < 0 || conn_idx >= MAX_HTTP_CONNECTIONS || !g_conns[conn_idx].in_use) {
        return HTTP_INVALID_VALUE;
    }

    for (int i = 0; i < MAX_HTTP_REQUESTS; i++) {
        if (!g_requests[i].in_use) {
            HttpRequest *r = &g_requests[i];
            memset(r, 0, sizeof(*r));
            r->in_use = 1;
            r->conn_id = conn;
            r->method = method;
            strncpy(r->method_str, method_enum_name(method), sizeof(r->method_str) - 1);
            if (path) strncpy(r->path, path, sizeof(r->path) - 1);
            else strncpy(r->path, "/", sizeof(r->path) - 1);
            r->req_content_len = content_length;

            runtime_net_trace("sceHttpCreateRequestWithURL(conn=%d, method=%d/%s, path='%s') -> req=%d",
                              conn, method, r->method_str, r->path, i + 1);
            return i + 1;
        }
    }
    return HTTP_INVALID_VALUE;
}

int32_t runtime_http_create_request2(int32_t conn, const char *method, const char *path, uint64_t content_length) {
    /* PS4 variant with a method name string (ref: shadPS4
     * sceHttpCreateRequestWithURL2). The game (CTR) calls this one. */
    int conn_idx = conn - 1;
    if (conn_idx < 0 || conn_idx >= MAX_HTTP_CONNECTIONS || !g_conns[conn_idx].in_use) {
        return HTTP_INVALID_VALUE;
    }
    if (!method) return HTTP_INVALID_VALUE;

    for (int i = 0; i < MAX_HTTP_REQUESTS; i++) {
        if (!g_requests[i].in_use) {
            HttpRequest *r = &g_requests[i];
            memset(r, 0, sizeof(*r));
            r->in_use = 1;
            r->conn_id = conn;
            r->method = method_from_name(method);
            strncpy(r->method_str, method, sizeof(r->method_str) - 1);
            if (path) strncpy(r->path, path, sizeof(r->path) - 1);
            else strncpy(r->path, "/", sizeof(r->path) - 1);
            r->req_content_len = content_length;

            runtime_net_trace("sceHttpCreateRequestWithURL2(conn=%d, method='%s', path='%s') -> req=%d",
                              conn, r->method_str, r->path, i + 1);
            return i + 1;
        }
    }
    return HTTP_INVALID_VALUE;
}

int32_t runtime_http_delete_request(int32_t req) {
    int idx = req - 1;
    if (idx >= 0 && idx < MAX_HTTP_REQUESTS && g_requests[idx].in_use) {
        HttpRequest *r = &g_requests[idx];
        if (r->resp_body) free(r->resp_body);
        if (r->raw_resp_headers) free(r->raw_resp_headers);
        r->in_use = 0;
        runtime_net_trace("sceHttpDeleteRequest(req=%d)", req);
    }
    return ORBIS_OK;
}

int32_t runtime_http_set_request_content_length(int32_t req, uint64_t length) {
    int idx = req - 1;
    if (idx >= 0 && idx < MAX_HTTP_REQUESTS && g_requests[idx].in_use) {
        g_requests[idx].req_content_len = length;
    }
    return ORBIS_OK;
}

int32_t runtime_http_add_request_header(int32_t req, const char *name, const char *value, int32_t mode) {
    (void)mode;
    int idx = req - 1;
    if (idx < 0 || idx >= MAX_HTTP_REQUESTS || !g_requests[idx].in_use || !name || !value) {
        return HTTP_INVALID_VALUE;
    }
    HttpRequest *r = &g_requests[idx];
    if (r->req_header_count < MAX_HEADERS) {
        strncpy(r->req_headers[r->req_header_count].name, name, sizeof(r->req_headers[0].name) - 1);
        strncpy(r->req_headers[r->req_header_count].value, value, sizeof(r->req_headers[0].value) - 1);
        r->req_header_count++;
        runtime_net_trace("sceHttpAddRequestHeader(req=%d, '%s: %s')", req, name, value);
    }
    return ORBIS_OK;
}

/* Seconds to add to this PC's clock to get the RyuNET server's. Signatures carry the time and
 * the server takes them only within 5 minutes of its own clock, so a PC whose clock is off could
 * not log in at all ("nickname signature out of time"). run.py measures it before the game
 * starts (BB_ONLINE_CLOCK_OFFSET) and every answer of the server corrects it (its Date header):
 * a login refused for the time goes through on the game's next try. */
static long long g_clock_offset;
static int g_clock_offset_read;

static long long ryunet_clock_offset(void) {
    if (!__atomic_load_n(&g_clock_offset_read, __ATOMIC_ACQUIRE)) {
        const char *env = getenv("BB_ONLINE_CLOCK_OFFSET");
        long long offset = 0;
        if (env && sscanf(env, "%lld", &offset) == 1 && offset > -400000000ll && offset < 400000000ll) {
            long long unset = 0;
            __atomic_compare_exchange_n(&g_clock_offset, &unset, offset, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        }
        __atomic_store_n(&g_clock_offset_read, 1, __ATOMIC_RELEASE);
    }
    return __atomic_load_n(&g_clock_offset, __ATOMIC_RELAXED);
}

/* Unix time of an HTTP date ("Wed, 22 Oct 2026 00:10:00 GMT"), or -1. */
static long long http_date_seconds(const char *text) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char month[4] = "";
    int day, year, hour, minute, second;
    const char *comma = strchr(text, ',');
    if (!comma || sscanf(comma + 1, " %d %3s %d %d:%d:%d", &day, month, &year, &hour, &minute, &second) != 6)
        return -1;
    const char *found = strlen(month) == 3 ? strstr(months, month) : NULL;
    if (!found || (found - months) % 3 || year < 2000 || year > 2200) return -1;
    int m = (int)(found - months) / 3 + 1;
    /* days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's days_from_civil) */
    long long y = year - (m <= 2);
    const long long era = y / 400, yoe = y - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const long long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

/* An answer of the RyuNET server: its Date header sets the clock offset. */
static void ryunet_note_server_clock(const char *headers) {
    for (const char *line = headers; line && *line && *line != '\r' && *line != '\n';) {
        if (!_strnicmp(line, "Date:", 5)) {
            const long long server = http_date_seconds(line + 5);
            if (server < 0) return;
            ryunet_clock_offset(); /* the launcher's value is read first, then replaced */
            const long long offset = server - (long long)time(NULL);
            /* The header has whole seconds: a few seconds either way change nothing. */
            const long long old = __atomic_exchange_n(&g_clock_offset, offset, __ATOMIC_RELAXED);
            if (llabs(offset - old) > 30) {
                runtime_net_trace("RyuNET server clock: this PC's clock is %lld s %s the server's%s", llabs(offset),
                                  offset > 0 ? "behind" : "ahead of",
                                  llabs(offset) > 240 ? " (signed requests now use the server's time)" : "");
            }
            return;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
}

/* X-RyuNET-Proof for one request to the RyuNET server (its nick_proof.go): an HMAC-SHA256,
 * keyed with the hash of this installation's key for the server (BB_ONLINE_KEYHASH, from
 * run.py), of the time (the server's: ryunet_clock_offset), a nonce, the method and the
 * request target. 0 without a key. */
int runtime_ryunet_proof(const char *method, const char *target, char *out, size_t out_size) {
    const char *key_hash = getenv("BB_ONLINE_KEYHASH");
    if (!key_hash || strlen(key_hash) != 64 || strspn(key_hash, "0123456789abcdef") != 64) return 0;
    static unsigned long long counter;
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    const long long ts = (long long)now.tv_sec + ryunet_clock_offset();
    char nonce[33], message[1200];
    snprintf(nonce, sizeof nonce, "%016llx%016llx",
             (unsigned long long)now.tv_sec * 1000000000ull + (unsigned long long)now.tv_nsec,
             __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED) ^ (unsigned long long)(uintptr_t)&now);
    const int n = snprintf(message, sizeof message, "%lld\n%s\n%s\n%s", ts, nonce, method, target);
    if (n <= 0 || n >= (int)sizeof message) return 0;
    uint8_t mac[32];
    bb_hmac_sha256((const unsigned char *)key_hash, 64, (const unsigned char *)message, (size_t)n, mac);
    char mac_hex[65];
    for (int i = 0; i < 32; i++) snprintf(mac_hex + 2 * i, 3, "%02x", mac[i]);
    const int len = snprintf(out, out_size, "1.%lld.%s.%s", ts, nonce, mac_hex);
    return len > 0 && (size_t)len < out_size;
}

/* How requests name the RyuNET server. On some players' networks something on the way (a
 * router or ISP filter, an antivirus web shield) takes the request for one form and answers in
 * the server's place: a redirect to its own page ("302" with no body: the server never
 * redirects these requests) or nothing. The request then goes again in the next form, and the
 * form that went through is kept for the run:
 *   0: "POST /auth/", Host: the server's name, as the overrides give it;
 *   1: the same, Host: the server's address;
 *   2: as the game names it ("POST https://lavender-ps4-auth3.prod.demonware.net:443/auth/",
 *      Host: Demonware's), as v0.6 and older sent everything. */
#define RYUNET_FORMS 3
static int g_ryunet_form;
static const char *const ryunet_form_names[RYUNET_FORMS] = {
    "a plain request for the server's name", "a plain request for the server's address", "the game's own URL"};

/* Whether an answer came from something on the way and not from the RyuNET server: none, not
 * HTTP, or a redirect. A redirect's Location goes to the log: it names what intercepted it. */
static int ryunet_intercepted(const char *reply, int len, int form) {
    int status = 0;
    const char *space = len > 0 && strncmp(reply, "HTTP/", 5) == 0 ? strchr(reply, ' ') : NULL;
    if (!space || sscanf(space, "%d", &status) != 1) {
        runtime_net_trace("RyuNET request as %s: %s", ryunet_form_names[form],
                          len > 0 ? "an answer that is not HTTP" : "no answer");
        return 1;
    }
    if (status < 300 || status > 399) return 0;
    char location[200] = "";
    for (const char *line = reply; *line && *line != '\r' && *line != '\n';) {
        if (!_strnicmp(line, "Location:", 9)) {
            sscanf(line + 9, " %199[^\r\n]", location);
            break;
        }
        line = strchr(line, '\n');
        if (!line) break;
        line++;
    }
    runtime_net_trace("RyuNET request as %s: %d from something on the way, not the server (Location: %s)",
                      ryunet_form_names[form], status, location[0] ? location : "none");
    return 1;
}

/* A form that went through after another was intercepted: the next requests use it. */
static void ryunet_keep_form(int form) {
    if (__atomic_exchange_n(&g_ryunet_form, form, __ATOMIC_RELAXED) != form)
        runtime_net_trace("RyuNET requests now go as %s, which this network lets through", ryunet_form_names[form]);
}

/* A signed request of the port's own (not the game's) to the RyuNET server's HTTP port
 * (BB_ONLINE_SERVER, BB_ONLINE_HTTP_PORT): the body goes to body (NUL-terminated, cut to
 * body_size). The HTTP status, or 0 when the server could not be reached. Blocking, with
 * 8-second timeouts: for background threads. */
int runtime_ryunet_request(const char *method, const char *target, char *body, size_t body_size) {
    if (body_size) body[0] = '\0';
    const char *host = getenv("BB_ONLINE_SERVER");
    const char *port = getenv("BB_ONLINE_HTTP_PORT");
    if (!host || !*host) return 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port && *port ? port : "8080", &hints, &res) != 0 || !res) return 0;
    struct sockaddr_in sin;
    memcpy(&sin, res->ai_addr, sizeof sin);
    freeaddrinfo(res);
    char address[INET_ADDRSTRLEN] = "";
    inet_ntop(AF_INET, &sin.sin_addr, address, sizeof address);
    char reply[32768];
    const int first = __atomic_load_n(&g_ryunet_form, __ATOMIC_RELAXED);
    for (int tried = 0; tried < RYUNET_FORMS; tried++) {
        const int form = (first + tried) % RYUNET_FORMS;
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return 0;
#ifdef _WIN32
        DWORD timeout = 8000;
#else
        struct timeval timeout = {8, 0};
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof timeout);
        if (connect(s, (const struct sockaddr *)&sin, sizeof sin) != 0) {
            closesocket(s);
            return 0;
        }
        char host_header[96];
        snprintf(host_header, sizeof host_header, "%s", form == 0 ? host
                                                       : form == 1 && address[0] ? address
                                                                                 : "lavender-ps4-auth3.prod.demonware.net");
        char proof[160], request[4096]; /* a report's comment rides in the target */
        const int signed_ok = runtime_ryunet_proof(method, target, proof, sizeof proof);
        const int n = snprintf(request, sizeof request,
                               "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nContent-Length: 0\r\n%s%s%s\r\n",
                               method, target, host_header, signed_ok ? "X-RyuNET-Proof: " : "",
                               signed_ok ? proof : "", signed_ok ? "\r\n" : "");
        if (n <= 0 || n >= (int)sizeof request || send(s, request, n, 0) != n) {
            closesocket(s);
            return 0;
        }
        int got = 0;
        for (int r; got < (int)sizeof reply - 1 && (r = recv(s, reply + got, (int)sizeof reply - 1 - got, 0)) > 0;)
            got += r;
        closesocket(s);
        reply[got] = '\0';
        if (!ryunet_intercepted(reply, got, form)) {
            ryunet_note_server_clock(reply);
            ryunet_keep_form(form);
            break;
        }
    }
    int status = 0;
    if (strncmp(reply, "HTTP/", 5) != 0 || sscanf(strchr(reply, ' ') ? strchr(reply, ' ') : reply, "%d", &status) != 1)
        return 0;
    const char *start = strstr(reply, "\r\n\r\n");
    if (start && body_size) snprintf(body, body_size, "%s", start + 4);
    return status;
}

/* One exchange of a request: its answer in resp (NUL-terminated) and its length, or -1 when
 * the server could not be reached. form is how a request to the RyuNET server names it
 * (ryunet_form_names); address is the server's. */
static int http_exchange(const HttpConnection *c, const HttpRequest *r, const struct sockaddr_in *sin, int form,
                         const char *address, const void *post_data, size_t size, char *resp, int resp_size) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    if (connect(s, (const struct sockaddr *)sin, sizeof(*sin)) != 0) {
        runtime_net_trace("sceHttpSendRequest connect to %s:%u failed: %d", c->target_ip, c->port, WSAGetLastError());
        closesocket(s);
        return -1;
    }
    if (c->redirected) {
        /* The RyuNET server answers at once: a request that something on the way holds (a
         * player's antivirus) ends after 20 s instead of keeping the game waiting. */
#ifdef _WIN32
        DWORD timeout = 20000;
#else
        struct timeval timeout = {20, 0};
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof timeout);
    }

    /* Build HTTP request: prefer the stored method name (covers CUSTOM). */
    const char *method_str = r->method_str[0] ? r->method_str : "GET";

    /* The game names its requests by the real servers' URLs (the login's is
     * "https://lavender-ps4-auth3.prod.demonware.net:443/auth/"). To the RyuNET server they go
     * first as a plain request for its own host: antivirus web shields and some networks hold a
     * plain HTTP request that names an https:// URL or Demonware's host, and the login never came
     * back ("sceHttpSendRequest empty response"). The server reads only the path. */
    const char *target = r->path;
    char host_header[96];
    snprintf(host_header, sizeof host_header, "%s", c->host);
    if (c->redirected && form != 2) {
        const char *scheme = strstr(target, "://");
        if (scheme && scheme - target <= 5) {
            target = strchr(scheme + 3, '/');
            if (!target) target = "/";
        }
        const char *name = form == 1 && address[0] ? address : c->target_ip;
        if (c->port == 80) snprintf(host_header, sizeof host_header, "%s", name);
        else snprintf(host_header, sizeof host_header, "%s:%u", name, (unsigned)c->port);
    }

    char header_buf[4096];
    int hlen = snprintf(header_buf, sizeof(header_buf),
                        "%s %s HTTP/1.1\r\n"
                        "Host: %s\r\n"
                        "User-Agent: CTR-NitroFueled/1.21 PS4\r\n"
                        "Connection: close\r\n"
                        "Content-Length: %zu\r\n",
                        method_str, target, host_header, size);

    for (int i = 0; i < r->req_header_count; i++) {
        hlen += snprintf(header_buf + hlen, sizeof(header_buf) - hlen,
                         "%s: %s\r\n", r->req_headers[i].name, r->req_headers[i].value);
    }
    /* RyuNET nickname ownership: the server lets this installation keep the nickname it first
     * used. The request is signed (X-RyuNET-Proof, the server's nick_proof.go) with the hash of
     * the installation's key for this server (BB_ONLINE_KEYHASH, from run.py): the key itself
     * never crosses the network, and a copied signature is refused. Sent only to hosts
     * redirected to the RyuNET server, never to a real one. */
    char proof[160];
    if (c->is_plain_http && hlen < (int)sizeof(header_buf) - 200 &&
        runtime_ryunet_proof(method_str, target, proof, sizeof proof)) {
        hlen += snprintf(header_buf + hlen, sizeof(header_buf) - hlen, "X-RyuNET-Proof: %s\r\n", proof);
    }
    /* The player chose to keep their save's Pit Stop items (BB_ONLINE_INVENTORY=save, from the
     * launcher's Online tab): the server then leaves the game its save's inventory. */
    const char *inventory = c->is_plain_http ? getenv("BB_ONLINE_INVENTORY") : NULL;
    if (inventory && strcmp(inventory, "save") == 0 && hlen < (int)sizeof(header_buf) - 48) {
        hlen += snprintf(header_buf + hlen, sizeof(header_buf) - hlen, "X-RyuNET-Inventory: save\r\n");
    }
    hlen += snprintf(header_buf + hlen, sizeof(header_buf) - hlen, "\r\n");

    send(s, header_buf, hlen, 0);
    if (size > 0 && post_data) {
        send(s, (const char *)post_data, (int)size, 0);
    }

    /* Read response */
    int total_read = 0;
    while (total_read < resp_size - 1) {
        int n = recv(s, resp + total_read, resp_size - 1 - total_read, 0);
        if (n <= 0) break;
        total_read += n;
    }
    resp[total_read] = '\0';
    closesocket(s);
    return total_read;
}

int32_t runtime_http_send_request(int32_t req, const void *post_data, size_t size) {
    int idx = req - 1;
    if (idx < 0 || idx >= MAX_HTTP_REQUESTS || !g_requests[idx].in_use) return HTTP_NETWORK;
    HttpRequest *r = &g_requests[idx];

    int conn_idx = r->conn_id - 1;
    if (conn_idx < 0 || conn_idx >= MAX_HTTP_CONNECTIONS || !g_conns[conn_idx].in_use) return HTTP_NETWORK;
    HttpConnection *c = &g_conns[conn_idx];

    if (!runtime_net_is_online()) {
        runtime_net_trace("sceHttpSendRequest (offline) -> HTTP_NETWORK");
        return HTTP_NETWORK;
    }

    runtime_net_trace("sceHttpSendRequest(req=%d, host=%s, target=%s:%u, path='%s', body=%zu bytes)",
                      req, c->host, c->target_ip, c->port, r->path, size);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(c->port);
    sin.sin_addr.s_addr = inet_addr(c->target_ip);

    if (sin.sin_addr.s_addr == INADDR_NONE) {
        struct hostent *he = gethostbyname(c->target_ip);
        if (he && he->h_addr_list[0]) {
            memcpy(&sin.sin_addr, he->h_addr_list[0], sizeof(sin.sin_addr));
        } else {
            return HTTP_NETWORK;
        }
    }
    char address[INET_ADDRSTRLEN] = "";
    inet_ntop(AF_INET, &sin.sin_addr, address, sizeof address);

    char resp_buf[65536];
    int total_read = 0;
    const int first = c->redirected ? __atomic_load_n(&g_ryunet_form, __ATOMIC_RELAXED) : 0;
    for (int tried = 0; tried < RYUNET_FORMS; tried++) {
        const int form = (first + tried) % RYUNET_FORMS;
        total_read = http_exchange(c, r, &sin, form, address, post_data, size, resp_buf, (int)sizeof resp_buf);
        if (total_read < 0) return HTTP_NETWORK;
        if (!c->redirected) break;
        if (!ryunet_intercepted(resp_buf, total_read, form)) {
            ryunet_note_server_clock(resp_buf);
            ryunet_keep_form(form);
            break;
        }
    }

    if (total_read <= 0) {
        runtime_net_trace("sceHttpSendRequest empty response");
        return HTTP_NETWORK;
    }

    /* Parse response */
    char *body_start = strstr(resp_buf, "\r\n\r\n");
    if (!body_start) {
        body_start = strstr(resp_buf, "\n\n");
        if (body_start) body_start += 2;
    } else {
        body_start += 4;
    }

    /* Status code */
    r->status_code = 200;
    if (strncmp(resp_buf, "HTTP/", 5) == 0) {
        char *space = strchr(resp_buf, ' ');
        if (space) r->status_code = atoi(space + 1);
    }

    /* Store headers verbatim */
    size_t header_len = body_start ? (size_t)(body_start - resp_buf) : (size_t)total_read;
    r->raw_resp_headers_len = header_len;
    r->raw_resp_headers = (char *)malloc(header_len + 1);
    if (r->raw_resp_headers) {
        memcpy(r->raw_resp_headers, resp_buf, header_len);
        r->raw_resp_headers[header_len] = '\0';
    }

    /* Parse individual headers without mutating raw_resp_headers */
    const char *line = r->raw_resp_headers;
    r->resp_header_count = 0;
    while (line && *line) {
        const char *eol = strstr(line, "\r\n");
        if (!eol) eol = strchr(line, '\n');
        size_t line_len = eol ? (size_t)(eol - line) : strlen(line);

        const char *colon = (const char *)memchr(line, ':', line_len);
        if (colon && r->resp_header_count < MAX_HEADERS) {
            size_t name_len = (size_t)(colon - line);
            if (name_len >= sizeof(r->resp_headers[0].name)) name_len = sizeof(r->resp_headers[0].name) - 1;
            memcpy(r->resp_headers[r->resp_header_count].name, line, name_len);
            r->resp_headers[r->resp_header_count].name[name_len] = '\0';

            const char *v = colon + 1;
            while (*v == ' ' && v < line + line_len) v++;
            size_t val_len = (size_t)((line + line_len) - v);
            if (val_len >= sizeof(r->resp_headers[0].value)) val_len = sizeof(r->resp_headers[0].value) - 1;
            memcpy(r->resp_headers[r->resp_header_count].value, v, val_len);
            r->resp_headers[r->resp_header_count].value[val_len] = '\0';

            if (_stricmp(r->resp_headers[r->resp_header_count].name, "X-Signature") == 0) {
                runtime_net_trace("sceHttpSendRequest FOUND X-Signature: %s", r->resp_headers[r->resp_header_count].value);
            }
            r->resp_header_count++;
        }
        if (!eol) break;
        line = eol + (*eol == '\r' ? 2 : 1);
    }

    /* Store body */
    if (body_start) {
        size_t blen = total_read - (body_start - resp_buf);
        r->resp_body_len = blen;
        r->resp_body = (char *)malloc(blen + 1);
        if (r->resp_body) {
            memcpy(r->resp_body, body_start, blen);
            r->resp_body[blen] = '\0';
        }
    } else {
        r->resp_body_len = 0;
        r->resp_body = NULL;
    }
    r->resp_read_offset = 0;

    runtime_net_trace("sceHttpSendRequest -> status=%d, body=%zu bytes", r->status_code, r->resp_body_len);
    return ORBIS_OK;
}

int32_t runtime_http_abort_request(int32_t req) {
    (void)req;
    return ORBIS_OK;
}

int32_t runtime_http_get_status_code(int32_t req, int32_t *status_code) {
    int idx = req - 1;
    if (idx < 0 || idx >= MAX_HTTP_REQUESTS || !g_requests[idx].in_use) return HTTP_NETWORK;
    if (status_code) *status_code = g_requests[idx].status_code;
    runtime_net_trace("sceHttpGetStatusCode(req=%d) -> %d", req, g_requests[idx].status_code);
    return ORBIS_OK;
}

int32_t runtime_http_get_response_content_length(int32_t req, int32_t *result, uint64_t *content_length) {
    int idx = req - 1;
    if (idx < 0 || idx >= MAX_HTTP_REQUESTS || !g_requests[idx].in_use) return HTTP_NETWORK;
    if (result) *result = 0;
    if (content_length) *content_length = (uint64_t)g_requests[idx].resp_body_len;
    runtime_net_trace("sceHttpGetResponseContentLength(req=%d) -> %zu bytes",
                      req, g_requests[idx].resp_body_len);
    return ORBIS_OK;
}

int32_t runtime_http_read_data(int32_t req, void *data, uint64_t size) {
    int idx = req - 1;
    if (idx < 0 || idx >= MAX_HTTP_REQUESTS || !g_requests[idx].in_use || !data) return HTTP_NETWORK;
    HttpRequest *r = &g_requests[idx];

    if (r->resp_read_offset >= r->resp_body_len) return 0; /* EOF */

    size_t avail = r->resp_body_len - r->resp_read_offset;
    size_t to_copy = (avail < size) ? avail : (size_t)size;

    memcpy(data, r->resp_body + r->resp_read_offset, to_copy);
    r->resp_read_offset += to_copy;

    runtime_net_trace("sceHttpReadData(req=%d) -> read %zu bytes (%zu remaining)",
                      req, to_copy, r->resp_body_len - r->resp_read_offset);
    return (int32_t)to_copy;
}

int32_t runtime_http_get_all_response_headers(int32_t req, char **headers, uint64_t *length) {
    int idx = req - 1;
    if (idx < 0 || idx >= MAX_HTTP_REQUESTS || !g_requests[idx].in_use) return HTTP_NETWORK;
    HttpRequest *r = &g_requests[idx];

    if (headers) *headers = r->raw_resp_headers;
    if (length) *length = (uint64_t)r->raw_resp_headers_len;
    runtime_net_trace("sceHttpGetAllResponseHeaders(req=%d) -> %zu bytes",
                      req, r->raw_resp_headers_len);
    return ORBIS_OK;
}

/* PS4-faithful blob parser (ref: shadPS4 sceHttpParseResponseHeader): the game
 * passes the blob from get_all_response_headers, NOT a request id. The old
 * code used (req, name) and always failed, leaving the game with a garbage
 * signature pointer -> heap corruption and the 0x14a0383 crash after auth.
 * *value points INSIDE the blob (not NUL-terminated). */
int32_t runtime_http_parse_response_header(const char *blob, uint64_t blob_len, const char *name,
                                           const char **value, uint64_t *length) {
    if (!blob) {
        runtime_net_trace("sceHttpParseResponseHeader(NULL blob) -> INVALID_RESPONSE");
        return HTTP_PARSE_INVALID_RESPONSE;
    }
    if (!name || !value || !length) {
        runtime_net_trace("sceHttpParseResponseHeader -> INVALID_VALUE");
        return HTTP_PARSE_INVALID_VALUE;
    }

    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > 256 || blob_len > (1 << 20)) {
        runtime_net_trace("sceHttpParseResponseHeader('%s') -> INVALID_VALUE", name);
        return HTTP_PARSE_INVALID_VALUE;
    }

    /* Scan line by line for "Name:" at a line start (case-insensitive). */
    uint64_t value_off = 0;
    int found = 0;
    uint64_t cur = 0;
    int at_line_start = 1;
    while (cur < blob_len) {
        if (at_line_start && !_strnicmp(blob + cur, name, name_len) &&
            cur + name_len < blob_len && blob[cur + name_len] == ':' &&
            (cur == 0 || blob[cur - 1] == '\n' || blob[cur - 1] == '\r')) {
            value_off = cur + name_len + 1;
            found = 1;
            break;
        }
        at_line_start = (blob[cur] == '\n');
        cur++;
    }
    if (!found) {
        runtime_net_trace("sceHttpParseResponseHeader('%s') -> NOT FOUND", name);
        return HTTP_PARSE_NOT_FOUND;
    }

    /* Skip leading whitespace (a bare LF ends an empty value, like shadPS4). */
    while (value_off < blob_len) {
        unsigned char ch = (unsigned char)blob[value_off];
        if (ch == ' ' || ch == '\t' || ch == '\r') { value_off++; continue; }
        if (ch == '\n') { value_off++; break; }
        break;
    }
    uint64_t value_start = value_off;

    /* Value runs to end of line; strip a trailing CR; honor folded lines. */
    uint64_t scan = value_off, line_end = blob_len, length_end = blob_len;
    while (scan < blob_len) {
        while (scan < blob_len && blob[scan] != '\n') scan++;
        if (scan >= blob_len) { length_end = blob_len; line_end = blob_len; break; }
        int has_cr = (scan > value_start && blob[scan - 1] == '\r');
        uint64_t after = scan + 1;
        if (after < blob_len && (blob[after] == ' ' || blob[after] == '\t')) { scan = after; continue; }
        line_end = after;
        length_end = has_cr ? scan - 1 : scan;
        break;
    }

    uint64_t final_len = (length_end > value_start) ? length_end - value_start : 0;
    if (final_len == 0) {
        *value = NULL;
        *length = 0;
        runtime_net_trace("sceHttpParseResponseHeader('%s') -> empty", name);
        return 0;
    }
    *value = blob + value_start;
    *length = final_len;
    if (final_len < 128) {
        char preview[128];
        memcpy(preview, blob + value_start, (size_t)final_len);
        preview[final_len] = '\0';
        runtime_net_trace("sceHttpParseResponseHeader('%s') -> '%s'", name, preview);
    } else {
        runtime_net_trace("sceHttpParseResponseHeader('%s') -> %llu bytes", name,
                          (unsigned long long)final_len);
    }
    return (int32_t)line_end;
}

int32_t runtime_http_create_epoll(int32_t ctx, void **handle) {
    (void)ctx;
    if (!handle) return HTTP_INVALID_VALUE;
    static int next_http_epoll = 0x200;
    *handle = (void *)(uintptr_t)(next_http_epoll++);
    return ORBIS_OK;
}

int32_t runtime_http_destroy_epoll(void *handle) {
    (void)handle;
    return ORBIS_OK;
}

int32_t runtime_http_set_epoll(void *handle, int32_t req, void *user_data) {
    (void)handle; (void)req; (void)user_data;
    return ORBIS_OK;
}

int32_t runtime_http_unset_epoll(void *handle, int32_t req) {
    (void)handle; (void)req;
    return ORBIS_OK;
}

int32_t runtime_http_wait_request(void *handle, void *events, int32_t max_events, int64_t timeout_usec) {
    (void)handle; (void)events; (void)max_events; (void)timeout_usec;
    /* Since requests complete synchronously in send_request, report event immediately */
    return 1;
}
