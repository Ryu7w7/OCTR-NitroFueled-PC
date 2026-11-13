/* ryunet-bridge: a player's RyuNET traffic through Epic Online Services, for networks that do
 * not reach the server (an ISP that filters its name or its address, ports blocked on the way).
 *
 *   client   on the player's PC, started by the launcher. It first checks whether the server
 *            answers directly (HTTP, lobby TCP, NAT UDP); when it does, it says so and exits.
 *            Otherwise it logs in to EOS anonymously (Device ID: no Epic account), opens a P2P
 *            connection to the gateway, through Epic's relays when there is no direct path, and
 *            offers local ports on 127.0.0.1 that the launcher, run.py and the game use instead
 *            of the server's (BB_ONLINE_BRIDGE, src/runtime_net_bridge.c).
 *   gateway  next to the RyuNET server. It accepts those connections and hands each player's
 *            traffic to the server on 127.0.0.1, telling it which player it is by a virtual
 *            address of its own (198.18.0.0/15, kept per EOS user): a PROXY v1 line for TCP and
 *            a RYUGW1 header for UDP (the server's eos_gateway.go).
 *
 * Messages to and from the gateway, on its P2P socket "RYUNETGW" (first byte, then big-endian fields):
 *   channel 0, reliable    H ver flags                   client -> gateway   hello
 *                          W ver serverIP[4] vip[4]       gateway -> client   welcome
 *                          R reason...                    gateway -> client   refused
 *                          P stamp[4] / Q stamp[4]        either way          ping / pong
 *   channel 1, unreliable  U port[2] id[2] index count data                   a UDP datagram
 *                          (port: the game socket's local port; datagrams are cut in fragments)
 *   channel 2, reliable    O sid[4] port[2]               client -> gateway   open a TCP stream
 *                          D sid[4] data                  either way          stream data
 *                          C sid[4] how                   either way          0 end, 1 reset
 *
 * Peers (races through EOS P2P, src/runtime_net_eos.h): the game asks this bridge, on its UDP
 * port, to carry datagrams to other players by their EOS ids ("RYUEP1" Q / K / S, answered with I
 * and R). Each pair of players has a socket of its own ("RYP" and a hash of both ids), and only
 * players the game named may connect on it. Peers work in both modes: with the server reached
 * directly too (BRIDGE DIRECT, then BRIDGE PEERS), when EOS is set up.
 *
 * The status for the launcher goes to stdout, one line each:
 *   BRIDGE CHECKING | BRIDGE DIRECT | BRIDGE CONNECTING | BRIDGE READY ip=.. vip=.. map=..
 *   BRIDGE PEERS port=<udp> id=<product user id>   BRIDGE LOST | BRIDGE FAILED <reason>
 *   (gateway: GATEWAY ID <product user id>)
 * map= is what the game takes as BB_ONLINE_BRIDGE: "<udp port>,<server port>:<local port>,...".
 *
 * --debug-gateway / --debug-listen replace EOS by a plain TCP connection between the two ends,
 * with the same messages, to test everything else without Epic. */
#ifndef _WIN32
/* Not _GNU_SOURCE: with glibc 2.38 it turns scanf/strtol into their C23 versions, which a
 * server with an older glibc (Ubuntu 22.04) does not have. */
#define _DEFAULT_SOURCE
#endif

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#define poll_fn WSAPoll
#define SEND_FLAGS 0
static int sock_err(void) { return WSAGetLastError(); }
static int err_wouldblock(int e) { return e == WSAEWOULDBLOCK; }
static int err_inprogress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
#else
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
#define poll_fn poll
#define SEND_FLAGS MSG_NOSIGNAL
static int sock_err(void) { return errno; }
static int err_wouldblock(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
static int err_inprogress(int e) { return e == EINPROGRESS; }
#endif

#include "eos_sdk.h"
#include "eos_logging.h"
#include "eos_connect.h"
#include "eos_p2p.h"

#define BRIDGE_VERSION 1
#define SOCKET_NAME "RYUNETGW"          /* the gateway's; peers' are per pair (peer_socket) */
#define CH_CONTROL 0
#define CH_UDP 1
#define CH_STREAM 2
#define CH_PEER 3
#define PEER_MAGIC "RYUEP1"
#define PEER_MAGIC_LEN 6
#define RACE_PEER_IDLE_MS 120000u
#define P2P_MAX 1170                  /* EOS_P2P_MAX_PACKET_SIZE */
#define UDP_HEAD 7                    /* U port id index count */
#define UDP_FRAG (P2P_MAX - UDP_HEAD)
#define UDP_MAX_FRAGS 4
#define STREAM_CHUNK (P2P_MAX - 5)    /* D sid */
#define OUT_QUEUE_LIMIT (2u << 20)    /* stop reading TCP while EOS holds this much to send */
#define STREAM_BUFFER_LIMIT (16u << 20)
#define MAX_PEERS 2048
#define MAX_STREAMS 4096
#define MAX_STREAMS_PER_PEER 48
#define MAX_PORTS 16
#define PING_MS 4000u
#define LOST_MS 20000u
#define PEER_IDLE_MS 90000u
#define HELLO_MS 2000u
#define UDP_PPS_LIMIT 1500

/* ------------------------------------------------------------------ small helpers */

static uint64_t now_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static int g_verbose;

/* Milliseconds since t: 0 when t is later than now (a packet handled after the loop read the
 * clock stamps a later time than its now). */
static uint64_t ago(uint64_t now, uint64_t t) { return now > t ? now - t : 0; }
static int g_exit_on_eof; /* --exit-on-eof: stdin's end (the launcher and run.py gone) ends the bridge */

#ifdef _WIN32
static DWORD WINAPI eof_watch(LPVOID unused) {
    (void)unused;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    char b[64];
    DWORD n = 0;
    while (ReadFile(in, b, sizeof b, &n, NULL) && n) {}
    ExitProcess(0);
    return 0;
}
#endif

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[bridge] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}
#define LOG(...) logf_(__VA_ARGS__)
#define VLOG(...) do { if (g_verbose) logf_(__VA_ARGS__); } while (0)

static void status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("BRIDGE ");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

typedef struct { uint8_t *p; size_t len, cap; } Buf;

static int buf_add(Buf *b, const void *d, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n) cap *= 2;
        uint8_t *p = realloc(b->p, cap);
        if (!p) return 0;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
    return 1;
}

static void buf_drop(Buf *b, size_t n) {
    if (n >= b->len) { b->len = 0; return; }
    memmove(b->p, b->p + n, b->len - n);
    b->len -= n;
}

static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

static void set_nonblock(sock_t s) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static void set_nodelay(sock_t s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
}

static struct sockaddr_in addr4(uint32_t ip_be, uint16_t port) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip_be;
    a.sin_port = htons(port);
    return a;
}

static const char *ip_text(uint32_t ip_be, char *out) {
    const uint8_t *b = (const uint8_t *)&ip_be;
    sprintf(out, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return out;
}

static int resolve4(const char *host, uint32_t *ip_be) {
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) == 1) { *ip_be = a.s_addr; return 1; }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return 0;
    *ip_be = ((const struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return 1;
}

static void make_dirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        const char c = *p;
        *p = 0;
#ifdef _WIN32
        _mkdir(tmp);
#else
        mkdir(tmp, 0700);
#endif
        *p = c;
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0700);
#endif
}

static void exe_dir(char *out, size_t cap) {
    out[0] = 0;
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) { snprintf(out, cap, "."); return; }
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) { snprintf(out, cap, "."); return; }
    out[n] = 0;
#endif
    char *slash = strrchr(out, '/');
#ifdef _WIN32
    char *bslash = strrchr(out, '\\');
    if (!slash || (bslash && bslash > slash)) slash = bslash;
#endif
    if (slash) *slash = 0;
    else snprintf(out, cap, ".");
}

/* ------------------------------------------------------------------ settings */

typedef struct {
    int gateway;                 /* role */
    int mode_on;                 /* client: skip the direct check */
    char server[256];            /* client: the server's name or address */
    uint16_t http_port;
    uint16_t nat_port;
    uint16_t ports[MAX_PORTS];   /* the server's TCP ports offered (client) or allowed (gateway) */
    int port_count;
    uint16_t base_port;          /* client: first local port */
    uint16_t base_port_bound;    /* client: the UDP port it got */
    char config[1024];
    char data_dir[1024];
    char gateway_id[64];         /* client: the gateway's product user id */
    char debug_gateway[128];     /* client: host:port, TCP instead of EOS */
    char debug_listen[128];      /* gateway: host:port, TCP instead of EOS (also with EOS) */
    int no_eos;                  /* gateway: debug transport only */
    int force_relays;            /* --force-relays: only through Epic's relays (tests, or to hide addresses) */
    char server_ip[64];          /* gateway: the server's public address, told to clients */
    char target[64];             /* gateway: where the server listens (127.0.0.1) */
    char product_id[128], sandbox_id[128], deployment_id[128], client_id[128], client_secret[128];
} Settings;

static Settings S;

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

/* One "key = value" line; a value already set (an earlier source) stays. */
static void config_line(char *line) {
    {
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';' || line[0] == '[') return;
        char *eq = strchr(line, '=');
        if (!eq) return;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        struct { const char *name; char *dst; size_t cap; } keys[] = {
            {"product_id", S.product_id, sizeof S.product_id},
            {"sandbox_id", S.sandbox_id, sizeof S.sandbox_id},
            {"deployment_id", S.deployment_id, sizeof S.deployment_id},
            {"client_id", S.client_id, sizeof S.client_id},
            {"client_secret", S.client_secret, sizeof S.client_secret},
            {"gateway_id", S.gateway_id, sizeof S.gateway_id},
        };
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
            if (strcmp(k, keys[i].name) == 0 && !keys[i].dst[0]) snprintf(keys[i].dst, keys[i].cap, "%s", v);
    }
}

/* eosbridge.ini next to the binary, when there is one: it overrides what is built in. */
static void load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) config_line(line);
    fclose(f);
}

