#ifndef RUNTIME_NET_H
#define RUNTIME_NET_H

#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif

#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int NativeSockLen;
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int SOCKET;
typedef socklen_t NativeSockLen;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#define ioctlsocket ioctl
#define WSAGetLastError() errno
#define WSAEADDRINUSE EADDRINUSE
#define WSAEINPROGRESS EINPROGRESS
#define WSAEWOULDBLOCK EWOULDBLOCK
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ORBIS_NET_SO_NBIO 0x1200

#define ORBIS_NET_EPOLLIN     0x00000001
#define ORBIS_NET_EPOLLOUT    0x00000002
#define ORBIS_NET_EPOLLERR    0x00000008
#define ORBIS_NET_EPOLLHUP    0x00000010
#define ORBIS_NET_EPOLLDESCID 0x00010000

#define ORBIS_NET_EPOLL_CTL_ADD 1
#define ORBIS_NET_EPOLL_CTL_MOD 2
#define ORBIS_NET_EPOLL_CTL_DEL 3

#pragma pack(push, 1)
typedef struct {
    uint8_t sa_len;
    uint8_t sa_family;
    char sa_data[14];
} OrbisNetSockaddr;

typedef struct {
    uint8_t sin_len;
    uint8_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
} OrbisNetSockaddrIn;

typedef union {
    void *ptr;
    int32_t fd;
    uint32_t u32;
    uint64_t u64;
} OrbisNetEpollData;

typedef struct {
    uint32_t events;
    uint32_t pad;
    uint64_t ident;
    OrbisNetEpollData data;
} OrbisNetEpollEvent;

typedef struct {
    uint32_t size;
    int32_t stun_status;
    int32_t nat_type;
    uint32_t mapped_addr;
} OrbisNetCtlNatInfo;
#pragma pack(pop)

/* Check if online mode is enabled */
int runtime_net_is_online(void);

/* Tracing helper: prints only when BB_NET_TRACE=1 */
void runtime_net_trace(const char *fmt, ...);

/* Net initialization & lifecycle */
int32_t runtime_net_init(void);
int32_t runtime_net_term(void);
int32_t *runtime_net_errno_loc(void);

/* Sockets */
int32_t runtime_net_socket(const char *name, int domain, int type, int protocol);
int32_t runtime_net_socket_close(int s);
int32_t runtime_net_socket_abort(int s, int flags);
int32_t runtime_net_connect(int s, const OrbisNetSockaddr *addr, uint32_t addrlen);
int32_t runtime_net_bind(int s, const OrbisNetSockaddr *addr, uint32_t addrlen);
int32_t runtime_net_listen(int s, int backlog);
int32_t runtime_net_accept(int s, OrbisNetSockaddr *addr, uint32_t *addrlen);
int32_t runtime_net_send(int s, const void *msg, size_t len, int flags);
int32_t runtime_net_sendto(int s, const void *msg, size_t len, int flags, const OrbisNetSockaddr *to, uint32_t tolen);
int32_t runtime_net_recv(int s, void *buf, size_t len, int flags);
int32_t runtime_net_recvfrom(int s, void *buf, size_t len, int flags, OrbisNetSockaddr *from, uint32_t *fromlen);
int32_t runtime_net_setsockopt(int s, int level, int optname, const void *optval, uint32_t optlen);
int32_t runtime_net_getsockopt(int s, int level, int optname, void *optval, uint32_t *optlen);
int32_t runtime_net_getsockname(int s, OrbisNetSockaddr *name, uint32_t *namelen);
int32_t runtime_net_shutdown(int s, int how);

/* Epoll */
int32_t runtime_net_epoll_create(const char *name, int flags);
int32_t runtime_net_epoll_destroy(int eid);
int32_t runtime_net_epoll_control(int eid, int op, int s, OrbisNetEpollEvent *event);
int32_t runtime_net_epoll_wait(int eid, OrbisNetEpollEvent *events, int maxevents, int timeout_usec);
int32_t runtime_net_epoll_abort(int eid, int flags);

/* Resolver */
int32_t runtime_net_resolver_create(const char *name, int pool, int flags);
int32_t runtime_net_resolver_destroy(int rid);
int32_t runtime_net_resolver_start_ntoa(int rid, const char *hostname, struct in_addr *addr, int timeout_usec, int retry, int flags);
int32_t runtime_net_resolver_start_aton(int rid, struct in_addr addr, char *hostname, size_t hostlen, int timeout_usec, int retry, int flags);
int32_t runtime_net_resolver_abort(int rid, int flags);

/* NetCtl */
int32_t runtime_netctl_init(void);
int32_t runtime_netctl_term(void);
int32_t runtime_netctl_state(int32_t *state);
int32_t runtime_netctl_info(int code, void *info);
int32_t runtime_netctl_nat(OrbisNetCtlNatInfo *info);
int32_t runtime_netctl_register(void *cb, void *arg, int32_t *cid);
int32_t runtime_netctl_unregister(int32_t cid);
int32_t runtime_netctl_check(void);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_NET_H */
