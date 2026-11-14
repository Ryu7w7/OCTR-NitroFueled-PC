#include "runtime_net_eos.h"

#include <stdatomic.h>
#include <string.h>

typedef struct {
    int used;
    uint32_t ip;
    uint16_t port;
    char id[EOSP_ID_MAX];   /* "" until it announced itself */
    uint64_t capable;       /* when its tunnelled packets last carried TUN_FLAG_EOS */
    uint64_t announced;     /* when this PC last announced itself to it */
    uint64_t last_rx;       /* its last datagram through EOS */
    uint64_t last_use;
} EospPeer;

static EospPeer peers[EOSP_MAX_PEERS];
static char self[EOSP_ID_MAX];
static uint64_t query_at;
static atomic_flag lock = ATOMIC_FLAG_INIT;

/* ms since t; 0 when another thread stamped t after now was read */
static uint64_t ago(uint64_t now, uint64_t t) { return now > t ? now - t : 0; }

static void take(void) { while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {} }
static void give(void) { atomic_flag_clear_explicit(&lock, memory_order_release); }

/* An id is what EOS prints for a product user id: letters and digits. */
static int id_ok(const char *id, size_t len) {
    if (!len || len >= EOSP_ID_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return 0;
    }
    return 1;
}

void eosp_reset(void) {
    take();
    memset(peers, 0, sizeof peers);
    memset(self, 0, sizeof self);
    query_at = 0;
    give();
}

void eosp_set_self(const char *id) {
    const size_t len = strlen(id);
    if (!id_ok(id, len)) return;
    take();
    memcpy(self, id, len + 1);
    give();
}

int eosp_self(char *id) {
    take();
    const int known = self[0] != 0;
    if (known && id) memcpy(id, self, EOSP_ID_MAX);
    give();
    return known;
}

int eosp_query_due(uint64_t now_ms) {
    take();
    const int due = !self[0] && (!query_at || ago(now_ms, query_at) >= EOSP_QUERY_MS);
    if (due) query_at = now_ms;
    give();
    return due;
}

/* The peer's entry, made (reusing the idlest one) when missing. Holds the lock. */
static EospPeer *peer_for(uint32_t ip, uint16_t port, uint64_t now, int create) {
    EospPeer *idlest = NULL;
    for (int i = 0; i < EOSP_MAX_PEERS; i++) {
        EospPeer *p = &peers[i];
        if (p->used && p->ip == ip && p->port == port) return p;
        if (!idlest || !p->used || (idlest->used && p->last_use < idlest->last_use)) idlest = p;
    }
    if (!create) return NULL;
    memset(idlest, 0, sizeof *idlest);
    idlest->used = 1;
    idlest->ip = ip;
    idlest->port = port;
    idlest->last_use = now;
    return idlest;
}

void eosp_on_capable(uint32_t ip, uint16_t port, uint64_t now_ms) {
    take();
    EospPeer *p = peer_for(ip, port, now_ms, 1);
    p->capable = now_ms;
    p->last_use = now_ms;
    give();
}

static size_t put_id(uint8_t *out, size_t cap, char kind, const char *id) {
    const size_t len = strlen(id);
    if (cap < EOSP_MAGIC_LEN + 2 + len || len > 255) return 0;
    memcpy(out, EOSP_MAGIC, EOSP_MAGIC_LEN);
    out[EOSP_MAGIC_LEN] = (uint8_t)kind;
    out[EOSP_MAGIC_LEN + 1] = (uint8_t)len;
    memcpy(out + EOSP_MAGIC_LEN + 2, id, len);
    return EOSP_MAGIC_LEN + 2 + len;
}

/* kind and id of a message; its length so far (0: not one). */
static size_t get_id(const uint8_t *data, size_t len, char *kind, char *id) {
    if (len < EOSP_MAGIC_LEN + 2 || memcmp(data, EOSP_MAGIC, EOSP_MAGIC_LEN) != 0) return 0;
    const size_t n = data[EOSP_MAGIC_LEN + 1];
    if (len < EOSP_MAGIC_LEN + 2 + n || !id_ok((const char *)data + EOSP_MAGIC_LEN + 2, n)) return 0;
    *kind = (char)data[EOSP_MAGIC_LEN];
    memcpy(id, data + EOSP_MAGIC_LEN + 2, n);
    id[n] = '\0';
    return EOSP_MAGIC_LEN + 2 + n;
}