#ifdef EOS_EMBEDDED
#include EOS_EMBEDDED
/* The settings an official build carries (eosbridge/embed_settings.py), for what the file left unset. */
static void load_embedded(void) {
    char line[512];
    size_t n = 0;
    for (size_t i = 0; i < sizeof eos_embedded; i++) {
        const char c = (char)(eos_embedded[i] ^ eos_embedded_key[i % sizeof eos_embedded_key]);
        if (c == '\n' || n == sizeof line - 1) {
            line[n] = 0;
            config_line(line);
            n = 0;
            continue;
        }
        line[n++] = c;
    }
    memset(line, 0, sizeof line);
}
#else
static void load_embedded(void) {}
#endif

static int parse_ports(const char *list) {
    S.port_count = 0;
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", list);
    for (char *t = strtok(tmp, ", "); t && S.port_count < MAX_PORTS; t = strtok(NULL, ", ")) {
        const int p = atoi(t);
        if (p > 0 && p < 65536) S.ports[S.port_count++] = (uint16_t)p;
    }
    return S.port_count;
}

static int port_allowed(uint16_t port) {
    for (int i = 0; i < S.port_count; i++)
        if (S.ports[i] == port) return 1;
    return 0;
}

static int split_hostport(const char *text, char *host, size_t cap, uint16_t *port) {
    const char *colon = strrchr(text, ':');
    if (!colon) return 0;
    snprintf(host, cap, "%.*s", (int)(colon - text), text);
    const int p = atoi(colon + 1);
    if (p <= 0 || p > 65535) return 0;
    *port = (uint16_t)p;
    return 1;
}

/* ------------------------------------------------------------------ EOS, loaded at run time */

#define EOS_FUNCS(X) \
    X(EOS_Initialize) X(EOS_Platform_Create) X(EOS_Platform_Tick) \
    X(EOS_Platform_GetConnectInterface) X(EOS_Platform_GetP2PInterface) \
    X(EOS_Logging_SetCallback) X(EOS_Logging_SetLogLevel) X(EOS_EResult_ToString) \
    X(EOS_ProductUserId_ToString) X(EOS_ProductUserId_FromString) \
    X(EOS_Connect_CreateDeviceId) X(EOS_Connect_Login) X(EOS_Connect_CreateUser) \
    X(EOS_Connect_AddNotifyAuthExpiration) \
    X(EOS_P2P_SendPacket) X(EOS_P2P_GetNextReceivedPacketSize) X(EOS_P2P_ReceivePacket) \
    X(EOS_P2P_AddNotifyPeerConnectionRequest) X(EOS_P2P_AddNotifyPeerConnectionClosed) \
    X(EOS_P2P_AddNotifyPeerConnectionEstablished) X(EOS_P2P_AcceptConnection) \
    X(EOS_P2P_CloseConnection) X(EOS_P2P_SetRelayControl) X(EOS_P2P_SetPacketQueueSize) \
    X(EOS_P2P_GetPacketQueueInfo)

#define X(name) static __typeof__(name) *p_##name;
EOS_FUNCS(X)
#undef X

static EOS_HPlatform g_platform;
static EOS_HConnect g_connect;
static EOS_HP2P g_p2p;
static EOS_ProductUserId g_local;
static char g_local_id[64];
static EOS_P2P_SocketId g_socket;
static int g_eos_on;          /* EOS is the transport (not --debug-*) */
static int g_eos_ready;       /* logged in, P2P set up */
static int g_eos_failed;
static int g_peers_only;      /* client: the server is reached directly, EOS only for races */
static uint64_t g_eos_retry_at;

static const char *eos_text(EOS_EResult r) { return p_EOS_EResult_ToString ? p_EOS_EResult_ToString(r) : "?"; }

#if defined(_WIN32) && defined(EOS_LINKED)
/* Linked with the SDK's import library (eosbridge/build.sh): Windows loads EOSSDK-Win64-Shipping.dll
 * with the program, from its folder, and the SDK's functions are named in the import table. A
 * program that only resolved them by hand at run time (LoadLibrary + GetProcAddress) and talks to the
 * network was what antivirus heuristics took for a trojan (v0.8.4's bridge). */
static int eos_load(void) {
#define X(name) p_##name = name;
    EOS_FUNCS(X)
#undef X
    return 1;
}
#else
static int eos_load(void) {
    char dir[1024], path[1200];
    exe_dir(dir, sizeof dir);
#ifdef _WIN32
    snprintf(path, sizeof path, "%s\\EOSSDK-Win64-Shipping.dll", dir);
    HMODULE lib = LoadLibraryA(path);
    if (!lib) lib = LoadLibraryA("EOSSDK-Win64-Shipping.dll");
    if (!lib) { LOG("EOS SDK not found (%s)", path); return 0; }
#define SYM(name) ((void *)GetProcAddress(lib, name))
#else
#if defined(__aarch64__)
#define EOS_LIB "libEOSSDK-LinuxArm64-Shipping.so"   /* an ARM server (the production one) */
#else
#define EOS_LIB "libEOSSDK-Linux-Shipping.so"
#endif
    snprintf(path, sizeof path, "%s/" EOS_LIB, dir);
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen(EOS_LIB, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { LOG("EOS SDK not found (%s): %s", path, dlerror()); return 0; }
#define SYM(name) dlsym(lib, name)
#endif
#define X(name) \
    { void *f = SYM(#name); if (!f) { LOG("EOS SDK without %s", #name); return 0; } memcpy(&p_##name, &f, sizeof f); }
    EOS_FUNCS(X)
#undef X
#undef SYM
    return 1;
}
#endif

static void EOS_CALL eos_log(const EOS_LogMessage *m) {
    if (m->Level <= EOS_LOG_Warning || g_verbose) LOG("EOS %s: %s", m->Category, m->Message);
}

static int eos_start(void) {
    if (!S.product_id[0] || !S.sandbox_id[0] || !S.deployment_id[0] || !S.client_id[0] || !S.client_secret[0]) {
        LOG("EOS settings missing (product_id, sandbox_id, deployment_id, client_id, client_secret in %s)", S.config);
        return 0;
    }
    if (!eos_load()) return 0;
    EOS_InitializeOptions init;
    memset(&init, 0, sizeof init);
    init.ApiVersion = EOS_INITIALIZE_API_LATEST;
    init.ProductName = "CTR-NitroFueled-PC RyuNET";
    init.ProductVersion = "1";
    EOS_EResult r = p_EOS_Initialize(&init);
    if (r != EOS_Success && r != EOS_AlreadyConfigured) { LOG("EOS_Initialize: %s", eos_text(r)); return 0; }
    p_EOS_Logging_SetCallback(eos_log);
    p_EOS_Logging_SetLogLevel(EOS_LC_ALL_CATEGORIES, g_verbose ? EOS_LOG_Info : EOS_LOG_Warning);
    make_dirs(S.data_dir);
    EOS_Platform_Options o;
    memset(&o, 0, sizeof o);
    o.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
    o.ProductId = S.product_id;
    o.SandboxId = S.sandbox_id;
    o.DeploymentId = S.deployment_id;
    o.ClientCredentials.ClientId = S.client_id;
    o.ClientCredentials.ClientSecret = S.client_secret;
    o.bIsServer = EOS_FALSE;
    o.Flags = EOS_PF_DISABLE_OVERLAY | EOS_PF_DISABLE_SOCIAL_OVERLAY;
    o.CacheDirectory = S.data_dir;
    g_platform = p_EOS_Platform_Create(&o);
    if (!g_platform) { LOG("EOS_Platform_Create failed (check the ids in %s)", S.config); return 0; }
    g_connect = p_EOS_Platform_GetConnectInterface(g_platform);
    g_p2p = p_EOS_Platform_GetP2PInterface(g_platform);
    g_socket.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
    snprintf(g_socket.SocketName, sizeof g_socket.SocketName, "%s", SOCKET_NAME);
    return 1;
}

/* ------------------------------------------------------------------ peers and transport */

typedef struct {
    uint16_t port, id;
    uint8_t count, got;
    uint16_t len[UDP_MAX_FRAGS];
    uint8_t data[UDP_MAX_FRAGS][UDP_FRAG];
} Reassembly;

typedef struct {
    int used;
    char id[80];
    EOS_ProductUserId puid;
    sock_t dbg;                  /* debug transport */
    Buf dbg_in, dbg_out;
    int welcomed;                /* gateway: the client said hello */
    uint32_t vip;                /* gateway: its virtual address */
    uint64_t last_rx;
    uint64_t udp_window;
    unsigned udp_count;
    unsigned streams;
    uint16_t udp_id;
    Reassembly frag;
    int is_peer;                 /* client: another player (races), not the gateway */
    int allowed;                 /* the game named it: it may connect */
    EOS_P2P_SocketId sock;       /* its socket (peers) */
} Peer;

static Peer *g_peers[MAX_PEERS];

static Peer *peer_find(const char *id) {
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i] && strcmp(g_peers[i]->id, id) == 0) return g_peers[i];
    return NULL;
}

static Peer *peer_new(const char *id) {
    for (int i = 0; i < MAX_PEERS; i++) {
        if (g_peers[i]) continue;
        Peer *p = calloc(1, sizeof *p);
        if (!p) return NULL;
        p->used = 1;
        snprintf(p->id, sizeof p->id, "%s", id);
        p->dbg = BAD_SOCK;
        p->last_rx = now_ms();
        g_peers[i] = p;
        return p;
    }
    return NULL;
}

static void peer_free(Peer *p) {
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i] == p) g_peers[i] = NULL;
    if (p->dbg != BAD_SOCK) sock_close(p->dbg);
    buf_free(&p->dbg_in);
    buf_free(&p->dbg_out);
    free(p);
}

static int dbg_flush(Peer *p) {
    while (p->dbg_out.len) {
        const int n = send(p->dbg, (const char *)p->dbg_out.p, (int)p->dbg_out.len, SEND_FLAGS);
        if (n > 0) { buf_drop(&p->dbg_out, (size_t)n); continue; }
        if (n < 0 && err_wouldblock(sock_err())) return 1;
        return 0;
    }
    return 1;
}

