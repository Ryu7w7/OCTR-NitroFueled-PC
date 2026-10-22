#include "runtime_net_ping.h"

#include <stdatomic.h>
#include <string.h>

/* Per peer address the game sends to. */
typedef struct {
    uint32_t ip;
    uint16_t port;
    int used;
    int capable;         /* it has the ping (TUN_FLAG_PING, or it sent a ping packet) */
    char name[PING_NAME];
    uint64_t last_game_send;
    uint64_t last_ping;
    uint64_t last_answer;
    uint32_t answered_stamp; /* an answer comes direct and through the tunnel: counted once */
    int ms;              /* smoothed round trip; -1 before the first answer */
} PingSlot;

static PingSlot slots[PING_MAX_PEERS];
static char own_name[PING_NAME];
static atomic_flag lock = ATOMIC_FLAG_INIT;

static void take(void) { while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {} }
static void give(void) { atomic_flag_clear_explicit(&lock, memory_order_release); }

void ping_reset(void) {
    take();
    memset(slots, 0, sizeof(slots));
    give();
}

/* A nickname's characters only (letters, digits, _ and -), at most 16. */
static void copy_name(char *out, const char *in, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len && n < PING_NAME - 1; i++) {
        const char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
            out[n++] = c;
    }
    out[n] = '\0';
}

void ping_set_name(const char *name) {
    take();
    copy_name(own_name, name ? name : "", name ? strlen(name) : 0);
    give();
}

/* The slot of this peer; with create, a new one (in place of the longest unused) when none. */
static PingSlot *slot_for(uint32_t ip, uint16_t port, int create) {
    PingSlot *oldest = &slots[0];
    for (int i = 0; i < PING_MAX_PEERS; i++) {
        PingSlot *s = &slots[i];
        if (s->used && s->ip == ip && s->port == port) return s;
        if (!s->used) {
            if (oldest->used) oldest = s;
        } else if (oldest->used && s->last_game_send < oldest->last_game_send) {
            oldest = s;
        }
    }
    if (!create) return NULL;
    memset(oldest, 0, sizeof(*oldest));
    oldest->used = 1;
    oldest->ip = ip;
    oldest->port = port;
    oldest->ms = -1;
    return oldest;
}

void ping_on_capable(uint32_t ip, uint16_t port, uint64_t now_ms) {
    (void)now_ms;
    take();
    slot_for(ip, port, 1)->capable = 1;
    give();
}

void ping_on_game_send(uint32_t ip, uint16_t port, uint64_t now_ms) {
    take();
    slot_for(ip, port, 1)->last_game_send = now_ms;
    give();
}

static size_t packet(uint8_t *out, size_t cap, int kind, uint32_t stamp) {
    const size_t name_len = strlen(own_name);
    const size_t n = PING_MAGIC_LEN + 1 + 4 + 1 + name_len;
    if (cap < n) return 0;
    memcpy(out, PING_MAGIC, PING_MAGIC_LEN);
    out[6] = (uint8_t)kind;
    out[7] = (uint8_t)(stamp >> 24);
    out[8] = (uint8_t)(stamp >> 16);
    out[9] = (uint8_t)(stamp >> 8);
    out[10] = (uint8_t)stamp;
    out[11] = (uint8_t)name_len;
    memcpy(out + 12, own_name, name_len);
    return n;
}

size_t ping_next(uint64_t now_ms, uint8_t *out, size_t cap, uint32_t *ip, uint16_t *port) {
    size_t n = 0;
    take();
    for (int i = 0; i < PING_MAX_PEERS && !n; i++) {
        PingSlot *s = &slots[i];
        if (!s->used || !s->capable || now_ms - s->last_game_send > PING_ACTIVE_MS) continue;
        if (s->last_ping && now_ms - s->last_ping < PING_INTERVAL_MS) continue;
        n = packet(out, cap, 1, (uint32_t)now_ms);
        if (!n) break;
        s->last_ping = now_ms;
        *ip = s->ip;
        *port = s->port;
    }
    give();
    return n;
}

int ping_on_packet(const uint8_t *in, size_t len, uint32_t ip, uint16_t port, uint64_t now_ms,
                   uint8_t *reply, size_t cap, size_t *reply_len) {
    *reply_len = 0;
    if (len < 12 || memcmp(in, PING_MAGIC, PING_MAGIC_LEN) != 0) return 0;
    const int kind = in[6];
    const uint32_t stamp = (uint32_t)in[7] << 24 | (uint32_t)in[8] << 16 | (uint32_t)in[9] << 8 | in[10];
    const size_t name_len = in[11];
    if ((kind != 1 && kind != 2) || len != 12 + name_len || name_len >= PING_NAME) return 1;
    take();
    PingSlot *s = slot_for(ip, port, 1);
    s->capable = 1;
    copy_name(s->name, (const char *)in + 12, name_len);
    if (kind == 1) {
        *reply_len = packet(reply, cap, 2, stamp);
    } else {
        const uint32_t rtt = (uint32_t)now_ms - stamp;
        if (rtt < 5000u && stamp != s->answered_stamp) {
            s->answered_stamp = stamp;
            s->last_answer = now_ms;
            s->ms = s->ms < 0 ? (int)rtt : (s->ms * 3 + (int)rtt + 2) / 4;
        }
    }
    give();
    return 1;
}

int ping_view(uint64_t now_ms, PingPeer *out, int max) {
    int n = 0;
    take();
    for (int i = 0; i < PING_MAX_PEERS && n < max; i++) {
        const PingSlot *s = &slots[i];
        if (!s->used || !s->last_game_send || now_ms - s->last_game_send > PING_ACTIVE_MS) continue;
        PingPeer *p = &out[n++];
        memcpy(p->name, s->name, sizeof(p->name));
        p->ms = !s->capable                                                     ? PING_OLDER_PORT
                : s->last_answer && now_ms - s->last_answer <= PING_FRESH_MS ? s->ms
                                                                                : PING_NO_ANSWER;
    }
    give();
    return n;
}