size_t eosp_announce(uint32_t ip, uint16_t port, uint64_t now_ms, uint8_t *out, size_t cap) {
    take();
    EospPeer *p = peer_for(ip, port, now_ms, 0);
    size_t n = 0;
    /* To a peer with EOS, until its datagrams come through EOS (then it knows this PC's id). */
    const int reached = p && p->last_rx && ago(now_ms, p->last_rx) < EOSP_FRESH_MS;
    if (p && self[0] && p->capable && !reached && (!p->announced || ago(now_ms, p->announced) >= EOSP_ANNOUNCE_MS)) {
        n = put_id(out, cap, 'A', self);
        if (n) p->announced = now_ms;
    }
    give();
    return n;
}

int eosp_take_announce(const uint8_t *data, size_t len, uint32_t ip, uint16_t port, uint64_t now_ms,
                       char *id, int *learned) {
    char kind;
    *learned = 0;
    if (!get_id(data, len, &kind, id) || kind != 'A') return 0;
    take();
    EospPeer *p = peer_for(ip, port, now_ms, 1);
    if (strcmp(p->id, id) != 0) {
        memcpy(p->id, id, strlen(id) + 1);
        *learned = 1;
    }
    p->capable = now_ms; /* it has EOS: it can be answered with ours */
    p->last_use = now_ms;
    give();
    return 1;
}

int eosp_peer_id(uint32_t ip, uint16_t port, char *id) {
    take();
    EospPeer *p = peer_for(ip, port, 0, 0);
    const int known = p && p->id[0];
    if (known) memcpy(id, p->id, EOSP_ID_MAX);
    give();
    return known;
}

int eosp_peer_addr(const char *id, uint32_t *ip, uint16_t *port) {
    take();
    int found = 0;
    for (int i = 0; i < EOSP_MAX_PEERS && !found; i++) {
        if (!peers[i].used || strcmp(peers[i].id, id) != 0) continue;
        *ip = peers[i].ip;
        *port = peers[i].port;
        found = 1;
    }
    give();
    return found;
}

void eosp_on_rx(uint32_t ip, uint16_t port, uint64_t now_ms) {
    take();
    EospPeer *p = peer_for(ip, port, now_ms, 0);
    if (p) {
        p->last_rx = now_ms;
        p->last_use = now_ms;
    }
    give();
}

int eosp_reached(uint32_t ip, uint16_t port, uint64_t now_ms) {
    take();
    EospPeer *p = peer_for(ip, port, now_ms, 0);
    const int reached = p && p->last_rx && ago(now_ms, p->last_rx) < EOSP_FRESH_MS;
    give();
    return reached;
}

size_t eosp_msg_query(uint8_t *out, size_t cap) {
    if (cap < EOSP_MAGIC_LEN + 1) return 0;
    memcpy(out, EOSP_MAGIC, EOSP_MAGIC_LEN);
    out[EOSP_MAGIC_LEN] = 'Q';
    return EOSP_MAGIC_LEN + 1;
}

size_t eosp_msg_known(const char *id, uint8_t *out, size_t cap) { return put_id(out, cap, 'K', id); }

size_t eosp_msg_send(const char *id, const void *payload, size_t len, uint8_t *out, size_t cap) {
    const size_t head = put_id(out, cap, 'S', id);
    if (!head || head + len > cap) return 0;
    memcpy(out + head, payload, len);
    return head + len;
}

int eosp_parse_bridge(const uint8_t *data, size_t len, char *id, size_t *payload_at) {
    char kind;
    const size_t head = get_id(data, len, &kind, id);
    if (!head || (kind != 'I' && kind != 'R')) return 0;
    if (payload_at) *payload_at = head;
    return kind;
}