static int tp_send(Peer *p, uint8_t channel, int reliable, const void *data, size_t len) {
    if (len > P2P_MAX) return 0;
    if (p->dbg != BAD_SOCK) {
        uint8_t head[3];
        put16(head, (uint16_t)(len + 1));
        head[2] = channel;
        if (!buf_add(&p->dbg_out, head, 3) || !buf_add(&p->dbg_out, data, len)) return 0;
        return dbg_flush(p);
    }
    if (!g_eos_ready || !p->puid) return 0;
    EOS_P2P_SendPacketOptions o;
    memset(&o, 0, sizeof o);
    o.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
    o.LocalUserId = g_local;
    o.RemoteUserId = p->puid;
    o.SocketId = p->is_peer ? &p->sock : &g_socket;
    o.Channel = channel;
    o.DataLengthBytes = (uint32_t)len;
    o.Data = data;
    o.bAllowDelayedDelivery = EOS_TRUE;
    o.Reliability = reliable ? EOS_PR_ReliableOrdered : EOS_PR_UnreliableUnordered;
    o.bDisableAutoAcceptConnection = EOS_FALSE;
    const EOS_EResult r = p_EOS_P2P_SendPacket(g_p2p, &o);
    if (r != EOS_Success) VLOG("SendPacket to %s: %s", p->id, eos_text(r));
    return r == EOS_Success;
}

/* Bytes waiting to go out: TCP is read only while this stays small. */
static size_t tp_queued(void) {
    size_t total = 0;
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i] && g_peers[i]->dbg != BAD_SOCK) total += g_peers[i]->dbg_out.len;
    if (g_eos_ready) {
        EOS_P2P_GetPacketQueueInfoOptions o = {EOS_P2P_GETPACKETQUEUEINFO_API_LATEST};
        EOS_P2P_PacketQueueInfo q;
        memset(&q, 0, sizeof q);
        if (p_EOS_P2P_GetPacketQueueInfo(g_p2p, &o, &q) == EOS_Success) total += (size_t)q.OutgoingPacketQueueCurrentSizeBytes;
    }
    return total;
}

static void on_packet(Peer *p, uint8_t channel, const uint8_t *data, size_t len);
static void on_peer_closed(Peer *p, const char *why);

/* ------------------------------------------------------------------ TCP streams */

typedef struct {
    int used;
    Peer *peer;
    uint32_t sid;
    uint16_t port;
    sock_t s;
    int connecting;              /* gateway: connect() to the server in progress */
    int rd_closed;               /* our socket ended: the other end was told */
    int wr_closed;               /* the other end ended: shut ours once out is sent */
    int wr_shut;
    Buf out;                     /* to write to our socket */
} Stream;

static Stream g_streams[MAX_STREAMS];
static uint32_t g_next_sid = 1;

static Stream *stream_find(Peer *p, uint32_t sid) {
    for (int i = 0; i < MAX_STREAMS; i++)
        if (g_streams[i].used && g_streams[i].peer == p && g_streams[i].sid == sid) return &g_streams[i];
    return NULL;
}

static Stream *stream_new(Peer *p, uint32_t sid, uint16_t port, sock_t s) {
    if (p->streams >= MAX_STREAMS_PER_PEER) return NULL;
    for (int i = 0; i < MAX_STREAMS; i++) {
        Stream *st = &g_streams[i];
        if (st->used) continue;
        memset(st, 0, sizeof *st);
        st->used = 1;
        st->peer = p;
        st->sid = sid;
        st->port = port;
        st->s = s;
        p->streams++;
        return st;
    }
    return NULL;
}

static void stream_free(Stream *st) {
    if (!st->used) return;
    if (st->s != BAD_SOCK) sock_close(st->s);
    buf_free(&st->out);
    if (st->peer && st->peer->streams) st->peer->streams--;
    st->used = 0;
}

static void send_close(Peer *p, uint32_t sid, uint8_t how) {
    uint8_t m[6] = {'C'};
    put32(m + 1, sid);
    m[5] = how;
    tp_send(p, CH_STREAM, 1, m, sizeof m);
}

static void stream_reset(Stream *st) {
    if (st->peer) send_close(st->peer, st->sid, 1);
    stream_free(st);
}

static void streams_of_peer_free(Peer *p) {
    for (int i = 0; i < MAX_STREAMS; i++)
        if (g_streams[i].used && g_streams[i].peer == p) stream_free(&g_streams[i]);
}

/* Writes what is pending; frees the stream once both directions are over. */
static void stream_flush(Stream *st) {
    while (st->out.len && !st->connecting) {
        const int n = send(st->s, (const char *)st->out.p, (int)st->out.len, SEND_FLAGS);
        if (n > 0) { buf_drop(&st->out, (size_t)n); continue; }
        if (n < 0 && err_wouldblock(sock_err())) return;
        stream_reset(st);
        return;
    }
    if (st->wr_closed && !st->out.len && !st->wr_shut && !st->connecting) {
        st->wr_shut = 1;
#ifdef _WIN32
        shutdown(st->s, SD_SEND);
#else
        shutdown(st->s, SHUT_WR);
#endif
    }
    if (st->rd_closed && st->wr_shut) stream_free(st);
}

static void stream_on_data(Peer *p, const uint8_t *m, size_t len) {
    if (len < 5) return;
    Stream *st = stream_find(p, get32(m + 1));
    if (!st) return;
    if (st->out.len + (len - 5) > STREAM_BUFFER_LIMIT || !buf_add(&st->out, m + 5, len - 5)) {
        stream_reset(st);
        return;
    }
    stream_flush(st);
}

static void stream_on_close(Peer *p, const uint8_t *m, size_t len) {
    if (len < 6) return;
    Stream *st = stream_find(p, get32(m + 1));
    if (!st) return;
    if (m[5]) { stream_free(st); return; }
    st->wr_closed = 1;
    stream_flush(st);
}

/* Our socket has something to read: it goes to the other end. */
static void stream_read(Stream *st) {
    uint8_t m[P2P_MAX];
    m[0] = 'D';
    put32(m + 1, st->sid);
    for (int rounds = 0; rounds < 16 && st->used && !st->rd_closed; rounds++) {
        if (tp_queued() > OUT_QUEUE_LIMIT) return;
        const int n = recv(st->s, (char *)m + 5, STREAM_CHUNK, 0);
        if (n > 0) {
            if (!tp_send(st->peer, CH_STREAM, 1, m, (size_t)n + 5)) { stream_reset(st); return; }
            continue;
        }
        if (n < 0 && err_wouldblock(sock_err())) return;
        if (n < 0) { stream_reset(st); return; }
        st->rd_closed = 1;
        send_close(st->peer, st->sid, 0);
        stream_flush(st);
        return;
    }
}

/* ------------------------------------------------------------------ UDP datagrams over P2P */

static void udp_send(Peer *p, uint8_t channel, uint16_t port, const uint8_t *data, size_t len) {
    const size_t count = (len + UDP_FRAG - 1) / UDP_FRAG;
    if (!len || count > UDP_MAX_FRAGS) return;
    const uint16_t id = ++p->udp_id;
    uint8_t m[P2P_MAX];
    for (size_t i = 0; i < count; i++) {
        const size_t off = i * UDP_FRAG, n = len - off > UDP_FRAG ? UDP_FRAG : len - off;
        m[0] = 'U';
        put16(m + 1, port);
        put16(m + 3, id);
        m[5] = (uint8_t)i;
        m[6] = (uint8_t)count;
        memcpy(m + UDP_HEAD, data + off, n);
        tp_send(p, channel, 0, m, n + UDP_HEAD);
    }
}

/* A fragment: 1 with the whole datagram in out when it completes one. */
static int udp_take(Peer *p, const uint8_t *m, size_t len, uint16_t *port, uint8_t *out, size_t *out_len) {
    if (len < UDP_HEAD) return 0;
    const uint16_t prt = get16(m + 1), id = get16(m + 3);
    const uint8_t index = m[5], count = m[6];
    const size_t n = len - UDP_HEAD;
    if (!count || count > UDP_MAX_FRAGS || index >= count || n > UDP_FRAG) return 0;
    if (count == 1) {
        memcpy(out, m + UDP_HEAD, n);
        *out_len = n;
        *port = prt;
        return 1;
    }
    Reassembly *r = &p->frag;
    if (r->id != id || r->port != prt || r->count != count) {
        memset(r, 0, sizeof *r);
        r->id = id;
        r->port = prt;
        r->count = count;
    }
    if (r->got & (1u << index)) return 0;
    r->got |= (uint8_t)(1u << index);
    r->len[index] = (uint16_t)n;
    memcpy(r->data[index], m + UDP_HEAD, n);
    if (r->got != (uint8_t)((1u << count) - 1)) return 0;
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        memcpy(out + total, r->data[i], r->len[i]);
        total += r->len[i];
    }
    *out_len = total;
    *port = prt;
    r->got = 0;
    r->id = 0;
    return 1;
}

/* ------------------------------------------------------------------ peers (races) */

static sock_t g_udp;                     /* defined with the client's below */
static struct sockaddr_in g_game_addr;   /* the game socket that last spoke to this bridge */
static int g_game_known;
static int g_peers_announced;

/* The pair's socket name: the same on both sides, and only theirs. */
static void peer_socket(const char *a, const char *b, EOS_P2P_SocketId *out) {
    const char *lo = strcmp(a, b) < 0 ? a : b, *hi = lo == a ? b : a;
    uint64_t h = 0xcbf29ce484222325ull;
    const char *parts[3] = {"ryunet-p2p-1|", lo, hi};
    for (int i = 0; i < 3; i++)
        for (const char *c = parts[i]; *c; c++) h = (h ^ (uint8_t)*c) * 0x100000001b3ull;
    memset(out, 0, sizeof *out);
    out->ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
    snprintf(out->SocketName, sizeof out->SocketName, "RYP%016llx", (unsigned long long)h);
}

