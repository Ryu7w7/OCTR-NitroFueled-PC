/* The game's requests to the RyuNET server (src/runtime_http.c), against a fake server on this
 * PC whose clock is an hour ahead and which, like nick_proof.go, takes a signature only within
 * 5 minutes of its own time: the login goes as a plain request for the server's own host, the
 * first signature is refused, and the answer's Date puts the next one on the server's time.
 * Then something on the way (a router's or an antivirus's filter) answers with a redirect for
 * requests that name the server, and later for its address too: the requests go again in the
 * next form, which the following ones keep. */
#include "runtime_http.h"
#include "runtime_net.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

enum { SKEW = 3600 };
static SOCKET listener;
static unsigned short server_port;
static char last_line[256], last_host[128];
static long long last_ts;
static int intercept; /* 1: requests for the server's name; 2: also for its address; 3: all */
static int requests;

/* What src/runtime_net.c and runtime_net_overrides.c give runtime_http.c. */
int runtime_net_is_online(void) { return 1; }
int32_t runtime_net_init(void) { return 0; }
void runtime_net_trace(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    putchar('\n');
}
const char *net_overrides_lookup(const char *host, uint16_t *port_override, int *is_http) {
    if (strcmp(host, "lavender-ps4-auth3.prod.demonware.net") != 0) return NULL;
    *port_override = server_port;
    *is_http = 1;
    return "localhost";
}

/* One request: its line, Host and signature time; the answer as nick_proof.go would give it. */
static void serve_one(void) {
    SOCKET s = accept(listener, NULL, NULL);
    if (s == INVALID_SOCKET) return;
    char request[8192];
    int got = 0;
    for (int n; got < (int)sizeof request - 1 && (n = recv(s, request + got, (int)sizeof request - 1 - got, 0)) > 0;) {
        got += n;
        request[got] = '\0';
        const char *end = strstr(request, "\r\n\r\n");
        const char *length = strstr(request, "Content-Length: ");
        if (end && length && got >= (int)(end + 4 - request) + atoi(length + 16)) break;
    }
    request[got] = '\0';
    sscanf(request, "%255[^\r]", last_line);
    const char *host = strstr(request, "\r\nHost: ");
    last_host[0] = '\0';
    if (host) sscanf(host + 8, "%127[^\r]", last_host);
    const char *proof = strstr(request, "X-RyuNET-Proof: 1.");
    last_ts = proof ? atoll(proof + 18) : 0;

    const time_t now = time(NULL) + SKEW;
    char date[64];
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", gmtime(&now));
    const int ok = llabs(last_ts - (long long)now) <= 300;
    char reply[512];
    requests++;
    if ((intercept >= 1 && !strncmp(last_host, "localhost", 9)) ||
        (intercept >= 2 && !strncmp(last_host, "127.0.0.1", 9)) || intercept >= 3) {
        const int n = snprintf(reply, sizeof reply, "HTTP/1.1 302 Found\r\nLocation: http://filter.example/blocked\r\n"
                                                    "Date: Thu, 01 Jan 2015 00:00:00 GMT\r\nContent-Length: 0\r\n\r\n");
        send(s, reply, n, 0);
        closesocket(s);
        return;
    }
    const int n = snprintf(reply, sizeof reply,
                           "HTTP/1.1 %s\r\nDate: %s\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}",
                           ok ? "200 OK" : "403 Forbidden", date);
    send(s, reply, n, 0);
    closesocket(s);
}

#ifdef _WIN32
static DWORD WINAPI server_thread(LPVOID unused) {
    (void)unused;
    for (;;) serve_one();
    return 0;
}
#else
static void *server_thread(void *unused) {
    (void)unused;
    for (;;) serve_one();
    return NULL;
}
#endif

