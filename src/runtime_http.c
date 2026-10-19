#include "runtime_http.h"
#include "runtime_net.h"
#include "runtime_psn.h"
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
                runtime_net_trace("sceHttpCreateConnectionWithURL('%s') -> override %s:%u (HTTP)",
                                  url, c->target_ip, c->port);
            } else {
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

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return HTTP_NETWORK;

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
            closesocket(s);
            return HTTP_NETWORK;
        }
    }

    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        runtime_net_trace("sceHttpSendRequest connect to %s:%u failed: %d", c->target_ip, c->port, WSAGetLastError());
        closesocket(s);
        return HTTP_NETWORK;
    }

    /* Build HTTP request: prefer the stored method name (covers CUSTOM). */
    const char *method_str = r->method_str[0] ? r->method_str : "GET";

    char header_buf[4096];
    int hlen = snprintf(header_buf, sizeof(header_buf),
                        "%s %s HTTP/1.1\r\n"
                        "Host: %s\r\n"
                        "User-Agent: CTR-NitroFueled/1.21 PS4\r\n"
                        "Connection: close\r\n"
                        "Content-Length: %zu\r\n",
                        method_str, r->path, c->host, size);

    for (int i = 0; i < r->req_header_count; i++) {
        hlen += snprintf(header_buf + hlen, sizeof(header_buf) - hlen,
                         "%s: %s\r\n", r->req_headers[i].name, r->req_headers[i].value);
    }
    /* RyuNET nickname ownership: the server lets this installation keep the nickname it first
     * used. The request is signed (X-RyuNET-Proof, the server's nick_proof.go) with the hash of
     * the installation's key for this server (BB_ONLINE_KEYHASH, from run.py): the key itself
     * never crosses the network, and a copied signature is refused. Sent only to hosts
     * redirected to the RyuNET server, never to a real one. */
    const char *key_hash = c->is_plain_http && !runtime_psn_selected() ? getenv("BB_ONLINE_KEYHASH") : NULL;
    if (key_hash && strlen(key_hash) == 64 && strspn(key_hash, "0123456789abcdef") == 64 &&
        hlen < (int)sizeof(header_buf) - 200) {
        static unsigned long long counter;
        struct timespec now;
        timespec_get(&now, TIME_UTC);
        char nonce[33], message[1200];
        snprintf(nonce, sizeof nonce, "%016llx%016llx",
                 (unsigned long long)now.tv_sec * 1000000000ull + (unsigned long long)now.tv_nsec,
                 __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED) ^ (unsigned long long)(uintptr_t)&now);
        const int n = snprintf(message, sizeof message, "%lld\n%s\n%s\n%s", (long long)now.tv_sec, nonce,
                               method_str, r->path);
        if (n > 0 && n < (int)sizeof message) {
            uint8_t mac[32];
            bb_hmac_sha256((const unsigned char *)key_hash, 64, (const unsigned char *)message, (size_t)n, mac);
            char mac_hex[65];
            for (int i = 0; i < 32; i++) snprintf(mac_hex + 2 * i, 3, "%02x", mac[i]);
            hlen += snprintf(header_buf + hlen, sizeof(header_buf) - hlen, "X-RyuNET-Proof: 1.%lld.%s.%s\r\n",
                             (long long)now.tv_sec, nonce, mac_hex);
        }
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
    char resp_buf[65536];
    int total_read = 0;
    while (total_read < (int)sizeof(resp_buf) - 1) {
        int n = recv(s, resp_buf + total_read, (int)sizeof(resp_buf) - 1 - total_read, 0);
        if (n <= 0) break;
        total_read += n;
    }
    resp_buf[total_read] = '\0';
    closesocket(s);

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