static int peer_id_ok(const char *id, size_t len) {
    if (!len || len >= 40) return 0;
    for (size_t i = 0; i < len; i++) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return 0;
    }
    return 1;
}

/* The player with this id, made (and allowed) when the game names it. */
static Peer *peer_named(const char *id) {
    if (!g_eos_ready || !strcmp(id, g_local_id)) return NULL;
    Peer *p = peer_find(id);
    if (p && !p->is_peer) return NULL; /* the gateway is not a race peer */
    if (!p) {
        p = peer_new(id);
        if (!p) return NULL;
        p->is_peer = 1;
        p->puid = p_EOS_ProductUserId_FromString(id);
        peer_socket(g_local_id, id, &p->sock);
        VLOG("peer %s on %s", id, p->sock.SocketName);
    }
    p->allowed = 1;
    p->last_rx = now_ms(); /* named now: not idle */
    return p;
}

static void to_game(const uint8_t *m, size_t n) {
    if (!g_game_known) return;
    sendto(g_udp, (const char *)m, (int)n, 0, (const struct sockaddr *)&g_game_addr, sizeof g_game_addr);
}

/* "RYUEP1" kind len id [payload]: the id's length (0: not one). */
static size_t peer_msg_id(const uint8_t *m, size_t n, char *id) {
    if (n < PEER_MAGIC_LEN + 2) return 0;
    const size_t len = m[PEER_MAGIC_LEN + 1];
    if (n < PEER_MAGIC_LEN + 2 + len || !peer_id_ok((const char *)m + PEER_MAGIC_LEN + 2, len)) return 0;
    memcpy(id, m + PEER_MAGIC_LEN + 2, len);
    id[len] = 0;
    return len;
}

static size_t peer_msg(uint8_t *out, size_t cap, char kind, const char *id, const uint8_t *data, size_t len) {
    const size_t idn = strlen(id);
    if (PEER_MAGIC_LEN + 2 + idn + len > cap) return 0;
    memcpy(out, PEER_MAGIC, PEER_MAGIC_LEN);
    out[PEER_MAGIC_LEN] = (uint8_t)kind;
    out[PEER_MAGIC_LEN + 1] = (uint8_t)idn;
    memcpy(out + PEER_MAGIC_LEN + 2, id, idn);
    if (len) memcpy(out + PEER_MAGIC_LEN + 2 + idn, data, len);
    return PEER_MAGIC_LEN + 2 + idn + len;
}

/* A message of the game's (Q, K, S) on the UDP port. */
static void peer_from_game(const uint8_t *m, size_t n, const struct sockaddr_in *from) {
    g_game_addr = *from;
    g_game_known = 1;
    if (!g_eos_ready) return;
    const char kind = (char)m[PEER_MAGIC_LEN];
    if (kind == 'Q') {
        uint8_t out[64];
        const size_t len = peer_msg(out, sizeof out, 'I', g_local_id, NULL, 0);
        if (len) to_game(out, len);
        return;
    }
    char id[48];
    const size_t idn = peer_msg_id(m, n, id);
    if (!idn) return;
    Peer *p = peer_named(id);
    if (!p || kind != 'S') return;
    const size_t at = PEER_MAGIC_LEN + 2 + idn;
    udp_send(p, CH_PEER, 0, m + at, n - at);
}

/* A race peer's datagram through EOS: to the game, with whose it is. */
static void peer_packet(Peer *p, uint8_t channel, const uint8_t *m, size_t len) {
    if (channel != CH_PEER || !len || m[0] != 'U') return;
    p->last_rx = now_ms();
    uint8_t data[UDP_MAX_FRAGS * UDP_FRAG];
    size_t n = 0;
    uint16_t port = 0;
    if (!udp_take(p, m, len, &port, data, &n)) return;
    uint8_t out[UDP_MAX_FRAGS * UDP_FRAG + 64];
    const size_t total = peer_msg(out, sizeof out, 'R', p->id, data, n);
    if (total) to_game(out, total);
}

static void peers_tick(uint64_t now) {
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = g_peers[i];
        if (!p || !p->is_peer || ago(now, p->last_rx) < RACE_PEER_IDLE_MS) continue;
        /* Out of the table first: closing notifies at once (cb_closed), which must not find it. */
        const EOS_ProductUserId puid = p->puid;
        const EOS_P2P_SocketId sock = p->sock;
        peer_free(p);
        if (g_eos_ready && puid) {
            EOS_P2P_CloseConnectionOptions o = {EOS_P2P_CLOSECONNECTION_API_LATEST, g_local, puid, &sock};
            p_EOS_P2P_CloseConnection(g_p2p, &o);
        }
    }
}

/* ------------------------------------------------------------------ client */

typedef enum { C_LOGIN, C_HELLO, C_READY } ClientState;

static ClientState g_cstate = C_LOGIN;
static Peer *g_gw;                       /* the gateway */
static sock_t g_udp = BAD_SOCK;          /* 127.0.0.1:base, the game's datagrams to the server */
static sock_t g_listen[MAX_PORTS];
static uint16_t g_local_ports[MAX_PORTS];

static uint64_t g_hello_at, g_ping_at, g_last_pong;
static uint32_t g_server_ip_be, g_vip_be;
static uint32_t g_rtt;
static char g_map[512];

static int client_bind_udp_only(void) {
    for (int attempt = 0; attempt < 20; attempt++) {
        const uint16_t base = (uint16_t)(S.base_port + attempt * 20);
        g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const struct sockaddr_in a = addr4(htonl(INADDR_LOOPBACK), base);
        if (g_udp != BAD_SOCK && bind(g_udp, (const struct sockaddr *)&a, sizeof a) == 0) {
            set_nonblock(g_udp);
            S.base_port_bound = base;
            return 1;
        }
        if (g_udp != BAD_SOCK) sock_close(g_udp);
        g_udp = BAD_SOCK;
    }
    return 0;
}

static int client_bind_ports(void) {
    for (int attempt = 0; attempt < 20; attempt++) {
        const uint16_t base = (uint16_t)(S.base_port + attempt * 20);
        int ok = 1;
        g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        struct sockaddr_in a = addr4(htonl(INADDR_LOOPBACK), base);
        if (g_udp == BAD_SOCK || bind(g_udp, (struct sockaddr *)&a, sizeof a) != 0) ok = 0;
        for (int i = 0; ok && i < S.port_count; i++) {
            g_listen[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            a = addr4(htonl(INADDR_LOOPBACK), (uint16_t)(base + 1 + i));
            if (g_listen[i] == BAD_SOCK || bind(g_listen[i], (struct sockaddr *)&a, sizeof a) != 0 ||
                listen(g_listen[i], 16) != 0) {
                if (g_listen[i] != BAD_SOCK) sock_close(g_listen[i]);
                g_listen[i] = BAD_SOCK;
                for (int j = 0; j < i; j++) { sock_close(g_listen[j]); g_listen[j] = BAD_SOCK; }
                ok = 0;
                break;
            }
            g_local_ports[i] = (uint16_t)(base + 1 + i);
        }
        if (!ok) {
            if (g_udp != BAD_SOCK) sock_close(g_udp);
            g_udp = BAD_SOCK;
            continue;
        }
        set_nonblock(g_udp);
        for (int i = 0; i < S.port_count; i++) set_nonblock(g_listen[i]);
        S.base_port_bound = base;

        int n = snprintf(g_map, sizeof g_map, "%u", (unsigned)base);
        for (int i = 0; i < S.port_count && n < (int)sizeof g_map - 16; i++)
            n += snprintf(g_map + n, sizeof g_map - (size_t)n, ",%u:%u", S.ports[i], g_local_ports[i]);
        return 1;
    }
    return 0;
}

/* --- the direct check: does this network reach the server by itself? --- */

static int wait_socket(sock_t s, int write, int ms) {
    struct pollfd pf;
    memset(&pf, 0, sizeof pf);
    pf.fd = s;
    pf.events = write ? POLLOUT : POLLIN;
    if (poll_fn(&pf, 1, ms) <= 0) return 0;
    if (write) {
        int err = 0;
#ifdef _WIN32
        int len = sizeof err;
#else
        socklen_t len = sizeof err;
#endif
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
        return err == 0 && (pf.revents & POLLOUT);
    }
    return (pf.revents & POLLIN) != 0;
}

static sock_t connect_timeout(uint32_t ip_be, uint16_t port, int ms) {
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BAD_SOCK) return BAD_SOCK;
    set_nonblock(s);
    struct sockaddr_in a = addr4(ip_be, port);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0 && !err_inprogress(sock_err())) { sock_close(s); return BAD_SOCK; }
    if (!wait_socket(s, 1, ms)) { sock_close(s); return BAD_SOCK; }
    return s;
}

static int check_http(uint32_t ip_be) {
    sock_t s = connect_timeout(ip_be, S.http_port, 4000);
    if (s == BAD_SOCK) { LOG("direct: HTTP %u does not connect", S.http_port); return 0; }
    char req[512];
    const int n = snprintf(req, sizeof req, "GET /ryunet/theme HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n"
                                            "User-Agent: ryunet-bridge\r\n\r\n", S.server, S.http_port);
    send(s, req, n, SEND_FLAGS);
    char reply[4096];
    int got = 0;
    const uint64_t until = now_ms() + 5000;
    while (got < (int)sizeof reply - 1 && now_ms() < until) {
        if (!wait_socket(s, 0, 500)) continue;
        const int r = recv(s, reply + got, (int)sizeof reply - 1 - got, 0);
        if (r <= 0) break;
        got += r;
        reply[got] = 0;
        if (strstr(reply, "\r\n\r\n")) break;
    }
    sock_close(s);
    reply[got] = 0;
    const int ok = strncmp(reply, "HTTP/1.", 7) == 0 && strstr(reply, "json") != NULL;
    if (!ok) LOG("direct: HTTP %u answered by something that is not the server", S.http_port);
    return ok;
}