static int login(void) {
    const char *url = "https://lavender-ps4-auth3.prod.demonware.net:443/auth/";
    const int32_t tmpl = runtime_http_create_template(1, "SCE-PS4", 1, 1);
    const int32_t conn = runtime_http_create_connection(tmpl, url, 1);
    const int32_t req = runtime_http_create_request2(conn, "POST", url, 7);
    CHECK(conn > 0 && req > 0);
    runtime_http_add_request_header(req, "Content-Type", "application/json", 0);
    int32_t status = 0;
    CHECK(runtime_http_send_request(req, "{\"a\":1}", 7) == 0);
    runtime_http_get_status_code(req, &status);
    runtime_http_delete_request(req);
    runtime_http_delete_connection(conn);
    return status;
}

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    putenv("BB_ONLINE_KEYHASH=00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof addr;
    if (bind(listener, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(listener, 4) != 0 ||
        getsockname(listener, (struct sockaddr *)&addr, &len) != 0) {
        fprintf(stderr, "no local socket\n");
        return 1;
    }
    server_port = ntohs(addr.sin_port);
#ifdef _WIN32
    CreateThread(NULL, 0, server_thread, NULL, 0, NULL);
#else
    pthread_t thread;
    pthread_create(&thread, NULL, server_thread, NULL);
#endif
    runtime_http_init();

    char host[64], address[64], port[16];
    snprintf(host, sizeof host, "localhost:%u", server_port);
    snprintf(address, sizeof address, "127.0.0.1:%u", server_port);
    snprintf(port, sizeof port, "%u", server_port);
    /* The PC's clock: refused, but the answer tells the server's time. */
    CHECK(login() == 403);
    CHECK(strcmp(last_line, "POST /auth/ HTTP/1.1") == 0);
    CHECK(strcmp(last_host, host) == 0);
    CHECK(llabs(last_ts - (long long)time(NULL)) <= 2);
    /* The game's next try: the server's time. */
    CHECK(login() == 200);
    CHECK(llabs(last_ts - ((long long)time(NULL) + SKEW)) <= 3);
    CHECK(login() == 200);

    /* A filter redirects requests for the server's name: the login goes again for its address
     * (on the server's time still: the redirect's Date is not the server's), and the next
     * requests go that way at once. */
    intercept = 1;
    requests = 0;
    CHECK(login() == 200);
    CHECK(requests == 2);
    CHECK(strcmp(last_host, address) == 0);
    CHECK(llabs(last_ts - ((long long)time(NULL) + SKEW)) <= 3);
    requests = 0;
    CHECK(login() == 200);
    CHECK(requests == 1);

    /* It redirects the address too: the game's own URL, as v0.6 sent it. */
    intercept = 2;
    requests = 0;
    CHECK(login() == 200);
    CHECK(requests == 2);
    CHECK(strcmp(last_line, "POST https://lavender-ps4-auth3.prod.demonware.net:443/auth/ HTTP/1.1") == 0);
    CHECK(strcmp(last_host, "lavender-ps4-auth3.prod.demonware.net") == 0);

    /* The port's own requests (friends, reports) go the way that went through. */
    char env_server[] = "BB_ONLINE_SERVER=localhost", env_port[48], body[64];
    snprintf(env_port, sizeof env_port, "BB_ONLINE_HTTP_PORT=%s", port);
    putenv(env_server);
    putenv(env_port);
    requests = 0;
    CHECK(runtime_ryunet_request("POST", "/ryunet/friends/privacy?name=a&hidden=0&format=text", body, sizeof body) == 200);
    CHECK(requests == 1);
    CHECK(strcmp(last_line, "POST /ryunet/friends/privacy?name=a&hidden=0&format=text HTTP/1.1") == 0);

    /* Every form redirected: the game sees the redirect, as before. */
    intercept = 3;
    requests = 0;
    CHECK(runtime_ryunet_request("POST", "/ryunet/friends/privacy?name=a&hidden=0&format=text", body, sizeof body) == 302);
    CHECK(requests == 3);

    if (failures) {
        fprintf(stderr, "FAIL: %d checks\n", failures);
        return 1;
    }
    puts("PASS: RyuNET login as a plain request, signatures on the server's clock, intercepted forms retried");
    return 0;
}