static int check_lobby(uint32_t ip_be) {
    for (int i = 0; i < S.port_count; i++) {
        if (S.ports[i] < 3074 || S.ports[i] > 3080) continue;
        sock_t s = connect_timeout(ip_be, S.ports[i], 3500);
        if (s == BAD_SOCK) { LOG("direct: lobby TCP %u does not connect", S.ports[i]); return 0; }
        sock_close(s);
        return 1;
    }
    return 1;
}

static int check_nat(uint32_t ip_be) {
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD_SOCK) return 0;
    const struct sockaddr_in to = addr4(ip_be, S.nat_port);
    const uint8_t probe[3] = {0x1e, 0x03, 0x00};
    int ok = 0;
    for (int i = 0; i < 4 && !ok; i++) {
        sendto(s, (const char *)probe, 3, 0, (const struct sockaddr *)&to, sizeof to);
        if (!wait_socket(s, 0, 800)) continue;
        uint8_t reply[64];
        const int n = recv(s, (char *)reply, sizeof reply, 0);
        ok = n >= 9 && reply[0] == 31;
    }
    sock_close(s);
    if (!ok) LOG("direct: NAT UDP %u does not answer", S.nat_port);
    return ok;
}

static int direct_works(void) {
    uint32_t ip;
    if (!resolve4(S.server, &ip)) { LOG("direct: %s does not resolve", S.server); return 0; }
    char t[16];
    LOG("direct: checking %s (%s)", S.server, ip_text(ip, t));
    return check_http(ip) && check_lobby(ip) && check_nat(ip);
}

/* --- the gateway connection --- */

static void client_lost(const char *why) {
    if (g_cstate == C_READY) status("LOST %s", why);
    LOG("gateway lost: %s", why);
    for (int i = 0; i < MAX_STREAMS; i++)
        if (g_streams[i].used) stream_free(&g_streams[i]);
    g_cstate = g_eos_ready || !g_eos_on ? C_HELLO : C_LOGIN;
    g_hello_at = 0;
}

static void client_hello(uint64_t now) {
    if (now < g_hello_at) return;
    g_hello_at = now + HELLO_MS;
    const uint8_t m[3] = {'H', BRIDGE_VERSION, 0};
    tp_send(g_gw, CH_CONTROL, 1, m, sizeof m);
}

static void client_packet(Peer *p, uint8_t channel, const uint8_t *m, size_t len) {
    (void)channel;
    if (p != g_gw || !len) return;
    p->last_rx = now_ms();
    switch (m[0]) {
    case 'W':
        if (len < 10) return;
        g_server_ip_be = 0;
        memcpy(&g_server_ip_be, m + 2, 4);
        memcpy(&g_vip_be, m + 6, 4);
        if (g_cstate != C_READY) {
            char a[16], b[16];
            g_cstate = C_READY;
            g_last_pong = now_ms();
            status("READY ip=%s vip=%s map=%s", ip_text(g_server_ip_be, a), ip_text(g_vip_be, b), g_map);
        }
        break;
    case 'R':
        status("FAILED refused %.*s", (int)(len - 1), (const char *)m + 1);
        exit(4);
    case 'P': {
        uint8_t q[5];
        memcpy(q, m, len >= 5 ? 5 : len);
        q[0] = 'Q';
        tp_send(p, CH_CONTROL, 1, q, 5);
        break;
    }
    case 'Q':
        if (len >= 5) {
            const uint32_t rtt = (uint32_t)(now_ms() & 0xffffffffu) - get32(m + 1);
            g_rtt = g_rtt ? (g_rtt * 3 + rtt) / 4 : rtt;
            g_last_pong = now_ms();
        }
        break;
    case 'U': {
        uint8_t data[UDP_MAX_FRAGS * UDP_FRAG];
        size_t n = 0;
        uint16_t port = 0;
        if (udp_take(p, m, len, &port, data, &n)) {
            const struct sockaddr_in to = addr4(htonl(INADDR_LOOPBACK), port);
            sendto(g_udp, (const char *)data, (int)n, 0, (const struct sockaddr *)&to, sizeof to);
        }
        break;
    }
    case 'D': stream_on_data(p, m, len); break;
    case 'C': stream_on_close(p, m, len); break;
    default: break;
    }
}

static void client_accept(int index) {
    for (int k = 0; k < 8; k++) {
        sock_t c = accept(g_listen[index], NULL, NULL);
        if (c == BAD_SOCK) return;
        Stream *st = g_cstate == C_READY ? stream_new(g_gw, g_next_sid, S.ports[index], c) : NULL;
        if (!st) { sock_close(c); continue; }
        g_next_sid++;
        set_nonblock(c);
        set_nodelay(c);
        uint8_t m[7] = {'O'};
        put32(m + 1, st->sid);
        put16(m + 5, st->port);
        if (!tp_send(g_gw, CH_STREAM, 1, m, sizeof m)) stream_free(st);
        VLOG("stream %u: local -> server port %u", st->sid, st->port);
    }
}

static void client_udp_read(void) {
    uint8_t data[2200];
    for (int k = 0; k < 64; k++) {
        struct sockaddr_in from;
#ifdef _WIN32
        int fl = sizeof from;
#else
        socklen_t fl = sizeof from;
#endif
        const int n = recvfrom(g_udp, (char *)data, sizeof data, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) return;
        if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
        if (n > PEER_MAGIC_LEN && !memcmp(data, PEER_MAGIC, PEER_MAGIC_LEN)) {
            peer_from_game(data, (size_t)n, &from);
            continue;
        }
        if (g_cstate != C_READY || !g_gw) continue; /* peers only, or the gateway not there yet */
        udp_send(g_gw, CH_UDP, ntohs(from.sin_port), data, (size_t)n);
    }
}

static void client_tick(uint64_t now) {
    peers_tick(now);
    if (!g_gw) return; /* peers only: the server is reached directly */
    if (g_cstate == C_HELLO) client_hello(now);
    if (g_cstate == C_READY) {
        if (now >= g_ping_at) {
            g_ping_at = now + PING_MS;
            uint8_t m[5] = {'P'};
            put32(m + 1, (uint32_t)(now & 0xffffffffu));
            tp_send(g_gw, CH_CONTROL, 1, m, sizeof m);
            static uint64_t logged;
            if (g_rtt && ago(now, logged) > 60000) {
                logged = now;
                LOG("gateway round trip %u ms", g_rtt);
            }
        }
        if (ago(now, g_last_pong) > LOST_MS) client_lost("no answer");
    }
}

/* ------------------------------------------------------------------ gateway */

static sock_t g_srv_udp = BAD_SOCK;   /* to the server's NAT port, RYUGW1 */
static uint32_t g_target_be, g_public_be;
static char g_ips_path[1200];
typedef struct { char id[80]; uint32_t vip; } VipEntry;
static VipEntry *g_vips;
static size_t g_vip_count, g_vip_cap;
static uint32_t g_vip_next = 0xC6120002u; /* 198.18.0.2 */
static uint64_t g_stat_at;
static unsigned long long g_stat_udp_in, g_stat_udp_out, g_stat_tcp;

static void vips_load(void) {
    FILE *f = fopen(g_ips_path, "r");
    if (!f) return;
    char id[80], ip[32];
    while (fscanf(f, "%79s %31s", id, ip) == 2) {
        struct in_addr a;
        if (inet_pton(AF_INET, ip, &a) != 1) continue;
        if (g_vip_count == g_vip_cap) {
            g_vip_cap = g_vip_cap ? g_vip_cap * 2 : 256;
            g_vips = realloc(g_vips, g_vip_cap * sizeof *g_vips);
        }
        snprintf(g_vips[g_vip_count].id, sizeof g_vips[0].id, "%s", id);
        g_vips[g_vip_count].vip = ntohl(a.s_addr);
        if (g_vips[g_vip_count].vip >= g_vip_next) g_vip_next = g_vips[g_vip_count].vip + 1;
        g_vip_count++;
    }
    fclose(f);
    LOG("%zu players' addresses known (%s)", g_vip_count, g_ips_path);
}

/* The player's virtual address (host order): the same every time it comes back. */
static uint32_t vip_for(const char *id) {
    for (size_t i = 0; i < g_vip_count; i++)
        if (strcmp(g_vips[i].id, id) == 0) return g_vips[i].vip;
    while ((g_vip_next & 0xff) == 0 || (g_vip_next & 0xff) == 0xff) g_vip_next++;
    if (g_vip_next >= 0xC6140000u) return 0; /* past 198.19.255.255 */
    if (g_vip_count == g_vip_cap) {
        g_vip_cap = g_vip_cap ? g_vip_cap * 2 : 256;
        VipEntry *v = realloc(g_vips, g_vip_cap * sizeof *g_vips);
        if (!v) return 0;
        g_vips = v;
    }
    const uint32_t vip = g_vip_next++;
    snprintf(g_vips[g_vip_count].id, sizeof g_vips[0].id, "%s", id);
    g_vips[g_vip_count++].vip = vip;
    FILE *f = fopen(g_ips_path, "a");
    if (f) {
        char t[16];
        fprintf(f, "%s %s\n", id, ip_text(htonl(vip), t));
        fclose(f);
    }
    return vip;
}

static Peer *peer_by_vip(uint32_t vip) {
    for (int i = 0; i < MAX_PEERS; i++)
        if (g_peers[i] && g_peers[i]->welcomed && g_peers[i]->vip == vip) return g_peers[i];
    return NULL;
}

static void gateway_open(Peer *p, const uint8_t *m, size_t len) {
    if (len < 7) return;
    const uint32_t sid = get32(m + 1);
    const uint16_t port = get16(m + 5);
    if (!p->welcomed || !port_allowed(port) || stream_find(p, sid)) { send_close(p, sid, 1); return; }
    sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Stream *st = s != BAD_SOCK ? stream_new(p, sid, port, s) : NULL;
    if (!st) {
        if (s != BAD_SOCK) sock_close(s);
        send_close(p, sid, 1);
        return;
    }
    set_nonblock(s);
    set_nodelay(s);
    char vip[16], dst[16], line[128];
    /* The server takes the player's address from this line (eos_gateway.go trusts it only from
     * this machine); the source port only tells the connections apart. */
    const int n = snprintf(line, sizeof line, "PROXY TCP4 %s %s %u %u\r\n", ip_text(htonl(p->vip), vip),
                           ip_text(g_public_be, dst), (unsigned)(20000 + sid % 40000), (unsigned)port);
    buf_add(&st->out, line, (size_t)n);
    const struct sockaddr_in a = addr4(g_target_be, port);
    if (connect(s, (const struct sockaddr *)&a, sizeof a) != 0) {
        if (!err_inprogress(sock_err())) { stream_reset(st); return; }
        st->connecting = 1;
    }
    g_stat_tcp++;
    VLOG("%s: stream %u to port %u", p->id, sid, port);
    stream_flush(st);
}

static void gateway_packet(Peer *p, uint8_t channel, const uint8_t *m, size_t len) {
    (void)channel;
    if (!len) return;
    p->last_rx = now_ms();
    switch (m[0]) {
    case 'H': {
        if (len < 2 || m[1] != BRIDGE_VERSION) {
            const char reason[] = "Rversion";
            tp_send(p, CH_CONTROL, 1, reason, sizeof reason - 1);
            return;
        }
        if (!p->welcomed) {
            p->vip = vip_for(p->id);
            if (!p->vip) return;
            p->welcomed = 1;
            char t[16];
            LOG("player %s is %s", p->id, ip_text(htonl(p->vip), t));
        }
        uint8_t w[10] = {'W', BRIDGE_VERSION};
        memcpy(w + 2, &g_public_be, 4);
        const uint32_t vip_be = htonl(p->vip);
        memcpy(w + 6, &vip_be, 4);
        tp_send(p, CH_CONTROL, 1, w, sizeof w);
        break;
    }
    case 'P': {
        uint8_t q[5] = {'Q'};
        if (len >= 5) memcpy(q + 1, m + 1, 4);
        tp_send(p, CH_CONTROL, 1, q, sizeof q);
        break;
    }
    case 'U': {
        if (!p->welcomed) return;
        const uint64_t now = now_ms();
        if (ago(now, p->udp_window) >= 1000) { p->udp_window = now; p->udp_count = 0; }
        if (++p->udp_count > UDP_PPS_LIMIT) return;
        uint8_t data[12 + UDP_MAX_FRAGS * UDP_FRAG];
        size_t n = 0;
        uint16_t port = 0;
        if (!udp_take(p, m, len, &port, data + 12, &n)) return;
        memcpy(data, "RYUGW1", 6);
        const uint32_t vip_be = htonl(p->vip);
        memcpy(data + 6, &vip_be, 4);
        put16(data + 10, port);
        const struct sockaddr_in to = addr4(g_target_be, S.nat_port);
        sendto(g_srv_udp, (const char *)data, (int)(n + 12), 0, (const struct sockaddr *)&to, sizeof to);
        g_stat_udp_in++;
        break;
    }
    case 'O': gateway_open(p, m, len); break;
    case 'D': stream_on_data(p, m, len); break;
    case 'C': stream_on_close(p, m, len); break;
    default: break;
    }
}

static void gateway_udp_read(void) {
    uint8_t data[2400];
    for (int k = 0; k < 256; k++) {
        struct sockaddr_in from;
#ifdef _WIN32
        int fl = sizeof from;
#else
        socklen_t fl = sizeof from;
#endif
        const int n = recvfrom(g_srv_udp, (char *)data, sizeof data, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) return;
        if (n < 12 || memcmp(data, "RYUGW1", 6) != 0) continue;
        uint32_t vip_be;
        memcpy(&vip_be, data + 6, 4);
        Peer *p = peer_by_vip(ntohl(vip_be));
        if (!p) continue;
        udp_send(p, CH_UDP, get16(data + 10), data + 12, (size_t)(n - 12));
        g_stat_udp_out++;
    }
}

static void gateway_tick(uint64_t now) {
    for (int i = 0; i < MAX_PEERS; i++) {
        Peer *p = g_peers[i];
        if (!p || p->dbg != BAD_SOCK) continue;
        if (ago(now, p->last_rx) > PEER_IDLE_MS) on_peer_closed(p, "idle");
    }
    if (now >= g_stat_at) {
        g_stat_at = now + 60000;
        int peers = 0, streams = 0;
        for (int i = 0; i < MAX_PEERS; i++) peers += g_peers[i] && g_peers[i]->welcomed;
        for (int i = 0; i < MAX_STREAMS; i++) streams += g_streams[i].used;
        LOG("players %d, streams %d, datagrams in %llu out %llu, connections %llu", peers, streams,
            g_stat_udp_in, g_stat_udp_out, g_stat_tcp);
    }
}

/* ------------------------------------------------------------------ dispatch */

static void on_packet(Peer *p, uint8_t channel, const uint8_t *data, size_t len) {
    if (S.gateway) gateway_packet(p, channel, data, len);
    else if (p->is_peer) peer_packet(p, channel, data, len);
    else client_packet(p, channel, data, len);
}

static void on_peer_closed(Peer *p, const char *why) {
    if (!S.gateway) {
        if (p == g_gw) client_lost(why);
        else if (p->is_peer) VLOG("peer %s: P2P %s", p->id, why); /* it reopens with the next datagram */
        return;
    }
    LOG("player %s gone (%s)", p->id, why);
    streams_of_peer_free(p);
    /* Out of the table first: closing notifies at once (cb_closed), which must not find it. */
    const EOS_ProductUserId puid = p->puid;
    peer_free(p);
    if (puid && g_eos_ready) {
        EOS_P2P_CloseConnectionOptions o = {EOS_P2P_CLOSECONNECTION_API_LATEST, g_local, puid, &g_socket};
        p_EOS_P2P_CloseConnection(g_p2p, &o);
    }
}

/* ------------------------------------------------------------------ EOS login and P2P */

static void eos_login(void);

static void EOS_CALL cb_established(const EOS_P2P_OnPeerConnectionEstablishedInfo *i) {
    char id[64] = "";
    int32_t n = sizeof id;
    p_EOS_ProductUserId_ToString(i->RemoteUserId, id, &n);
    LOG("P2P with %s: %s%s", id, i->NetworkType == EOS_NCT_RelayedConnection ? "through Epic's relay" : "direct",
        i->ConnectionType == EOS_CET_Reconnection ? " (reconnected)" : "");
}

static int is_gateway_socket(const EOS_P2P_SocketId *s) { return s && strcmp(s->SocketName, SOCKET_NAME) == 0; }

static void EOS_CALL cb_closed(const EOS_P2P_OnRemoteConnectionClosedInfo *i) {
    char id[64] = "";
    int32_t n = sizeof id;
    p_EOS_ProductUserId_ToString(i->RemoteUserId, id, &n);
    if (i->Reason == EOS_CCR_ClosedByLocalUser) return; /* this side closed it: already handled */
    Peer *p = peer_find(id);
    if (p && p->is_peer == is_gateway_socket(i->SocketId)) return; /* another socket's */
    char why[32];
    snprintf(why, sizeof why, "closed, reason %d", (int)i->Reason);
    if (p) on_peer_closed(p, why);
}

static void EOS_CALL cb_request(const EOS_P2P_OnIncomingConnectionRequestInfo *i) {
    const EOS_P2P_SocketId *sock = &g_socket;
    if (S.gateway) {
        if (!is_gateway_socket(i->SocketId)) return;
    } else {
        /* A client accepts only race peers its game named, on their pair's socket (its gateway
         * it calls itself). */
        char id[64] = "";
        int32_t n = sizeof id;
        p_EOS_ProductUserId_ToString(i->RemoteUserId, id, &n);
        Peer *p = peer_find(id);
        if (!p || !p->is_peer || !p->allowed || !i->SocketId || strcmp(i->SocketId->SocketName, p->sock.SocketName) != 0) {
            VLOG("P2P request from %s refused (not a peer the game named)", id);
            return;
        }
        sock = &p->sock;
    }
    EOS_P2P_AcceptConnectionOptions o = {EOS_P2P_ACCEPTCONNECTION_API_LATEST, g_local, i->RemoteUserId, sock};
    const EOS_EResult r = p_EOS_P2P_AcceptConnection(g_p2p, &o);
    if (r != EOS_Success) LOG("AcceptConnection: %s", eos_text(r));
}

static void eos_logged_in(EOS_ProductUserId id) {
    g_local = id;
    int32_t n = sizeof g_local_id;
    p_EOS_ProductUserId_ToString(id, g_local_id, &n);
    if (!g_eos_ready) {
        static int once;
        if (!once) {
            once = 1;
            /* every socket: the gateway's and the race peers' (NULL) */
            EOS_P2P_AddNotifyPeerConnectionRequestOptions rq = {EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST, g_local, NULL};
            p_EOS_P2P_AddNotifyPeerConnectionRequest(g_p2p, &rq, NULL, cb_request);
            EOS_P2P_AddNotifyPeerConnectionClosedOptions cl = {EOS_P2P_ADDNOTIFYPEERCONNECTIONCLOSED_API_LATEST, g_local, NULL};
            p_EOS_P2P_AddNotifyPeerConnectionClosed(g_p2p, &cl, NULL, cb_closed);
            EOS_P2P_AddNotifyPeerConnectionEstablishedOptions es = {EOS_P2P_ADDNOTIFYPEERCONNECTIONESTABLISHED_API_LATEST, g_local, NULL};
            p_EOS_P2P_AddNotifyPeerConnectionEstablished(g_p2p, &es, NULL, cb_established);
            EOS_P2P_SetRelayControlOptions rc = {EOS_P2P_SETRELAYCONTROL_API_LATEST,
                                                 S.force_relays ? EOS_RC_ForceRelays : EOS_RC_AllowRelays};
            p_EOS_P2P_SetRelayControl(g_p2p, &rc);
            EOS_P2P_SetPacketQueueSizeOptions qs = {EOS_P2P_SETPACKETQUEUESIZE_API_LATEST, 32u << 20, 32u << 20};
            p_EOS_P2P_SetPacketQueueSize(g_p2p, &qs);
        }
    }
    g_eos_ready = 1;
    if (S.gateway) {
        printf("GATEWAY ID %s\n", g_local_id);
        fflush(stdout);
        LOG("logged in to EOS as %s: clients need gateway_id=%s", g_local_id, g_local_id);
    } else {
        LOG("logged in to EOS as %s", g_local_id);
        if (!g_peers_announced) {
            g_peers_announced = 1;
            status("PEERS port=%u id=%s", (unsigned)S.base_port_bound, g_local_id);
        }
        if (g_gw && !g_gw->puid) g_gw->puid = p_EOS_ProductUserId_FromString(g_gw->id);
        if (g_cstate == C_LOGIN) {
            g_cstate = C_HELLO;
            g_hello_at = 0;
        }
    }
}

static void eos_fail(const char *what, EOS_EResult r) {
    LOG("EOS %s: %s", what, eos_text(r));
    if (g_peers_only) exit(0); /* races without EOS: the server itself works */
    if (!S.gateway) {
        status("FAILED eos_%s %s", what, eos_text(r));
        exit(3);
    }
    g_eos_failed = 1;
    g_eos_retry_at = now_ms() + 30000;
}

static void EOS_CALL cb_created_user(const EOS_Connect_CreateUserCallbackInfo *i) {
    if (i->ResultCode != EOS_Success) { eos_fail("create_user", i->ResultCode); return; }
    eos_logged_in(i->LocalUserId);
}

static void EOS_CALL cb_login(const EOS_Connect_LoginCallbackInfo *i) {
    if (i->ResultCode == EOS_Success) { eos_logged_in(i->LocalUserId); return; }
    if (i->ResultCode == EOS_InvalidUser && i->ContinuanceToken) {
        EOS_Connect_CreateUserOptions o = {EOS_CONNECT_CREATEUSER_API_LATEST, i->ContinuanceToken};
        p_EOS_Connect_CreateUser(g_connect, &o, NULL, cb_created_user);
        return;
    }
    eos_fail("login", i->ResultCode);
}

static void eos_login(void) {
    EOS_Connect_Credentials c;
    memset(&c, 0, sizeof c);
    c.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
    c.Token = NULL;
    c.Type = EOS_ECT_DEVICEID_ACCESS_TOKEN;
    EOS_Connect_UserLoginInfo u;
    memset(&u, 0, sizeof u);
    u.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
    u.DisplayName = S.gateway ? "RyuNET gateway" : "RyuNET player";
    EOS_Connect_LoginOptions o;
    memset(&o, 0, sizeof o);
    o.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
    o.Credentials = &c;
    o.UserLoginInfo = &u;
    p_EOS_Connect_Login(g_connect, &o, NULL, cb_login);
}

static void EOS_CALL cb_device(const EOS_Connect_CreateDeviceIdCallbackInfo *i) {
    if (i->ResultCode != EOS_Success && i->ResultCode != EOS_DuplicateNotAllowed) {
        eos_fail("device_id", i->ResultCode);
        return;
    }
    eos_login();
}

static void EOS_CALL cb_expiring(const EOS_Connect_AuthExpirationCallbackInfo *i) {
    (void)i;
    LOG("EOS login expiring: renewing it");
    eos_login();
}

static void eos_begin(void) {
    g_eos_failed = 0;
    static int notified;
    if (!notified) {
        notified = 1;
        EOS_Connect_AddNotifyAuthExpirationOptions ao = {EOS_CONNECT_ADDNOTIFYAUTHEXPIRATION_API_LATEST};
        p_EOS_Connect_AddNotifyAuthExpiration(g_connect, &ao, NULL, cb_expiring);
    }
    EOS_Connect_CreateDeviceIdOptions o = {EOS_CONNECT_CREATEDEVICEID_API_LATEST, "PC"};
    p_EOS_Connect_CreateDeviceId(g_connect, &o, NULL, cb_device);
}

static void eos_receive(void) {
    if (!g_eos_ready) return;
    uint8_t data[P2P_MAX];
    for (int k = 0; k < 4000; k++) {
        EOS_P2P_ReceivePacketOptions o = {EOS_P2P_RECEIVEPACKET_API_LATEST, g_local, P2P_MAX, NULL};
        EOS_ProductUserId from = NULL;
        EOS_P2P_SocketId sock;
        uint8_t channel = 0;
        uint32_t n = 0;
        memset(&sock, 0, sizeof sock);
        if (p_EOS_P2P_ReceivePacket(g_p2p, &o, &from, &sock, &channel, data, &n) != EOS_Success) return;
        char id[64] = "";
        int32_t idn = sizeof id;
        if (p_EOS_ProductUserId_ToString(from, id, &idn) != EOS_Success) continue;
        Peer *p = peer_find(id);
        const int gateway_socket = is_gateway_socket(&sock);
        if (!p && S.gateway && gateway_socket) {
            p = peer_new(id);
            if (p) p->puid = from;
        }
        if (!p || (p->is_peer && (gateway_socket || !p->allowed)) || (!p->is_peer && !gateway_socket)) continue;
        on_packet(p, channel, data, n);
    }
}

/* ------------------------------------------------------------------ debug transport (TCP) */

static sock_t g_dbg_listen = BAD_SOCK;
static unsigned g_dbg_count;

static void dbg_read(Peer *p) {
    uint8_t chunk[8192];
    for (;;) {
        const int n = recv(p->dbg, (char *)chunk, sizeof chunk, 0);
        if (n > 0) { buf_add(&p->dbg_in, chunk, (size_t)n); continue; }
        if (n < 0 && err_wouldblock(sock_err())) break;
        on_peer_closed(p, "debug connection ended");
        if (S.gateway) return;
        sock_close(p->dbg);
        p->dbg = BAD_SOCK;
        exit(5);
    }
    while (p->dbg_in.len >= 3) {
        const size_t len = get16(p->dbg_in.p);
        if (len < 1 || len > P2P_MAX + 1) { on_peer_closed(p, "debug frame"); return; }
        if (p->dbg_in.len < 2 + len) break;
        uint8_t frame[P2P_MAX + 1];
        memcpy(frame, p->dbg_in.p + 2, len);
        buf_drop(&p->dbg_in, 2 + len);
        on_packet(p, frame[0], frame + 1, len - 1);
        if (!S.gateway && !g_gw) return;
    }
}

static int dbg_connect(const char *hostport) {
    char host[128];
    uint16_t port;
    uint32_t ip;
    if (!split_hostport(hostport, host, sizeof host, &port) || !resolve4(host, &ip)) return 0;
    sock_t s = connect_timeout(ip, port, 5000);
    if (s == BAD_SOCK) return 0;
    set_nodelay(s);
    g_gw = peer_new("debug-gateway");
    g_gw->dbg = s;
    return 1;
}

static int dbg_listen(const char *hostport) {
    char host[128];
    uint16_t port;
    uint32_t ip;
    if (!split_hostport(hostport, host, sizeof host, &port) || !resolve4(host, &ip)) return 0;
    g_dbg_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int on = 1;
    setsockopt(g_dbg_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
    const struct sockaddr_in a = addr4(ip, port);
    if (bind(g_dbg_listen, (const struct sockaddr *)&a, sizeof a) != 0 || listen(g_dbg_listen, 16) != 0) return 0;
    set_nonblock(g_dbg_listen);
    LOG("debug transport on %s", hostport);
    return 1;
}

static void dbg_accept(void) {
    for (;;) {
        sock_t c = accept(g_dbg_listen, NULL, NULL);
        if (c == BAD_SOCK) return;
        set_nonblock(c);
        set_nodelay(c);
        char id[32];
        snprintf(id, sizeof id, "debug-%u", ++g_dbg_count);
        Peer *p = peer_new(id);
        if (!p) { sock_close(c); continue; }
        p->dbg = c;
    }
}

/* ------------------------------------------------------------------ main loop */

static void loop(void) {
    struct pollfd *pf = calloc(MAX_STREAMS + MAX_PEERS + MAX_PORTS + 8, sizeof *pf);
    void **who = calloc(MAX_STREAMS + MAX_PEERS + MAX_PORTS + 8, sizeof *who);
    int *kind = calloc(MAX_STREAMS + MAX_PEERS + MAX_PORTS + 8, sizeof *kind);
    enum { K_UDP, K_LISTEN, K_STREAM, K_DBG, K_DBGLISTEN, K_SRVUDP, K_STDIN };
    for (;;) {
        const uint64_t now = now_ms();
        if (g_platform) p_EOS_Platform_Tick(g_platform);
        if (g_eos_on && g_eos_failed && now >= g_eos_retry_at) eos_begin();
        eos_receive();
        if (S.gateway) gateway_tick(now);
        else client_tick(now);

        int n = 0;
        if (g_udp != BAD_SOCK) { pf[n].fd = g_udp; pf[n].events = POLLIN; kind[n] = K_UDP; n++; }
        if (g_srv_udp != BAD_SOCK) { pf[n].fd = g_srv_udp; pf[n].events = POLLIN; kind[n] = K_SRVUDP; n++; }
        if (g_dbg_listen != BAD_SOCK) { pf[n].fd = g_dbg_listen; pf[n].events = POLLIN; kind[n] = K_DBGLISTEN; n++; }
#ifndef _WIN32
        if (g_exit_on_eof) { pf[n].fd = 0; pf[n].events = POLLIN; kind[n] = K_STDIN; n++; }
#endif
        if (!S.gateway)
            for (int i = 0; i < S.port_count; i++)
                if (g_listen[i] != BAD_SOCK) { pf[n].fd = g_listen[i]; pf[n].events = POLLIN; kind[n] = K_LISTEN; who[n] = (void *)(intptr_t)i; n++; }
        for (int i = 0; i < MAX_PEERS; i++) {
            Peer *p = g_peers[i];
            if (!p || p->dbg == BAD_SOCK) continue;
            pf[n].fd = p->dbg;
            pf[n].events = (short)(POLLIN | (p->dbg_out.len ? POLLOUT : 0));
            kind[n] = K_DBG;
            who[n] = p;
            n++;
        }
        const int reading = tp_queued() <= OUT_QUEUE_LIMIT;
        int active = 0;
        for (int i = 0; i < MAX_STREAMS; i++) {
            Stream *st = &g_streams[i];
            if (!st->used) continue;
            active = 1;
            short ev = 0;
            if (!st->rd_closed && !st->connecting && reading) ev |= POLLIN;
            if (st->connecting || st->out.len) ev |= POLLOUT;
            if (!ev) continue;
            pf[n].fd = st->s;
            pf[n].events = ev;
            kind[n] = K_STREAM;
            who[n] = st;
            n++;
        }
        for (int i = 0; i < n; i++) pf[i].revents = 0;
        /* EOS delivers nothing to poll: short waits while anything is going on. */
        const int wait = (g_cstate == C_READY || active || g_eos_ready) ? 2 : 10;
        const int r = poll_fn(pf, (unsigned)n, wait);
        if (r <= 0) continue;
        for (int i = 0; i < n; i++) {
            const short ev = pf[i].revents;
            if (!ev) continue;
            switch (kind[i]) {
            case K_UDP: client_udp_read(); break;
#ifndef _WIN32
            case K_STDIN: {
                char b[64];
                if (read(0, b, sizeof b) <= 0) exit(0);
                break;
            }
#endif
            case K_SRVUDP: gateway_udp_read(); break;
            case K_DBGLISTEN: dbg_accept(); break;
            case K_LISTEN: client_accept((int)(intptr_t)who[i]); break;
            case K_DBG: {
                Peer *p = who[i];
                int alive = 0;
                for (int k = 0; k < MAX_PEERS; k++) alive |= g_peers[k] == p;
                if (!alive) break;
                if (ev & POLLOUT) dbg_flush(p);
                if (ev & (POLLIN | POLLHUP | POLLERR)) dbg_read(p);
                break;
            }
            case K_STREAM: {
                Stream *st = who[i];
                if (!st->used || st->s != pf[i].fd) break;
                if (st->connecting) {
                    int err = 0;
#ifdef _WIN32
                    int len = sizeof err;
#else
                    socklen_t len = sizeof err;
#endif
                    getsockopt(st->s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
                    if (err || (ev & (POLLERR | POLLHUP) && !(ev & POLLOUT))) { stream_reset(st); break; }
                    if (!(ev & POLLOUT)) break;
                    st->connecting = 0;
                }
                if (ev & POLLOUT) stream_flush(st);
                if (st->used && (ev & (POLLIN | POLLHUP | POLLERR))) stream_read(st);
                break;
            }
            default: break;
            }
        }
    }
}

static void usage(void) {
    fprintf(stderr,
            "ryunet-bridge --client --server HOST [--http-port 8080] [--mode auto|on] [--base-port 39700]\n"
            "              [--gateway-id ID] [--config eosbridge.ini] [--data DIR] [--debug-gateway HOST:PORT]\n"
            "ryunet-bridge --gateway --server-ip PUBLIC_IP [--target 127.0.0.1] [--config eosbridge.ini]\n"
            "              [--data DIR] [--debug-listen HOST:PORT] [--no-eos]\n"
            "  common: [--ports 3074,3075,...,8460,8080] [--nat-port 3074] [--exit-on-eof] [--force-relays] [-v]\n");
    exit(2);
}

int main(int argc, char **argv) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    char dir[1024];
    exe_dir(dir, sizeof dir);
    snprintf(S.config, sizeof S.config, "%s/eosbridge.ini", dir);
    S.http_port = 8080;
    S.nat_port = 3074;
    S.base_port = 39700;
    snprintf(S.target, sizeof S.target, "127.0.0.1");
    parse_ports("3074,3075,3076,3077,3078,3079,3080,8460,8080");
    int role = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (strcmp(a, name) == 0 && v && (i++, 1))
        if (strcmp(a, "--client") == 0) role = 0;
        else if (strcmp(a, "--gateway") == 0) role = 1;
        else if (strcmp(a, "-v") == 0) g_verbose = 1;
        else if (strcmp(a, "--no-eos") == 0) S.no_eos = 1;
        else if (strcmp(a, "--exit-on-eof") == 0) g_exit_on_eof = 1;
        else if (strcmp(a, "--force-relays") == 0) S.force_relays = 1;
        else if (ARG("--server")) snprintf(S.server, sizeof S.server, "%s", v);
        else if (ARG("--http-port")) S.http_port = (uint16_t)atoi(v);
        else if (ARG("--nat-port")) S.nat_port = (uint16_t)atoi(v);
        else if (ARG("--mode")) S.mode_on = strcmp(v, "on") == 0;
        else if (ARG("--base-port")) S.base_port = (uint16_t)atoi(v);
        else if (ARG("--ports")) parse_ports(v);
        else if (ARG("--gateway-id")) snprintf(S.gateway_id, sizeof S.gateway_id, "%s", v);
        else if (ARG("--config")) snprintf(S.config, sizeof S.config, "%s", v);
        else if (ARG("--data")) snprintf(S.data_dir, sizeof S.data_dir, "%s", v);
        else if (ARG("--debug-gateway")) snprintf(S.debug_gateway, sizeof S.debug_gateway, "%s", v);
        else if (ARG("--debug-listen")) snprintf(S.debug_listen, sizeof S.debug_listen, "%s", v);
        else if (ARG("--server-ip")) snprintf(S.server_ip, sizeof S.server_ip, "%s", v);
        else if (ARG("--target")) snprintf(S.target, sizeof S.target, "%s", v);
        else usage();
#undef ARG
    }
    if (role < 0) usage();
    S.gateway = role == 1;
    load_config(S.config);
    load_embedded();
    if (!S.data_dir[0]) {
#ifdef _WIN32
        const char *base = getenv("LOCALAPPDATA");
        snprintf(S.data_dir, sizeof S.data_dir, "%s\\RyuNET\\eosbridge%s", base ? base : ".", S.gateway ? "-gateway" : "");
#else
        const char *home = getenv("HOME");
        snprintf(S.data_dir, sizeof S.data_dir, "%s/.cache/ryunet-eosbridge%s", home ? home : ".", S.gateway ? "-gateway" : "");
#endif
    }
    make_dirs(S.data_dir);
    for (int i = 0; i < MAX_PORTS; i++) g_listen[i] = BAD_SOCK;

    if (!S.gateway) {
        if (!S.server[0]) usage();
        if (!port_allowed(S.http_port) && S.port_count < MAX_PORTS) S.ports[S.port_count++] = S.http_port;
        int direct = 0;
        if (!S.mode_on) {
            status("CHECKING");
            direct = direct_works();
            if (direct) {
                status("DIRECT");
                /* The server needs no bridge; races still can go through EOS P2P, if it is set up. */
                if (S.debug_gateway[0] || !S.product_id[0] || !S.client_id[0]) return 0;
            }
        }
        /* One client per user: they all log in as the same EOS user (this PC's Device ID), and the
         * gateway would take two of them for one player. */
#ifdef _WIN32
        CreateMutexA(NULL, TRUE, "Local\\RyuNET-bridge");
        if (GetLastError() == ERROR_ALREADY_EXISTS) { status("FAILED busy"); return 7; }
#else
        char lock_path[1100];
        snprintf(lock_path, sizeof lock_path, "%s/client.lock", S.data_dir);
        const int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0600);
        if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) { status("FAILED busy"); return 7; }
#endif
        if (direct) {
            if (!client_bind_udp_only()) { LOG("no local port for EOS P2P"); return 0; }
            if (!eos_start()) { LOG("EOS P2P unavailable: races without it"); return 0; }
            g_eos_on = 1;
            g_peers_only = 1;
            eos_begin();
            loop();
            return 0;
        }
        if (!client_bind_ports()) { status("FAILED ports"); return 6; }
        status("CONNECTING");
        if (S.debug_gateway[0]) {
            if (!dbg_connect(S.debug_gateway)) { status("FAILED debug_gateway"); return 5; }
            g_cstate = C_HELLO;
        } else {
            if (!S.gateway_id[0]) { status("FAILED no_gateway_id"); return 3; }
            if (!eos_start()) { status("FAILED eos_unavailable"); return 3; }
            g_eos_on = 1;
            g_gw = peer_new(S.gateway_id);
            eos_begin();
        }
    } else {
        if (!S.server_ip[0] || !resolve4(S.server_ip, &g_public_be) || !resolve4(S.target, &g_target_be)) usage();
        snprintf(g_ips_path, sizeof g_ips_path, "%s/gateway-ips.txt", S.data_dir);
        vips_load();
        g_srv_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        const struct sockaddr_in a = addr4(g_target_be, 0);
        if (g_srv_udp == BAD_SOCK || bind(g_srv_udp, (const struct sockaddr *)&a, sizeof a) != 0) { LOG("UDP socket failed"); return 6; }
        set_nonblock(g_srv_udp);
        if (S.debug_listen[0] && !dbg_listen(S.debug_listen)) { LOG("debug listen failed"); return 6; }
        if (!S.no_eos) {
            if (!eos_start()) return 3;
            g_eos_on = 1;
            eos_begin();
        }
        char t[16];
        LOG("gateway for %s, to the server on %s (ports %d allowed, NAT %u)", ip_text(g_public_be, t), S.target,
            S.port_count, S.nat_port);
    }
#ifdef _WIN32
    if (g_exit_on_eof) CreateThread(NULL, 0, eof_watch, NULL, 0, NULL);
#endif
    loop();
    return 0;
}
